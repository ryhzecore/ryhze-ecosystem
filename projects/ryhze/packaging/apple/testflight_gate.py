"""Existing-only TestFlight signing and artifact gates; never creates Apple resources."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path, PurePosixPath
import plistlib
import re
import stat
import subprocess
import tempfile
import zipfile
from xml.parsers.expat import ExpatError

from verify_package import source_version, verify_package


BUNDLE = "com.ryhze.ryhze"
TEAM = "B45M4C7CDW"
APP = "6815007396"
PREVIOUS_BUILD = "36"
UPLOAD_BUILD = "37"


def check_latest(value):
    if (not isinstance(value, dict)
            or not isinstance(value.get("buildId"), str) or not value["buildId"]
            or not isinstance(value.get("version"), str) or not value["version"]
            or value.get("buildNumber") != PREVIOUS_BUILD):
        raise ValueError("Live TestFlight checkpoint is not build 36; stop and reconcile the upload number.")
    return {"app": APP, "previous_build": PREVIOUS_BUILD, "upload_build": UPLOAD_BUILD,
            "previous_build_id": value["buildId"], "previous_version": value["version"]}


def live_latest():
    command = ["app-store-connect", "get-latest-testflight-build-number", APP,
               "--platform", "IOS", "--all-versions", "--include-version", "--json",
               "--log-stream", "stderr", "--no-color", "--disable-jwt-cache"]
    result = subprocess.run(command, capture_output=True, check=False, timeout=90)
    if result.returncode:
        raise ValueError("Live TestFlight lookup failed; verify the existing integration and Codemagic CLI >= 0.67.0. No upload is permitted.")
    return check_latest(json.loads(result.stdout))


SIGNING_REASONS = frozenset({
    "profile_malformed", "team_mismatch", "platform_mismatch",
    "profile_expiry_malformed", "profile_expired", "entitlements_malformed",
    "entitlement_team_mismatch", "not_app_store_distribution",
    "app_identifier_prefix_malformed", "profile_uuid_malformed",
    "app_identifier_mismatch", "keychain_group_mismatch", "certificates_missing",
    "profile_decode_command_failed", "profile_plist_invalid",
    "identity_inspection_failed", "profile_directory_unsafe", "signing_io_failed",
    "no_qualifying_signing_pair", "signing_inspection_failed", "diagnostics_write_failed",
})


class SigningGateError(ValueError):
    """Only fixed reason codes cross the public signing diagnostic boundary."""

    def __init__(self, reason):
        self.reason = reason if isinstance(reason, str) and reason in SIGNING_REASONS else "signing_inspection_failed"
        super().__init__("Existing signing inspection stopped: " + self.reason + ".")


def platform_shape(platforms):
    # Apple's current iOS App Store profiles can also declare xrOS/visionOS.
    # Unknown values remain blocked until reviewed. These fixed labels are safe
    # metadata; never return arbitrary profile fields in public diagnostics.
    if (not isinstance(platforms, list) or not platforms
            or not all(isinstance(value, str) for value in platforms)
            or len(set(platforms)) != len(platforms)
            or "iOS" not in platforms
            or not set(platforms) <= {"iOS", "xrOS", "visionOS"}):
        return "unsupported"
    return "ios_only" if len(platforms) == 1 else "ios_with_companions"


def profile_details(profile, now=None):
    now = now or datetime.now(timezone.utc)
    if not isinstance(profile, dict):
        raise SigningGateError("profile_malformed")
    expires = profile.get("ExpirationDate")
    if isinstance(expires, datetime) and expires.tzinfo is None:
        expires = expires.replace(tzinfo=timezone.utc)
    entitlements = profile.get("Entitlements", {})
    prefixes = profile.get("ApplicationIdentifierPrefix", [])
    uuid = profile.get("UUID", "")
    if profile.get("TeamIdentifier") != [TEAM]:
        raise SigningGateError("team_mismatch")
    if platform_shape(profile.get("Platform")) == "unsupported":
        raise SigningGateError("platform_mismatch")
    if not isinstance(expires, datetime):
        raise SigningGateError("profile_expiry_malformed")
    if expires <= now:
        raise SigningGateError("profile_expired")
    if not isinstance(entitlements, dict):
        raise SigningGateError("entitlements_malformed")
    if entitlements.get("com.apple.developer.team-identifier") != TEAM:
        raise SigningGateError("entitlement_team_mismatch")
    if (entitlements.get("get-task-allow") is not False
            or "ProvisionedDevices" in profile or profile.get("ProvisionsAllDevices", False)):
        raise SigningGateError("not_app_store_distribution")
    if (not isinstance(prefixes, list) or len(prefixes) != 1
            or not isinstance(prefixes[0], str) or not re.fullmatch(r"[A-Z0-9]{10}", prefixes[0])):
        raise SigningGateError("app_identifier_prefix_malformed")
    if (not isinstance(uuid, str)
            or not re.fullmatch(r"[A-Fa-f0-9]{8}(?:-[A-Fa-f0-9]{4}){3}-[A-Fa-f0-9]{12}", uuid)):
        raise SigningGateError("profile_uuid_malformed")
    group = prefixes[0] + "." + BUNDLE
    if entitlements.get("application-identifier") != group:
        raise SigningGateError("app_identifier_mismatch")
    groups = entitlements.get("keychain-access-groups", [])
    if (not isinstance(groups, list) or not all(isinstance(value, str) for value in groups)
            or not ({group, prefixes[0] + ".*"} & set(groups))):
        raise SigningGateError("keychain_group_mismatch")
    certificates = profile.get("DeveloperCertificates", [])
    if (not isinstance(certificates, list) or not certificates
            or not all(isinstance(value, bytes) and value for value in certificates)):
        raise SigningGateError("certificates_missing")
    return {"profile_uuid": uuid, "expires": expires.isoformat(), "keychain_group": group,
            "certificates": {hashlib.sha1(value).hexdigest().upper() for value in certificates}}


def inspect_identities(output):
    # Only complete numbered lines from security find-identity -v -p codesigning
    # qualify. Error suffixes, unnumbered diagnostics and development identities
    # cannot establish a valid distribution identity with an accessible private key.
    identities = set()
    metadata = {"scope": "default_keychain_search_list", "numbered_lines": 0,
                "parsed_lines": 0, "unparsed_lines": 0,
                "non_distribution_lines": 0, "valid_distribution_identities": 0}
    for line in output.split("\n"):
        line = line.removesuffix("\r")  # Accept normal CRLF, not embedded CR.
        if not re.match(r"^[ \t]*[0-9]+\)", line):
            continue
        metadata["numbered_lines"] += 1
        if any((ord(value) < 32 and value != "\t")
               or value in "\x7f\x85\u2028\u2029" for value in line):
            metadata["unparsed_lines"] += 1
            continue
        match = re.fullmatch(r'[ \t]*[0-9]+\)[ \t]+([A-Fa-f0-9]{40})[ \t]+"([^"\r\n]+)"[ \t]*', line)
        if match is None:
            metadata["unparsed_lines"] += 1
            continue
        metadata["parsed_lines"] += 1
        fingerprint, name = match.groups()
        if name.startswith(("Apple Distribution:", "iPhone Distribution:")):
            identities.add(fingerprint.upper())
        else:
            metadata["non_distribution_lines"] += 1
    metadata["valid_distribution_identities"] = len(identities)
    return identities, metadata


def valid_identities(output):
    return inspect_identities(output)[0]


def export_options(selection):
    return {"method": "app-store-connect", "destination": "export",
            "signingStyle": "manual", "signingCertificate": selection["certificate"],
            "teamID": TEAM, "provisioningProfiles": {BUNDLE: selection["profile_uuid"]},
            "testFlightInternalTestingOnly": True, "manageAppVersionAndBuildNumber": False}


STDERR_CLASSIFICATION_LIMIT = 8192
SIGNED_STAGES = frozenset({
    "selection_input", "package_structure", "inspection_setup", "safe_extraction",
    "app_selection", "signature_strict", "signature_requirement",
    "certificate_extract", "certificate_match", "embedded_profile_decode",
    "embedded_profile_match", "entitlements_display", "entitlements_match", "ipa_digest",
})
SIGNED_REASONS = SIGNING_REASONS | frozenset({
    "command_failed", "command_timeout", "command_unavailable", "rejected_data",
    "io_failed", "missing_certificate", "certificate_mismatch", "profile_mismatch",
    "malformed_entitlements", "malformed_profile", "entitlements_mismatch", "inspection_incomplete",
})


def classify_stderr(stderr):
    """Fixed hints from at most 8 KiB; never return payloads or parsed substrings."""
    data = stderr if isinstance(stderr, bytes) else b""
    text = data[:STDERR_CLASSIFICATION_LIMIT].lower()
    patterns = (
        ("invalid_option", (b"unrecognized option", b"unknown option", b"invalid option", b"usage: codesign")),
        ("missing_target_or_file", (b"no such file or directory", b"cannot find code object")),
        ("requirement_not_satisfied", (b"does not satisfy", b"failed to satisfy")),
        ("invalid_resources", (b"a sealed resource is missing or invalid", b"file modified", b"resource envelope is obsolete")),
        ("invalid_signature", (b"code object is not signed", b"invalid signature")),
        ("trust_or_certificate_diagnostic", (b"unable to build chain", b"certificate has expired", b"cssmerr_tp_not_trusted")),
        ("permission_diagnostic", (b"permission denied", b"operation not permitted")),
    )
    category = next((name for name, matches in patterns if any(value in text for value in matches)),
                    "unclassified" if data else "empty")
    return {"error_class": category, "stderr_truncated": len(data) > STDERR_CLASSIFICATION_LIMIT}


class SignedCommandError(ValueError):
    def __init__(self, reason, *, return_code=None, stderr=None):
        self.reason = reason
        self.metadata = {"return_code": return_code, **classify_stderr(stderr)}
        super().__init__(reason)


class SignedIpaInspectionError(ValueError):
    """A report constructed solely from fixed stages/reasons and safe metadata."""
    def __init__(self, report):
        self.report = report
        super().__init__("Exported IPA inspection stopped at " + report["failed_stage"] + ".")


class SignedPolicyError(ValueError):
    def __init__(self, reason):
        self.reason = reason if reason in SIGNED_REASONS else "rejected_data"
        super().__init__(self.reason)


class SignedInspection:
    def __init__(self, diagnostics=None):
        self.checks = []
        self.diagnostics = diagnostics
        self.started = False

    def write(self, report):
        if self.diagnostics is None:
            return
        # Preserve old evidence. Subsequent atomic replacements update only this
        # invocation's file. A killed outer phase still leaves its current stage.
        try:
            if not self.started:
                with self.diagnostics.open("x") as stream:
                    json.dump(report, stream, indent=2)
                    stream.write("\n")
                self.started = True
            else:
                temporary = self.diagnostics.with_name(self.diagnostics.name + ".tmp")
                with temporary.open("x") as stream:
                    json.dump(report, stream, indent=2)
                    stream.write("\n")
                temporary.replace(self.diagnostics)
        except OSError:
            raise SignedIpaInspectionError({
                "schema": 1, "operation": "signed-ipa", "status": "failed",
                "failed_stage": report.get("current_stage", report.get("failed_stage", "inspection_setup")),
                "reason": "diagnostics_write_failed", "checks": list(self.checks),
            }) from None

    def step(self, stage, operation):
        if stage not in SIGNED_STAGES:
            raise ValueError("Unknown signed inspection stage")
        self.write({"schema": 1, "operation": "signed-ipa", "status": "running",
                    "current_stage": stage, "checks": list(self.checks)})
        metadata = {}
        try:
            value = operation()
        except SignedCommandError as error:
            reason, metadata = error.reason, error.metadata
        except SigningGateError as error:
            reason = error.reason
        except SignedPolicyError as error:
            reason = error.reason
        except FileNotFoundError:
            reason = "missing_certificate" if stage == "certificate_match" else "io_failed"
        except OSError:
            reason = "io_failed"
        except (ValueError, TypeError, KeyError, AttributeError, OverflowError,
                plistlib.InvalidFileException, ExpatError, zipfile.BadZipFile):
            reason = {"certificate_match": "certificate_mismatch",
                      "embedded_profile_decode": "malformed_profile",
                      "embedded_profile_match": "profile_mismatch",
                      "entitlements_match": "malformed_entitlements"}.get(stage, "rejected_data")
        else:
            self.checks.append({"stage": stage, "status": "passed"})
            return value
        reason = reason if reason in SIGNED_REASONS else "rejected_data"
        self.checks.append({"stage": stage, "status": "failed", "reason": reason, **metadata})
        report = {"schema": 1, "operation": "signed-ipa", "status": "failed",
                  "failed_stage": stage, "reason": reason, "checks": list(self.checks)}
        self.write(report)
        raise SignedIpaInspectionError(report) from None

    def complete(self):
        report = {"schema": 1, "operation": "signed-ipa", "status": "passed",
                  "checks": list(self.checks)}
        self.write(report)
        return report


def command_bytes(command, *, inspect_failure=False):
    try:
        result = subprocess.run(command, capture_output=True, check=False, timeout=60)
    except subprocess.TimeoutExpired as error:
        if inspect_failure:
            raise SignedCommandError("command_timeout", stderr=error.stderr) from None
        raise
    except OSError:
        if inspect_failure:
            raise SignedCommandError("command_unavailable") from None
        raise
    if result.returncode:
        if inspect_failure:
            raise SignedCommandError("command_failed", return_code=result.returncode,
                                     stderr=result.stderr) from None
        # Never echo credential-tool payloads, profile contents or private paths.
        raise ValueError("Existing Apple signing material could not be inspected; review authorized Codemagic signing resources.")
    return result.stdout


def decode_profile(path, *, inspect_failure=False):
    try:
        command = ["security", "cms", "-D", "-i", str(path)]
        data = (command_bytes(command, inspect_failure=True) if inspect_failure
                else command_bytes(command))
    except SignedCommandError:
        raise
    except (ValueError, OSError, subprocess.TimeoutExpired):
        raise SigningGateError("profile_decode_command_failed") from None
    try:
        return plistlib.loads(data)
    except (ValueError, TypeError, OverflowError, plistlib.InvalidFileException, ExpatError):
        raise SigningGateError("profile_plist_invalid") from None


def prepare_signing(output, diagnostics=None):
    # Public diagnostics contain only fixed classifications, counts and booleans.
    # Names, paths, UUIDs, fingerprints, entitlements, dates and tool payloads stay
    # out. Detailed selection is written only to the existing private directory.
    report = {"schema": 1, "status": "blocked", "reason": "inspection_incomplete",
              "bundle": BUNDLE, "team": TEAM, "locations": [], "profiles": [],
              "eligible_profiles": 0, "matching_pairs": 0}
    try:
        try:
            identities, metadata = inspect_identities(command_bytes(
                ["security", "find-identity", "-v", "-p", "codesigning"]).decode("utf-8"))
        except (ValueError, OSError, UnicodeError, subprocess.TimeoutExpired):
            raise SigningGateError("identity_inspection_failed") from None
        report["identities"] = metadata
        selections = []
        for location, relative in (
                ("mobiledevice", "Library/MobileDevice/Provisioning Profiles"),
                ("xcode_userdata", "Library/Developer/Xcode/UserData/Provisioning Profiles")):
            directory = Path.home() / relative
            entry = {"location": location, "state": "absent", "candidate_count": 0}
            report["locations"].append(entry)
            if directory.is_symlink():
                entry["state"] = "symlink_rejected"
                raise SigningGateError("profile_directory_unsafe")
            if not directory.exists():
                continue
            if not directory.is_dir():
                entry["state"] = "not_directory"
                raise SigningGateError("profile_directory_unsafe")
            entry["state"] = "directory"
            paths = sorted(directory.glob("*.mobileprovision"))
            entry["candidate_count"] = len(paths)
            for index, path in enumerate(paths):
                item = {"location": location, "index": index, "status": "rejected"}
                report["profiles"].append(item)
                if path.is_symlink() or not path.is_file():
                    item["reason"] = "profile_file_unsafe"
                    continue
                try:
                    decoded = decode_profile(path)
                    item["platform_shape"] = platform_shape(
                        decoded.get("Platform") if isinstance(decoded, dict) else None)
                    details = profile_details(decoded)
                except SigningGateError as error:
                    item["reason"] = error.reason
                    continue
                except (ValueError, OSError, TypeError, OverflowError,
                        subprocess.TimeoutExpired, plistlib.InvalidFileException):
                    item["reason"] = "profile_inspection_failed"
                    continue
                report["eligible_profiles"] += 1
                matches = details.pop("certificates") & identities
                item["matching_identity_count"] = len(matches)
                if not matches:
                    item["reason"] = "no_matching_valid_distribution_identity"
                    continue
                item.update(status="matched", reason="matched")
                for certificate in matches:
                    selections.append({**details, "certificate": certificate})
        report["matching_pairs"] = len(selections)
        if not selections:
            raise SigningGateError("no_qualifying_signing_pair")
        # Prefer the longest existing validity; deterministic tie-breaking, no
        # changes to profiles, private keys, keychains, grants or Apple resources.
        selection = max(selections, key=lambda item: (item["expires"], item["profile_uuid"], item["certificate"]))
        output.mkdir(parents=True, exist_ok=False)
        (output / "selection.json").write_text(json.dumps(selection, indent=2) + "\n")
        (output / "export-options.plist").write_bytes(plistlib.dumps(export_options(selection)))
        report.update(status="selected", reason="matching_existing_pair_selected")
        return {"bundle": BUNDLE, "team": TEAM, "profile_expires": selection["expires"],
                "signing": "existing App Store profile and matching valid private-key identity",
                "distribution": "internal TestFlight only"}
    except SigningGateError as error:
        report["reason"] = error.reason
        raise
    except OSError:
        report["reason"] = "signing_io_failed"
        raise SigningGateError("signing_io_failed") from None
    finally:
        if diagnostics is not None:
            # Refuse overwriting previous evidence. The caller supplies a path in
            # this run's fresh public evidence directory; never a resource path.
            try:
                with diagnostics.open("x") as stream:
                    json.dump(report, stream, indent=2)
                    stream.write("\n")
            except OSError:
                raise SigningGateError("diagnostics_write_failed") from None


def extract_checked(archive, destination):
    with zipfile.ZipFile(archive) as package:
        entries = package.infolist()
        if len(entries) > 100000 or sum(entry.file_size for entry in entries) > 2 * 1024**3:
            raise ValueError("Exported IPA exceeds inspection limits.")
        seen = set()
        for entry in entries:
            path = PurePosixPath(entry.filename)
            parts = entry.filename.rstrip("/").split("/")
            mode = entry.external_attr >> 16
            if (path.is_absolute() or not parts or any(part in {"", ".", ".."} for part in parts)
                    or "\\" in entry.filename or "\x00" in entry.filename
                    or any(ord(character) < 32 for character in entry.filename)
                    or stat.S_ISLNK(mode)
                    or (stat.S_IFMT(mode) not in {0, stat.S_IFREG, stat.S_IFDIR})
                    or entry.filename.casefold() in seen):
                raise ValueError("Unsafe path or file in exported IPA.")
            seen.add(entry.filename.casefold())
        for entry in entries:
            path = destination.joinpath(*PurePosixPath(entry.filename).parts)
            if entry.is_dir():
                path.mkdir(parents=True, exist_ok=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                with path.open("xb") as output, package.open(entry) as source:
                    while chunk := source.read(1024 * 1024):
                        output.write(chunk)
                path.chmod((entry.external_attr >> 16) & 0o777 or 0o644)


def check_entitlements(entitlements, selection):
    if (entitlements.get("application-identifier") != selection["keychain_group"]
            or entitlements.get("com.apple.developer.team-identifier") != TEAM
            or entitlements.get("get-task-allow", False) is not False
            or entitlements.get("keychain-access-groups") != [selection["keychain_group"]]):
        raise ValueError("Exported IPA signing entitlements do not match the authorized application.")


def verify_signed(archive, selection, diagnostics=None):
    inspection = SignedInspection(diagnostics)

    def inspect_package():
        version, _ = source_version()
        return verify_package(archive, version=version, build=UPLOAD_BUILD)

    package = inspection.step("package_structure", inspect_package)
    temporary = inspection.step("inspection_setup", lambda: tempfile.TemporaryDirectory(prefix="ryhze-ipa-inspect-"))
    with temporary as directory:
        root = Path(directory)
        inspection.step("safe_extraction", lambda: extract_checked(archive, root))

        def select_app():
            apps = list((root / "Payload").glob("*.app"))
            if len(apps) != 1:
                raise ValueError("Expected exactly one exported application.")
            return apps[0]

        app = inspection.step("app_selection", select_app)
        requirement = f'anchor apple generic and identifier "{BUNDLE}" and certificate leaf[subject.OU] = "{TEAM}"'
        inspection.step("signature_strict", lambda: command_bytes(
            ["codesign", "--verify", "--deep", "--strict", str(app)], inspect_failure=True))
        # Apple's requirement reader treats an argument without a leading '=' as
        # a filename. Preserve the exact policy expression, supply it as text.
        inspection.step("signature_requirement", lambda: command_bytes(
            ["codesign", "--verify", "-R", "=" + requirement, str(app)], inspect_failure=True))
        certificate_prefix = root / "signer-"
        # Optional long-option values must be attached with '='. A separate
        # prefix becomes another code-object operand and changes the output path.
        inspection.step("certificate_extract", lambda: command_bytes(
            ["codesign", "--display", f"--extract-certificates={certificate_prefix}", str(app)],
            inspect_failure=True))

        def match_certificate():
            signer = hashlib.sha1((root / "signer-0").read_bytes()).hexdigest().upper()
            if signer != selection["certificate"]:
                raise SignedPolicyError("certificate_mismatch")

        inspection.step("certificate_match", match_certificate)
        profile = inspection.step("embedded_profile_decode", lambda: decode_profile(
            app / "embedded.mobileprovision", inspect_failure=True))

        def match_profile():
            details = profile_details(profile)
            if (details["profile_uuid"] != selection["profile_uuid"]
                    or selection["certificate"] not in details["certificates"]
                    or details["keychain_group"] != selection["keychain_group"]):
                raise SignedPolicyError("profile_mismatch")

        inspection.step("embedded_profile_match", match_profile)
        data = inspection.step("entitlements_display", lambda: command_bytes(
            ["codesign", "--display", "--entitlements", "-", "--xml", str(app)], inspect_failure=True))

        def match_entitlements():
            try:
                entitlements = plistlib.loads(data)
                if not isinstance(entitlements, dict):
                    raise ValueError
            except (ValueError, TypeError, OverflowError, plistlib.InvalidFileException, ExpatError):
                raise SignedPolicyError("malformed_entitlements") from None
            try:
                check_entitlements(entitlements, selection)
            except ValueError:
                raise SignedPolicyError("entitlements_mismatch") from None

        inspection.step("entitlements_match", match_entitlements)
    digest = inspection.step("ipa_digest", lambda: hashlib.sha256(Path(archive).read_bytes()).hexdigest())
    return {**package, "team": TEAM, "ipa_sha256": digest, "inspection": inspection.complete(),
            "signing": "exported signature, profile and Keychain entitlements verified",
            "limits": "Apple processing, tester assignment and physical devices are not verified"}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("latest-live")
    latest = commands.add_parser("latest")
    latest.add_argument("json_path", type=Path)
    signing = commands.add_parser("signing")
    signing.add_argument("output", type=Path)
    signing.add_argument("--diagnostics", type=Path)
    signed = commands.add_parser("signed-ipa")
    signed.add_argument("ipa", type=Path)
    signed.add_argument("selection", type=Path)
    signed.add_argument("--diagnostics", type=Path)
    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "latest-live":
            result = live_latest()
        elif arguments.command == "latest":
            result = check_latest(json.loads(arguments.json_path.read_text()))
        elif arguments.command == "signing":
            result = prepare_signing(arguments.output, arguments.diagnostics)
        else:
            # Invalid selection input must also yield a safe structured failure,
            # without copying JSON parse snippets or signing paths to the log.
            selection = SignedInspection().step("selection_input", lambda: json.loads(arguments.selection.read_text()))
            result = verify_signed(arguments.ipa, selection, arguments.diagnostics)
        print(json.dumps(result, indent=2))
    except SignedIpaInspectionError as error:
        print(json.dumps(error.report, indent=2))
        parser.exit(2)
    except (ValueError, OSError, subprocess.TimeoutExpired, plistlib.InvalidFileException, zipfile.BadZipFile) as error:
        # Errors produced here are safe policy messages; system exception text may
        # contain private signing paths, so do not expose it.
        message = str(error) if type(error) in (ValueError, SigningGateError) else "Signing evidence could not be read or validated; stop and review the existing resources."
        parser.exit(2, message + "\n")


if __name__ == "__main__":
    main()

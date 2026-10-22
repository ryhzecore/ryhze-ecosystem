"""Scope existing signing to Runner and verify reported Xcode settings, without building."""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import stat
import sys

from testflight_gate import BUNDLE, TEAM, UPLOAD_BUILD


PRIVATE_ROOT = Path(__file__).resolve().parents[2] / "build/apple-testflight-private"
MAX_SELECTION_BYTES = 64 * 1024
MAX_SETTINGS_BYTES = 16 * 1024 * 1024
MAX_TARGETS = 256
CONDITION = "[config=Release][sdk=iphoneos*]"
REASONS = frozenset({
    "private_path_invalid", "input_invalid", "selection_invalid", "selection_expired",
    "settings_invalid", "runner_missing_or_ambiguous", "runner_settings_mismatch",
    "dependency_profile_present", "signing_io_failed", "signing_validation_failed",
})


class RunnerSigningError(ValueError):
    def __init__(self, reason):
        self.reason = reason if reason in REASONS else "signing_validation_failed"
        super().__init__(self.reason)


def private_path(path, private_root=None):
    """Reject paths outside the private run directory and symlink traversal within it."""
    root = Path(os.path.abspath(private_root or PRIVATE_ROOT))
    path = Path(os.path.abspath(path))
    try:
        relative = path.relative_to(root)
        root_stat = root.lstat()
        if (not relative.parts or not stat.S_ISDIR(root_stat.st_mode)
                or root_stat.st_mode & 0o077):
            raise RunnerSigningError("private_path_invalid")
        current = root
        for component in relative.parts[:-1]:
            current /= component
            if not stat.S_ISDIR(current.lstat().st_mode):
                raise RunnerSigningError("private_path_invalid")
        if path.is_symlink():
            raise RunnerSigningError("private_path_invalid")
    except (OSError, ValueError):
        raise RunnerSigningError("private_path_invalid") from None
    return path


def _unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise RunnerSigningError("input_invalid")
        value[key] = item
    return value


def read_private_json(path, limit, private_root=None):
    path = private_path(path, private_root)
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise RunnerSigningError("input_invalid")
        stream = os.fdopen(descriptor, "rb")
        descriptor = None
        with stream:
            data = stream.read(limit + 1)
    finally:
        if descriptor is not None:
            os.close(descriptor)
    if len(data) > limit:
        raise RunnerSigningError("input_invalid")
    try:
        return json.loads(data, object_pairs_hook=_unique_object)
    except (ValueError, UnicodeError, RecursionError):
        raise RunnerSigningError("input_invalid") from None


def validated_selection(value, now=None):
    # This consumes the existing signing preflight's private selection, not an
    # arbitrary profile. Cryptographic/profile/private-key validation stays there.
    keys = {"certificate", "profile_uuid", "expires", "keychain_group"}
    if not isinstance(value, dict) or set(value) != keys:
        raise RunnerSigningError("selection_invalid")
    if (not all(isinstance(value[key], str) for key in keys)
            or not re.fullmatch(r"[A-F0-9]{40}", value["certificate"])
            or not re.fullmatch(r"[A-Fa-f0-9]{8}(?:-[A-Fa-f0-9]{4}){3}-[A-Fa-f0-9]{12}", value["profile_uuid"])
            or not re.fullmatch(r"[A-Z0-9]{10}\." + re.escape(BUNDLE), value["keychain_group"])):
        raise RunnerSigningError("selection_invalid")
    # The App ID prefix can differ from the team for legacy Apple accounts;
    # the existing preflight verifies both. Do not replace its prefix policy.
    try:
        expires = datetime.fromisoformat(value["expires"])
        if expires.tzinfo is None or expires.utcoffset() is None:
            raise ValueError
    except ValueError:
        raise RunnerSigningError("selection_invalid") from None
    if expires <= (now or datetime.now(timezone.utc)):
        raise RunnerSigningError("selection_expired")
    return value


def signing_settings(selection):
    return {
        "CODE_SIGN_STYLE": "Manual",
        "CODE_SIGNING_ALLOWED": "YES",
        "CODE_SIGNING_REQUIRED": "YES",
        "CODE_SIGN_IDENTITY": selection["certificate"],
        "DEVELOPMENT_TEAM": TEAM,
        "PROVISIONING_PROFILE_SPECIFIER": selection["profile_uuid"],
    }


def generate(selection_path, output, private_root=None):
    selection = validated_selection(read_private_json(
        selection_path, MAX_SELECTION_BYTES, private_root))
    output = private_path(output, private_root)
    root = Path(os.path.abspath(private_root or PRIVATE_ROOT))
    if output != root / "signing/runner-signing.xcconfig":
        raise RunnerSigningError("private_path_invalid")
    contents = "// Existing validated selection; Runner Release on iPhoneOS only.\n"
    contents += "".join(f"{key}{CONDITION} = {value}\n"
                        for key, value in signing_settings(selection).items())
    descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        stream.write(contents)
    return {"schema": 1, "status": "passed", "operation": "generate",
            "scope": "runner-release-iphoneos", "settings": 6}


def verify_settings(selection, records):
    selection = validated_selection(selection)
    if (not isinstance(records, list) or not records or len(records) > MAX_TARGETS):
        raise RunnerSigningError("settings_invalid")
    runners = []
    others = []
    for record in records:
        if (not isinstance(record, dict) or not isinstance(record.get("target"), str)
                or not record["target"] or not isinstance(record.get("buildSettings"), dict)):
            raise RunnerSigningError("settings_invalid")
        settings = record["buildSettings"]
        if not all(isinstance(key, str) and isinstance(value, str)
                   for key, value in settings.items()):
            raise RunnerSigningError("settings_invalid")
        if record["target"] == "Runner":
            runners.append(settings)
        else:
            others.append(settings)
    if len(runners) != 1:
        raise RunnerSigningError("runner_missing_or_ambiguous")
    runner = runners[0]
    required = {
        **signing_settings(selection),
        "TARGET_NAME": "Runner",
        "PRODUCT_TYPE": "com.apple.product-type.application",
        "PRODUCT_BUNDLE_IDENTIFIER": BUNDLE,
        "CONFIGURATION": "Release",
        "PLATFORM_NAME": "iphoneos",
        "CODE_SIGN_ENTITLEMENTS": "Runner/Runner.entitlements",
        "CURRENT_PROJECT_VERSION": UPLOAD_BUILD,
        "FLUTTER_BUILD_NUMBER": UPLOAD_BUILD,
    }
    if (any(runner.get(key) != value for key, value in required.items())
            or runner.get("PROVISIONING_PROFILE", "") != ""):
        raise RunnerSigningError("runner_settings_mismatch")
    for settings in others:
        if (settings.get("PROVISIONING_PROFILE_SPECIFIER", "") != ""
                or settings.get("PROVISIONING_PROFILE", "") != ""):
            raise RunnerSigningError("dependency_profile_present")
    return {"schema": 1, "status": "passed", "operation": "verify-build-settings",
            "reported_targets": len(records), "other_reported_targets": len(others),
            "runner_signing_verified": True, "reported_dependency_profiles_clear": True,
            "coverage": "returned-targets-only",
            "limits": "Unreported dependency settings and the signed archive remain unverified."}


def verify_build_settings(selection_path, settings_path, private_root=None):
    selection = read_private_json(selection_path, MAX_SELECTION_BYTES, private_root)
    records = read_private_json(settings_path, MAX_SETTINGS_BYTES, private_root)
    return verify_settings(selection, records)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("generate")
    create.add_argument("selection", type=Path)
    create.add_argument("output", type=Path)
    verify = commands.add_parser("verify-build-settings")
    verify.add_argument("selection", type=Path)
    verify.add_argument("settings", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "generate":
            report = generate(args.selection, args.output)
        else:
            report = verify_build_settings(args.selection, args.settings)
    except (RunnerSigningError, OSError, ValueError, UnicodeError, RecursionError) as error:
        reason = error.reason if isinstance(error, RunnerSigningError) else "signing_io_failed"
        print(json.dumps({"schema": 1, "status": "failed", "operation": args.command,
                          "reason": reason}, sort_keys=True))
        return 2
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())

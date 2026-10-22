"""Synthetic Runner signing configuration; no Xcode, signing or credentials run."""

from contextlib import redirect_stdout
from copy import deepcopy
from datetime import datetime, timezone
import io
import json
from pathlib import Path
import re
import stat
import tempfile
import unittest
from unittest.mock import patch

import runner_signing as signing


FINGERPRINT = "0123456789ABCDEF0123456789ABCDEF01234567"
PROFILE = "FEEDFACE-0123-4567-89AB-0123456789AB"
PRIVATE_TEXT = "PRIVATE_MATERIAL_MUST_NOT_REACH_REPORT"


def selection():
    return {"certificate": FINGERPRINT, "profile_uuid": PROFILE,
            "keychain_group": signing.TEAM + "." + signing.BUNDLE,
            "expires": "2090-01-01T00:00:00+00:00"}


def runner():
    return {"target": "Runner", "action": "build", "buildSettings": {
        "TARGET_NAME": "Runner", "PRODUCT_TYPE": "com.apple.product-type.application",
        "PRODUCT_BUNDLE_IDENTIFIER": signing.BUNDLE, "CONFIGURATION": "Release",
        "PLATFORM_NAME": "iphoneos", "CODE_SIGN_ENTITLEMENTS": "Runner/Runner.entitlements",
        "CURRENT_PROJECT_VERSION": "37", "FLUTTER_BUILD_NUMBER": "37",
        "CODE_SIGN_STYLE": "Manual", "CODE_SIGNING_ALLOWED": "YES",
        "CODE_SIGNING_REQUIRED": "YES", "CODE_SIGN_IDENTITY": FINGERPRINT,
        "DEVELOPMENT_TEAM": signing.TEAM, "PROVISIONING_PROFILE_SPECIFIER": PROFILE,
        "PROVISIONING_PROFILE": "",
    }}


class RunnerSigningTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ryhze-runner-signing-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.private = self.directory / "private"
        self.private.mkdir(mode=0o700)
        (self.private / "signing").mkdir()
        self.selection_path = self.private / "signing/selection.json"
        self.selection_path.write_text(json.dumps(selection()))
        self.output = self.private / "signing/runner-signing.xcconfig"
        self.settings_path = self.private / "archive-build-settings.json"
        self.settings_path.write_text(json.dumps([runner()]))

    def assert_private_absent(self, report):
        encoded = json.dumps(report)
        for value in (FINGERPRINT, PROFILE, PRIVATE_TEXT, str(self.directory)):
            self.assertNotIn(value, encoded)

    def test_generator_writes_only_runner_release_device_signing(self):
        report = signing.generate(self.selection_path, self.output, self.private)
        expected = {
            "CODE_SIGN_STYLE": "Manual", "CODE_SIGNING_ALLOWED": "YES",
            "CODE_SIGNING_REQUIRED": "YES", "CODE_SIGN_IDENTITY": FINGERPRINT,
            "DEVELOPMENT_TEAM": signing.TEAM, "PROVISIONING_PROFILE_SPECIFIER": PROFILE,
        }
        lines = self.output.read_text().splitlines()
        self.assertEqual(lines[1:], [
            f"{name}[config=Release][sdk=iphoneos*] = {value}"
            for name, value in expected.items()])
        self.assertEqual(stat.S_IMODE(self.output.stat().st_mode), 0o600)
        self.assert_private_absent(report)
        with self.assertRaises(FileExistsError):
            signing.generate(self.selection_path, self.output, self.private)

    def test_minimal_returned_runner_is_valid_without_invented_spm_coverage(self):
        report = signing.verify_build_settings(
            self.selection_path, self.settings_path, self.private)
        self.assertEqual(report["reported_targets"], 1)
        self.assertEqual(report["other_reported_targets"], 0)
        self.assertEqual(report["coverage"], "returned-targets-only")
        self.assertIn("Unreported", report["limits"])
        self.assert_private_absent(report)

    def test_all_reported_dependency_profiles_must_be_empty(self):
        dependency = {"target": "FlutterFramework", "buildSettings": {}}
        records = [runner(), dependency]
        self.assertEqual(signing.verify_settings(selection(), records)["reported_targets"], 2)
        for key in ("PROVISIONING_PROFILE_SPECIFIER", "PROVISIONING_PROFILE"):
            with self.subTest(key=key):
                bad = deepcopy(records)
                bad[1]["buildSettings"][key] = PROFILE
                with self.assertRaisesRegex(signing.RunnerSigningError, "dependency_profile_present"):
                    signing.verify_settings(selection(), bad)
                bad[1]["buildSettings"][key] = " "
                with self.assertRaises(signing.RunnerSigningError):
                    signing.verify_settings(selection(), bad)

    def test_wrong_runner_identity_configuration_or_signing_is_rejected(self):
        changes = {
            "TARGET_NAME": "Other", "PRODUCT_TYPE": "com.apple.product-type.framework",
            "PRODUCT_BUNDLE_IDENTIFIER": "com.example.wrong", "CONFIGURATION": "Profile",
            "PLATFORM_NAME": "iphonesimulator", "CODE_SIGN_ENTITLEMENTS": "Other.entitlements",
            "CURRENT_PROJECT_VERSION": "3", "FLUTTER_BUILD_NUMBER": "3",
            "CODE_SIGN_STYLE": "Automatic", "CODE_SIGNING_ALLOWED": "NO",
            "CODE_SIGNING_REQUIRED": "NO", "CODE_SIGN_IDENTITY": "Apple Distribution",
            "DEVELOPMENT_TEAM": "AAAAAAAAAA", "PROVISIONING_PROFILE_SPECIFIER": "wrong-profile",
            "PROVISIONING_PROFILE": "wrong-profile",
        }
        for key, value in changes.items():
            with self.subTest(key=key):
                record = runner()
                record["buildSettings"][key] = value
                with self.assertRaisesRegex(signing.RunnerSigningError, "runner_settings_mismatch"):
                    signing.verify_settings(selection(), [record])

    def test_missing_duplicate_and_malformed_targets_are_rejected(self):
        cases = [[], {}, [None], [{"target": "Runner"}],
                 [{"target": "Other", "buildSettings": {}}], [runner(), runner()]]
        bad = runner()
        bad["buildSettings"]["UNRELATED_SETTING"] = ["unexpected-array"]
        cases.append([bad])
        for records in cases:
            with self.subTest(records=records), self.assertRaises(signing.RunnerSigningError):
                signing.verify_settings(selection(), records)

    def test_unsafe_selection_values_never_reach_xcconfig(self):
        values = ["$(OTHER)", "bad\nCODE_SIGNING_ALLOWED = NO", 'bad"',
                  "bad // comment", PRIVATE_TEXT, "", None, True]
        for field in ("certificate", "profile_uuid", "keychain_group"):
            for value in values:
                with self.subTest(field=field, value=value):
                    candidate = selection()
                    candidate[field] = value
                    self.selection_path.write_text(json.dumps(candidate))
                    with self.assertRaises(signing.RunnerSigningError):
                        signing.generate(self.selection_path, self.output, self.private)
                    self.assertFalse(self.output.exists())

    def test_selection_must_be_unexpired_and_for_the_authorized_app(self):
        for changes in ({"keychain_group": signing.TEAM + ".com.example.wrong"},
                        {"expires": "2000-01-01T00:00:00+00:00"},
                        {"expires": "2090-01-01"}, {"expires": "not-a-date"},
                        {"team": "AAAAAAAAAA"}):
            candidate = {**selection(), **changes}
            with self.subTest(changes=changes), self.assertRaises(signing.RunnerSigningError):
                signing.validated_selection(candidate)
        # Preserve the existing preflight's support for a legacy App ID prefix.
        legacy = {**selection(), "keychain_group": "AAAAAAAAAA." + signing.BUNDLE}
        self.assertEqual(signing.validated_selection(legacy), legacy)
        with self.assertRaisesRegex(signing.RunnerSigningError, "selection_expired"):
            signing.validated_selection(selection(), datetime(2090, 1, 1, tzinfo=timezone.utc))

    def test_input_is_bounded_regular_json_with_unique_keys(self):
        for contents in (b'{"certificate":1,"certificate":2}', b'\xff', b'{malformed'):
            self.selection_path.write_bytes(contents)
            with self.subTest(contents=contents), self.assertRaises(signing.RunnerSigningError):
                signing.read_private_json(self.selection_path, 1024, self.private)
        self.selection_path.write_bytes(b" " * 33)
        with self.assertRaises(signing.RunnerSigningError):
            signing.read_private_json(self.selection_path, 32, self.private)
        with self.assertRaises(signing.RunnerSigningError):
            signing.read_private_json(self.private / "signing", 32, self.private)

    def test_private_paths_reject_symlinks_public_locations_and_wrong_output(self):
        outside = self.directory / "public.json"
        outside.write_text(json.dumps(selection()))
        link = self.private / "linked.json"
        link.symlink_to(self.selection_path)
        for path in (outside, link):
            with self.subTest(path=path), self.assertRaises(signing.RunnerSigningError):
                signing.read_private_json(path, 1024, self.private)
        parent_link = self.private / "linked-directory"
        parent_link.symlink_to(self.private / "signing", target_is_directory=True)
        with self.assertRaises(signing.RunnerSigningError):
            signing.read_private_json(parent_link / "selection.json", 1024, self.private)
        with self.assertRaises(signing.RunnerSigningError):
            signing.generate(self.selection_path, self.private / "wrong.xcconfig", self.private)
        self.private.chmod(0o755)
        with self.assertRaises(signing.RunnerSigningError):
            signing.read_private_json(self.selection_path, 1024, self.private)

    def test_cli_reports_only_fixed_metadata_for_success_and_failures(self):
        cases = [(["generate", str(self.selection_path), str(self.output)], 0),
                 (["generate", str(self.selection_path), str(self.output)], 2),
                 (["verify-build-settings", str(self.selection_path), str(self.settings_path)], 0),
                 (["verify-build-settings", str(self.selection_path), str(self.private / PRIVATE_TEXT)], 2)]
        for args, expected in cases:
            output = io.StringIO()
            with patch.object(signing, "PRIVATE_ROOT", self.private), redirect_stdout(output):
                self.assertEqual(signing.main(args), expected)
            report = json.loads(output.getvalue())
            self.assertEqual(report["status"], "passed" if expected == 0 else "failed")
            self.assert_private_absent(report)
        self.settings_path.write_text(PRIVATE_TEXT)
        output = io.StringIO()
        with patch.object(signing, "PRIVATE_ROOT", self.private), redirect_stdout(output):
            self.assertEqual(signing.main([
                "verify-build-settings", str(self.selection_path), str(self.settings_path)]), 2)
        self.assert_private_absent(json.loads(output.getvalue()))

    def test_optional_include_preserves_default_and_target_only_wiring(self):
        root = Path(__file__).resolve().parents[2]
        release = root / "ios/Flutter/Release.xcconfig"
        self.assertEqual(release.read_text().splitlines(), [
            '#include "Generated.xcconfig"',
            '#include? "../../build/apple-testflight-private/signing/runner-signing.xcconfig"'])
        self.assertEqual((root / "ios/Flutter/Debug.xcconfig").read_text(),
                         '#include "Generated.xcconfig"\n')
        project = (root / "ios/Runner.xcodeproj/project.pbxproj").read_text()
        blocks = re.findall(r"\t\t([A-F0-9]{24}) /\* [^\n]+ \*/ = \{\n(.*?)\n\t\t\};", project, re.S)
        mapped = {identifier for identifier, body in blocks
                  if "baseConfigurationReference = 7AFA3C8E1D35360C0083082E" in body}
        self.assertEqual(mapped, {"97C147071CF9000F007C117D", "249021D4217E4FDB00AE95B9"})
        # Both mappings belong to Runner; the generated config's Release
        # condition excludes Profile and its iphoneos condition excludes Simulator.
        runner_list = next(body for identifier, body in blocks
                           if identifier == "97C147051CF9000F007C117D")
        for identifier in mapped:
            self.assertIn(identifier, runner_list)
        self.assertNotIn("runner-signing.xcconfig", project)


if __name__ == "__main__":
    unittest.main()

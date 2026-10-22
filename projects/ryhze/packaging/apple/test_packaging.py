"""Synthetic archive tests; they do not build or sign an Apple application."""

import plistlib
import shutil
import datetime
import copy
import stat
import tempfile
import unittest
import zipfile
from pathlib import Path

from check_sdk import EXPECTED, verify
from check_native_tests import verify as verify_native_tests
from check_macos_entitlements import verify as verify_macos_entitlements
from verify_package import source_version, verify_package


class SDKPinTests(unittest.TestCase):
    def test_exact_framework_and_runtime_are_required(self):
        verify(dict(EXPECTED))
        for key in EXPECTED:
            changed = dict(EXPECTED)
            changed[key] = "different"
            with self.subTest(key=key), self.assertRaises(ValueError):
                verify(changed)
        with self.assertRaises(ValueError):
            verify({})


class NativeEvidenceTests(unittest.TestCase):
    def test_build_success_without_expected_test_execution_is_rejected(self):
        with self.assertRaises(ValueError):
            verify_native_tests("seed", "** TEST SUCCEEDED **")
        passed = "Test Case '-[RunnerTests.RunnerTests testKeychainSeed]' passed (0.2 seconds)."
        verify_native_tests("seed", passed + "\n** TEST SUCCEEDED **")
        with self.assertRaises(ValueError):
            verify_native_tests("restore", passed + "\n** TEST SUCCEEDED **")
        with self.assertRaises(ValueError):
            verify_native_tests("seed", passed + "\n** TEST FAILED **")
        core = ["testKeychainRoundTripAndScopeIsolation", "testDurableSnapshotReopensAndRejectsStaleWrite",
                "testUnsupportedSchemaIsPreserved", "testStateSymlinksAreRejected"]
        def log(names):
            return "\n".join(f"Test Case '-[RunnerTests.RunnerTests {name}]' passed (0.2 seconds)."
                             for name in names) + "\n** TEST SUCCEEDED **"
        with self.assertRaises(ValueError):
            verify_native_tests("core", log(core))
        migration = ["testAtomicMigrationPreservesBackupAndReopens",
                     "testInterruptedMigrationRollsBackBothWrites", "testFutureDocumentSchemaIsPreserved"]
        verify_native_tests("core", log(core + migration))
        for missing in migration:
            with self.subTest(missing=missing), self.assertRaises(ValueError):
                verify_native_tests("core", log(core + [name for name in migration if name != missing]))


class MacSigningMetadataTests(unittest.TestCase):
    def setUp(self):
        self.now = datetime.datetime(2026, 10, 7, tzinfo=datetime.timezone.utc)
        self.identifier = "B45M4C7CDW.com.ryhze.ryhze"
        self.entitlements = {
            "com.apple.developer.team-identifier": "B45M4C7CDW",
            "com.apple.application-identifier": self.identifier,
            "keychain-access-groups": [self.identifier],
            "com.apple.security.app-sandbox": True,
        }
        self.profile = {
            "ExpirationDate": datetime.datetime(2026, 10, 8),
            "TeamIdentifier": ["B45M4C7CDW"], "Platform": ["OSX"],
            "ApplicationIdentifierPrefix": ["B45M4C7CDW"],
            "Entitlements": {
                "com.apple.application-identifier": self.identifier,
                "keychain-access-groups": ["B45M4C7CDW.*"],
            },
        }

    def test_metadata_alone_explicitly_requires_native_execution(self):
        result = verify_macos_entitlements(self.entitlements, self.profile, now=self.now)
        self.assertEqual(result["native_keychain_execution"], "still_required")

    def test_adhoc_or_wrong_built_identity_cannot_pass(self):
        for key, value in [
            ("com.apple.developer.team-identifier", None),
            ("com.apple.application-identifier", "other.app"),
            ("keychain-access-groups", ["B45M4C7CDW.*"]),
            ("keychain-access-groups", [self.identifier, "unrelated.group"]),
        ]:
            changed = dict(self.entitlements)
            changed[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                verify_macos_entitlements(changed, self.profile, now=self.now)

    def test_expired_wrong_team_or_unauthorized_profile_cannot_pass(self):
        for field, value in [
            ("ExpirationDate", datetime.datetime(2026, 10, 6)),
            ("TeamIdentifier", ["OTHERTEAM00"]),
            ("Platform", ["iOS"]),
            ("ApplicationIdentifierPrefix", ["OTHERTEAM00"]),
        ]:
            changed = copy.deepcopy(self.profile)
            changed[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                verify_macos_entitlements(self.entitlements, changed, now=self.now)
        changed = copy.deepcopy(self.profile)
        changed["Entitlements"]["keychain-access-groups"] = ["OTHERTEAM00.*"]
        with self.assertRaises(ValueError):
            verify_macos_entitlements(self.entitlements, changed, now=self.now)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "fixture.ipa"
        version, build = source_version()
        self.info = {
            "CFBundleIdentifier": "com.ryhze.ryhze",
            "CFBundleShortVersionString": version,
            "CFBundleVersion": build,
            "CFBundleExecutable": "Runner",
            "CFBundlePackageType": "APPL",
        }
        self.privacy = {"NSPrivacyAccessedAPITypes": [{
            "NSPrivacyAccessedAPIType": "NSPrivacyAccessedAPICategoryFileTimestamp",
            "NSPrivacyAccessedAPITypeReasons": ["C617.1"],
        }]}

    def make(self, extra=None, *, info=True, binary=True, assets=True, privacy=True):
        with zipfile.ZipFile(self.path, "w") as package:
            if info:
                package.writestr("Payload/Runner.app/Info.plist", plistlib.dumps(self.info))
            if binary:
                package.writestr("Payload/Runner.app/Runner", b"synthetic binary fixture")
            if privacy:
                package.writestr("Payload/Runner.app/PrivacyInfo.xcprivacy", plistlib.dumps(self.privacy))
            if assets:
                package.writestr("Payload/Runner.app/Frameworks/App.framework/flutter_assets/AssetManifest.bin", b"fixture")
            for name, value in extra or []:
                package.writestr(name, value)

    def test_metadata_and_assets_pass_without_claiming_signing(self):
        self.make()
        self.assertEqual(verify_package(self.path)["build"], self.info["CFBundleVersion"])

    def test_wrong_identity_version_and_build_fail(self):
        for key in ("CFBundleIdentifier", "CFBundleShortVersionString", "CFBundleVersion"):
            previous = self.info[key]
            self.info[key] = "wrong"
            self.make()
            with self.subTest(key=key), self.assertRaises(ValueError):
                verify_package(self.path)
            self.info[key] = previous

    def test_historical_updater_and_renamed_executables_fail(self):
        root = "Payload/Runner.app/Frameworks/App.framework/flutter_assets/"
        for name, content in [
            ("assets/update/update-linux.py", b"print('fixture')"),
            ("assets/UPDATE/data.json", b"{}"),
            ("assets/setup.EXE", b"fixture"),
            ("assets/payload.data", b"MZfixture"),
            ("assets/payload.data", b"\x7fELFfixture"),
            ("assets/payload.data", b"\xcf\xfa\xed\xfefixture"),
            ("assets/payload.data", b"#!/bin/sh\n"),
        ]:
            self.make([(root + name, content)])
            with self.subTest(name=name, content=content[:4]), self.assertRaises(ValueError):
                verify_package(self.path)

    def test_unsafe_paths_and_duplicate_entries_fail(self):
        for name in ["../escape", "/absolute", "Payload/../escape", "Payload//escape", "Payload\\escape", "Payload/./escape", "Payload/control\n", "Payload/Runner.app/runner"]:
            self.make([(name, b"fixture")])
            with self.subTest(name=name), self.assertRaises(ValueError):
                verify_package(self.path)

    def test_symlink_asset_fails(self):
        entry = zipfile.ZipInfo("Payload/Runner.app/flutter_assets/link")
        entry.create_system = 3
        entry.external_attr = (stat.S_IFLNK | 0o777) << 16
        self.make([(entry, b"../../outside")])
        with self.assertRaises(ValueError):
            verify_package(self.path)

    def test_incomplete_or_ambiguous_archive_fails(self):
        for option in ["info", "binary", "assets", "privacy"]:
            self.make(**{option: False})
            with self.subTest(option=option), self.assertRaises(ValueError):
                verify_package(self.path)
        self.make([("Payload/Other.app/Info.plist", plistlib.dumps(self.info))])
        with self.assertRaises(ValueError):
            verify_package(self.path)
        self.make([("Other/flutter_assets/AssetManifest.bin", b"fixture")], assets=False)
        with self.assertRaises(ValueError):
            verify_package(self.path)

    def make_app(self):
        app = Path(self.directory.name) / "Runner.app"
        app.mkdir()
        (app / "Info.plist").write_bytes(plistlib.dumps(self.info))
        (app / "PrivacyInfo.xcprivacy").write_bytes(plistlib.dumps(self.privacy))
        (app / "Runner").write_bytes(b"synthetic binary fixture")
        assets = app / "Frameworks/App.framework/flutter_assets"
        assets.mkdir(parents=True)
        (assets / "AssetManifest.bin").write_bytes(b"fixture")
        return app, assets

    def test_built_app_gets_same_metadata_and_asset_checks(self):
        app, assets = self.make_app()
        self.assertEqual(verify_package(app)["build"], self.info["CFBundleVersion"])
        (assets / "payload.data").write_bytes(b"MZfixture")
        with self.assertRaises(ValueError):
            verify_package(app)
        (assets / "payload.data").unlink()
        (assets / "update-linux.py").write_bytes(b"fixture")
        with self.assertRaises(ValueError):
            verify_package(app)
        (assets / "update-linux.py").unlink()
        self.info["CFBundleIdentifier"] = "wrong"
        (app / "Info.plist").write_bytes(plistlib.dumps(self.info))
        with self.assertRaises(ValueError):
            verify_package(app)

    def test_built_app_rejects_symlink_file_and_directory_without_reading_target(self):
        app, assets = self.make_app()
        outside = Path(self.directory.name) / "outside"
        outside.mkdir()
        (outside / "preserved").write_bytes(b"unchanged")
        for target in [outside, outside / "preserved"]:
            link = assets / "link"
            link.symlink_to(target, target_is_directory=target.is_dir())
            with self.subTest(target=target.name), self.assertRaises(ValueError):
                verify_package(app)
            link.unlink()
            self.assertEqual((outside / "preserved").read_bytes(), b"unchanged")

    def test_built_app_missing_assets_or_binary_fails(self):
        app, assets = self.make_app()
        (app / "Runner").unlink()
        with self.assertRaises(ValueError):
            verify_package(app)
        (app / "Runner").write_bytes(b"fixture")
        shutil.rmtree(assets)
        with self.assertRaises(ValueError):
            verify_package(app)

    def test_missing_or_wrong_privacy_reason_fails_for_actual_artifact(self):
        self.privacy["NSPrivacyAccessedAPITypes"][0]["NSPrivacyAccessedAPITypeReasons"] = ["35F9.1"]
        self.make()
        with self.assertRaises(ValueError):
            verify_package(self.path)
        app, _ = self.make_app()
        with self.assertRaises(ValueError):
            verify_package(app)


if __name__ == "__main__":
    unittest.main()

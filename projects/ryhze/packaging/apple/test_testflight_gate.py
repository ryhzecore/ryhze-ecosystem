"""Synthetic policy/extraction tests, not Apple signing or native acceptance."""

import copy
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import plistlib
import stat
import subprocess
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import testflight_gate as gate


NOW = datetime(2026, 10, 8, tzinfo=timezone.utc)
CERTIFICATE = b"synthetic fixture, not an Apple certificate or private key"
FINGERPRINT = hashlib.sha1(CERTIFICATE).hexdigest().upper()
UUID = "01234567-89AB-CDEF-0123-456789ABCDEF"


def profile():
    return {"TeamIdentifier": [gate.TEAM], "Platform": ["iOS"],
            "ExpirationDate": NOW + timedelta(days=90), "UUID": UUID,
            "ApplicationIdentifierPrefix": [gate.TEAM],
            "DeveloperCertificates": [CERTIFICATE],
            "Entitlements": {"application-identifier": gate.TEAM + "." + gate.BUNDLE,
                             "com.apple.developer.team-identifier": gate.TEAM,
                             "get-task-allow": False,
                             "keychain-access-groups": [gate.TEAM + ".*"]}}


class TestFlightGates(unittest.TestCase):
    def test_live_checkpoint_allows_only_existing_36_to_37(self):
        value = {"buildId": "existing-build-id", "version": "0.1.0", "buildNumber": "36"}
        self.assertEqual(gate.check_latest(value)["upload_build"], "37")
        for number in (None, "", 36, "035", "35", "37", "100", "36.0"):
            with self.subTest(number=number), self.assertRaises(ValueError):
                gate.check_latest({**value, "buildNumber": number})
        for bad in (None, [], "36", {}, {**value, "buildId": ""}, {**value, "version": None}):
            with self.subTest(value=bad), self.assertRaises(ValueError):
                gate.check_latest(bad)

    def test_live_lookup_uses_global_versions_and_fails_on_empty_or_failure(self):
        result = subprocess.CompletedProcess([], 0, b'{"buildId":"id","version":"0.1.0","buildNumber":"36"}')
        with patch.object(gate.subprocess, "run", return_value=result) as run:
            self.assertEqual(gate.live_latest()["upload_build"], "37")
            command = run.call_args.args[0]
            for flag in ("--all-versions", "--include-version", "--json", "--disable-jwt-cache"):
                self.assertIn(flag, command)
            self.assertIn(gate.APP, command)
            self.assertNotIn("--create", command)
        for result in (subprocess.CompletedProcess([], 0, b""),
                       subprocess.CompletedProcess([], 1, b"private provider response")):
            with patch.object(gate.subprocess, "run", return_value=result), self.assertRaises(ValueError):
                gate.live_latest()

    def test_existing_store_profile_and_exact_scope(self):
        details = gate.profile_details(profile(), NOW)
        self.assertEqual(details["certificates"], {FINGERPRINT})
        self.assertEqual(details["keychain_group"], gate.TEAM + "." + gate.BUNDLE)
        value = profile()
        value["Entitlements"]["keychain-access-groups"] = [gate.TEAM + "." + gate.BUNDLE]
        gate.profile_details(value, NOW)

    def test_wrong_distribution_team_expiry_or_missing_private_identity_fails(self):
        edits = [{"TeamIdentifier": ["OTHERTEAM01"]}, {"Platform": ["OSX"]},
                 {"ExpirationDate": NOW}, {"ProvisionedDevices": []},
                 {"ProvisionsAllDevices": True}, {"DeveloperCertificates": []},
                 {"UUID": "-" * 36}, {"ApplicationIdentifierPrefix": [None]}]
        for edit in edits:
            with self.subTest(edit=edit), self.assertRaises(ValueError):
                gate.profile_details({**profile(), **edit}, NOW)
        for edit in ({"get-task-allow": True}, {"application-identifier": gate.TEAM + ".*"},
                     {"com.apple.developer.team-identifier": "OTHERTEAM01"},
                     {"keychain-access-groups": ["OTHERTEAM01.*"]}):
            value = profile()
            value["Entitlements"].update(edit)
            with self.subTest(edit=edit), self.assertRaises(ValueError):
                gate.profile_details(value, NOW)
        self.assertEqual(gate.valid_identities('0 valid identities found'), set())
        self.assertEqual(gate.valid_identities(f'1) {FINGERPRINT} "Apple Development: Example"'), set())
        self.assertEqual(gate.valid_identities(f'1) {FINGERPRINT} "Apple Distribution: Example"'), {FINGERPRINT})

    def test_existing_profile_discovery_supports_both_xcode_locations(self):
        for relative in ("Library/MobileDevice/Provisioning Profiles",
                         "Library/Developer/Xcode/UserData/Provisioning Profiles"):
            with self.subTest(path=relative), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                directory = root / relative
                directory.mkdir(parents=True)
                (directory / "existing.mobileprovision").write_bytes(b"synthetic")
                value = profile()
                value["ExpirationDate"] = datetime.now(timezone.utc) + timedelta(days=1)
                identities = f'1) {FINGERPRINT} "Apple Distribution: Example"'.encode()
                with patch.object(gate.Path, "home", return_value=root), \
                     patch.object(gate, "command_bytes", return_value=identities), \
                     patch.object(gate, "decode_profile", return_value=value):
                    gate.prepare_signing(root / "out")
                    selection = json.loads((root / "out/selection.json").read_text())
                    options = plistlib.loads((root / "out/export-options.plist").read_bytes())
                self.assertEqual(selection["certificate"], FINGERPRINT)
                self.assertEqual(options["provisioningProfiles"], {gate.BUNDLE: UUID})
                self.assertTrue(options["testFlightInternalTestingOnly"])
                self.assertFalse(options["manageAppVersionAndBuildNumber"])
                self.assertEqual(options["signingStyle"], "manual")
                self.assertEqual(options["method"], "app-store-connect")

    def test_no_existing_identity_produces_no_signing_selection(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.object(gate.Path, "home", return_value=root), \
                 patch.object(gate, "command_bytes", return_value=b"0 valid identities found"), \
                 self.assertRaisesRegex(ValueError, "no_qualifying_signing_pair"):
                gate.prepare_signing(root / "out")
            self.assertFalse((root / "out").exists())

    def test_signed_entitlements_require_exact_own_keychain_group(self):
        selection = {"keychain_group": gate.TEAM + "." + gate.BUNDLE}
        entitlements = copy.deepcopy(profile()["Entitlements"])
        entitlements["keychain-access-groups"] = [selection["keychain_group"]]
        gate.check_entitlements(entitlements, selection)
        for edit in ({"get-task-allow": True}, {"application-identifier": "wrong"},
                     {"keychain-access-groups": [gate.TEAM + ".*"]},
                     {"keychain-access-groups": [selection["keychain_group"], "other"]}):
            with self.subTest(edit=edit), self.assertRaises(ValueError):
                gate.check_entitlements({**entitlements, **edit}, selection)

    def test_export_extraction_preserves_executable_bytes_and_mode(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entry = zipfile.ZipInfo("Payload/Runner.app/Runner")
            entry.external_attr = (stat.S_IFREG | 0o755) << 16
            with zipfile.ZipFile(root / "app.ipa", "w") as archive:
                archive.writestr(entry, b"synthetic executable bytes")
            gate.extract_checked(root / "app.ipa", root / "out")
            binary = root / "out/Payload/Runner.app/Runner"
            self.assertEqual(binary.read_bytes(), b"synthetic executable bytes")
            self.assertEqual(binary.stat().st_mode & 0o777, 0o755)

    def test_export_extraction_rejects_unsafe_paths_and_links_before_writing(self):
        for name, mode in [("../escape", stat.S_IFREG), ("/escape", stat.S_IFREG),
                           ("Payload/../escape", stat.S_IFREG), ("Payload\\escape", stat.S_IFREG),
                           ("Payload/Runner.app/link", stat.S_IFLNK),
                           ("Payload/Runner.app/device", stat.S_IFCHR)]:
            with self.subTest(path=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                entry = zipfile.ZipInfo(name)
                entry.external_attr = (mode | 0o644) << 16
                with zipfile.ZipFile(root / "app.ipa", "w") as archive:
                    archive.writestr("Payload/safe", b"safe")
                    archive.writestr(entry, b"synthetic")
                with self.assertRaises(ValueError):
                    gate.extract_checked(root / "app.ipa", root / "out")
                self.assertFalse((root / "out").exists())

    def test_export_extraction_rejects_case_collisions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with zipfile.ZipFile(root / "app.ipa", "w") as archive:
                archive.writestr("Payload/Runner.app/Runner", b"one")
                archive.writestr("Payload/Runner.app/runner", b"two")
            with self.assertRaises(ValueError):
                gate.extract_checked(root / "app.ipa", root / "out")

    def test_preparation_stops_failed_shared_native_and_signature_gates(self):
        # Execute the actual orchestration against synthetic tools. No Apple,
        # Flutter, credentials or network is contacted; this tests shell gating.
        fake_tool = r'''
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
root = pathlib.Path(os.environ['RYHZE_SYNTHETIC_ROOT'])
failure = os.environ['RYHZE_SYNTHETIC_FAILURE']
with (root / 'calls.jsonl').open('a') as trace:
    trace.write(json.dumps([name, *args]) + '\n')
if name == 'python3':
    if args[:1] == ['packaging/apple/testflight_gate.py']:
        if args[1] == 'signing':
            diagnostic = pathlib.Path(args[args.index('--diagnostics') + 1])
            diagnostic.write_text(json.dumps({'schema': 1, 'status': 'blocked' if failure == 'signing-existing' else 'selected', 'synthetic': True}))
            if failure == 'signing-existing': sys.exit(2)
            output = pathlib.Path(args[2]); output.mkdir(parents=True)
            (output / 'selection.json').write_text(json.dumps({'certificate': 'A' * 40, 'profile_uuid': '01234567-89AB-CDEF-0123-456789ABCDEF'}))
            (output / 'export-options.plist').write_text('synthetic')
        if args[1] == 'signed-ipa' and failure == 'signed-package':
            sys.exit(19)
        print('{}'); sys.exit(0)
    if args[:1] == ['packaging/apple/runner_signing.py']:
        phase = 'runner-signing' if args[1] == 'generate' else 'runner-signing-check'
        if failure == phase:
            print(json.dumps({'status': 'blocked', 'synthetic': True})); sys.exit(2)
        if args[1] == 'generate':
            pathlib.Path(args[3]).write_text('// Synthetic Runner-only signing fixture\n')
        elif args[1] == 'verify-build-settings':
            json.loads(pathlib.Path(args[3]).read_text())
        else:
            raise RuntimeError('Unexpected Runner signing command')
        print(json.dumps({'status': 'passed', 'synthetic': True})); sys.exit(0)
    if args[:1] == ['packaging/apple/simulator_ui_readiness.py'] and args[1] in ('wait', 'capture'):
        # Marker semantics are exercised by the simulator preparation fixtures;
        # these placeholders keep this suite focused on signing failure gates.
        report = {'state': 'ready', 'synthetic': True}
        pathlib.Path(args[args.index('--report') + 1]).write_text(json.dumps(report))
        print(json.dumps(report)); sys.exit(0)
    if args[:2] == ['-m', 'unittest']:
        print('Synthetic packaging suite placeholder'); sys.exit(0)
    if args[:1] == ['-c'] and 'source_version' in args[1]:
        print('0.6.0'); sys.exit(0)
    os.execv(os.environ['RYHZE_SYNTHETIC_PYTHON'], [os.environ['RYHZE_SYNTHETIC_PYTHON'], *args])
elif name == 'uname': print('Darwin')
elif name == 'git':
    if args[:1] == ['rev-parse']: print('a' * 40)
elif name == 'flutter':
    if '--version' in args:
        print(json.dumps({'frameworkVersion': '3.47.6', 'frameworkRevision': '5fc346839b5d0eef006ed8404392afb4dfae428d', 'dartSdkVersion': '3.13.5', 'flutterRoot': str(root)}))
    elif 'test' in args and failure == 'flutter-tests': sys.exit(17)
elif name == 'xcodebuild':
    if args == ['-version']: print('Xcode 26.6\nBuild version synthetic')
    elif args[:2] == ['-showBuildSettings', '-json']:
        print(json.dumps([{'target': 'Runner', 'buildSettings': {}}]))
    elif args[0] == 'test':
        if failure == 'ios-core': sys.exit(18)
        for arg in args:
            if arg.startswith('-only-testing:'):
                print("Test Case '-[RunnerTests.RunnerTests " + arg.rsplit('/', 1)[1] + "]' passed")
        print('** TEST SUCCEEDED **')
    elif args[0] == '-exportArchive':
        output = pathlib.Path(args[args.index('-exportPath') + 1]); output.mkdir(parents=True)
        (output / 'Ryhze.ipa').write_bytes(b'synthetic unverified IPA')
elif name == 'xcrun':
    if args[:4] == ['simctl', 'list', 'devices', 'available']:
        print(json.dumps({'devices': {'com.apple.CoreSimulator.SimRuntime.iOS-26-5': [{'isAvailable': True, 'name': 'iPhone synthetic', 'state': 'Booted', 'udid': '01234567-89AB-CDEF-0123-456789ABCDEF'}]}}))
    elif args[:2] == ['simctl', 'get_app_container']:
        directory = root / 'synthetic-container'; directory.mkdir()
        print(directory)
    elif args[:2] == ['simctl', 'launch']:
        if 'SIMCTL_CHILD_RYHZE_FIRST_FRAME_NONCE' not in os.environ:
            raise RuntimeError('Missing synthetic launch nonce')
        print('com.ryhze.ryhze: 4242')
    elif 'screenshot' in args: pathlib.Path(args[-1]).write_bytes(b'synthetic screenshot')
elif name == 'app-store-connect': print('0.70.0 synthetic')
elif name == 'sw_vers': print('synthetic macOS')
elif name == 'codesign': print('synthetic entitlements')
elif name != 'dart': raise RuntimeError('Unexpected synthetic command: ' + name)
'''
        sources = Path(__file__).parent
        for failure in ('signing-existing', 'flutter-tests', 'ios-core',
                        'runner-signing', 'runner-signing-check', 'signed-package'):
            with self.subTest(phase=failure), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                directory = root / 'packaging/apple'
                directory.mkdir(parents=True)
                for source in ('prepare_testflight.sh', 'run_bounded.py', 'check_sdk.py',
                               'check_native_tests.py', 'select_simulator.py', 'simulator_ui_readiness.py'):
                    shutil.copyfile(sources / source, directory / source)
                binary = root / 'bin'
                binary.mkdir()
                for command in ('python3', 'uname', 'git', 'flutter', 'xcodebuild', 'xcrun',
                                'app-store-connect', 'sw_vers', 'codesign', 'dart'):
                    target = binary / command
                    target.write_text('#!' + sys.executable + '\n' + fake_tool)
                    target.chmod(0o755)
                environment = {**os.environ, 'PATH': str(binary) + os.pathsep + os.environ['PATH'],
                               'RYHZE_FLUTTER': str(binary / 'flutter'),
                               'RYHZE_SYNTHETIC_ROOT': str(root), 'RYHZE_SYNTHETIC_FAILURE': failure,
                               'RYHZE_SYNTHETIC_PYTHON': sys.executable}
                result = subprocess.run(['bash', str(directory / 'prepare_testflight.sh')],
                                        env=environment, capture_output=True, text=True, timeout=30)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('no validated IPA', result.stderr)
                self.assertFalse(list((root / 'build/apple-testflight').glob('publish/*.ipa')))
                calls = [json.loads(line) for line in (root / 'calls.jsonl').read_text().splitlines()]
                archive_called = any(call[:2] == ['xcodebuild', 'archive'] for call in calls)
                self.assertEqual(archive_called, failure == 'signed-package')
                if failure in ('signing-existing', 'flutter-tests'):
                    self.assertFalse(any(call[:2] == ['xcodebuild', 'test'] for call in calls))
                if failure == 'signing-existing':
                    self.assertEqual(result.returncode, 2)
                    self.assertFalse(any(call[0] == 'flutter' and 'test' in call for call in calls))
                    self.assertEqual(json.loads((root / 'build/apple-testflight/signing-diagnostics.json').read_text())['status'], 'blocked')
                    self.assertFalse((root / 'build/apple-testflight-private/signing').exists())
                if failure in ('runner-signing', 'runner-signing-check'):
                    self.assertEqual(result.returncode, 2)
                    self.assertEqual(sum(call[:2] == ['xcodebuild', 'test'] for call in calls), 3)
                    signing = [call[2] for call in calls
                               if call[:2] == ['python3', 'packaging/apple/runner_signing.py']]
                    self.assertEqual(signing, ['generate'] if failure == 'runner-signing'
                                     else ['generate', 'verify-build-settings'])
                    self.assertFalse(any(call[:2] == ['xcodebuild', '-exportArchive'] for call in calls))
                    self.assertFalse(any(call[:3] == ['python3', 'packaging/apple/testflight_gate.py', 'signed-ipa']
                                         for call in calls))
                if failure == 'signed-package':
                    archive_index = next(index for index, call in enumerate(calls)
                                         if call[:2] == ['xcodebuild', 'archive'])
                    signing_check = [index for index, call in enumerate(calls)
                                     if call[:3] == ['python3', 'packaging/apple/runner_signing.py', 'verify-build-settings']]
                    self.assertEqual(len(signing_check), 1)
                    self.assertLess(signing_check[0], archive_index)
                    self.assertFalse(any(arg.startswith(('CODE_SIGN', 'DEVELOPMENT_TEAM=', 'PROVISIONING_PROFILE'))
                                         for arg in calls[archive_index]))
                    self.assertTrue((root / 'build/apple-testflight-private/export/Ryhze.ipa').exists())
                    self.assertFalse((root / 'build/apple-testflight/publish').exists())


if __name__ == "__main__":
    unittest.main()

"""Synthetic signed-IPA inspection; no Apple signing tools or credentials run."""

from contextlib import redirect_stderr, redirect_stdout
import copy
from datetime import datetime
import hashlib
import io
import json
from pathlib import Path
import plistlib
import stat
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import testflight_gate as gate


PRIVATE_TEXT = 'PRIVATE_TOOL_OUTPUT_MUST_NOT_BE_PUBLISHED'
PRIVATE_NAME = 'PRIVATE_SIGNING_NAME_MUST_NOT_BE_PUBLISHED'
CERTIFICATE = b'PRIVATE_SYNTHETIC_CERTIFICATE_BYTES_NOT_APPLE_MATERIAL'
FINGERPRINT = hashlib.sha1(CERTIFICATE).hexdigest().upper()
UUID = 'FEEDFACE-0123-4567-89AB-0123456789AB'
STAGES = (
    'package_structure', 'inspection_setup', 'safe_extraction', 'app_selection',
    'signature_strict', 'signature_requirement', 'certificate_extract',
    'certificate_match', 'embedded_profile_decode', 'embedded_profile_match',
    'entitlements_display', 'entitlements_match', 'ipa_digest',
)
TOOL_STAGES = (
    'signature_strict', 'signature_requirement', 'certificate_extract',
    'embedded_profile_decode', 'entitlements_display',
)


class SignedIpaDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='ryhze-synthetic-signed-ipa-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.archive = self.root / (PRIVATE_NAME + '.ipa')
        self.selection_path = self.root / (PRIVATE_NAME + '.json')
        self.selection = {
            'certificate': FINGERPRINT, 'profile_uuid': UUID,
            'expires': '2090-01-01T00:00:00+00:00',
            'keychain_group': gate.TEAM + '.' + gate.BUNDLE,
        }
        self.profile = {
            'Name': PRIVATE_NAME, 'UUID': UUID, 'TeamIdentifier': [gate.TEAM],
            'Platform': ['iOS'], 'ExpirationDate': datetime(2090, 1, 1),
            'ApplicationIdentifierPrefix': [gate.TEAM],
            'DeveloperCertificates': [CERTIFICATE],
            'Entitlements': {
                'application-identifier': self.selection['keychain_group'],
                'com.apple.developer.team-identifier': gate.TEAM,
                'get-task-allow': False,
                'keychain-access-groups': [gate.TEAM + '.*'],
            },
        }
        self.entitlements = {
            **self.profile['Entitlements'],
            'keychain-access-groups': [self.selection['keychain_group']],
        }
        self.info = {
            'CFBundleIdentifier': gate.BUNDLE,
            'CFBundleShortVersionString': gate.source_version()[0],
            'CFBundleVersion': gate.UPLOAD_BUILD,
            'CFBundleExecutable': 'Runner', 'CFBundlePackageType': 'APPL',
        }
        self.privacy = {'NSPrivacyAccessedAPITypes': [{
            'NSPrivacyAccessedAPIType': 'NSPrivacyAccessedAPICategoryFileTimestamp',
            'NSPrivacyAccessedAPITypeReasons': ['C617.1'],
        }]}
        self.commands = []
        self.extracted_apps = []
        self.failure_stage = None
        self.failure_kind = None
        self.leaf = CERTIFICATE
        self.profile_bytes = None
        self.entitlement_bytes = None
        self.active_diagnostics = None
        self.make_archive()

    def make_archive(self, *, extra=(), privacy=True):
        with zipfile.ZipFile(self.archive, 'w') as archive:
            archive.writestr('Payload/Runner.app/Info.plist', plistlib.dumps(self.info))
            executable = zipfile.ZipInfo('Payload/Runner.app/Runner')
            executable.external_attr = (stat.S_IFREG | 0o755) << 16
            archive.writestr(executable, b'synthetic executable; not signed Apple code')
            if privacy:
                archive.writestr('Payload/Runner.app/PrivacyInfo.xcprivacy', plistlib.dumps(self.privacy))
            archive.writestr('Payload/Runner.app/Frameworks/App.framework/flutter_assets/AssetManifest.bin', b'fixture')
            archive.writestr('Payload/Runner.app/embedded.mobileprovision', PRIVATE_TEXT.encode())
            for name, data in extra:
                archive.writestr(name, data)
        self.selection_path.write_text(json.dumps(self.selection))

    def run_tool(self, command, **options):
        self.assertEqual(options, {'capture_output': True, 'check': False, 'timeout': 60})
        if command[:4] == ['codesign', '--verify', '--deep', '--strict']:
            self.assertEqual(len(command), 5)
            stage = 'signature_strict'
        elif command[:3] == ['codesign', '--verify', '-R']:
            self.assertEqual(len(command), 5)
            self.assertEqual(command[3], '=' + (
                f'anchor apple generic and identifier "{gate.BUNDLE}" '
                f'and certificate leaf[subject.OU] = "{gate.TEAM}"'))
            stage = 'signature_requirement'
        elif command[:2] == ['codesign', '--display'] and command[2].startswith('--extract-certificates='):
            self.assertEqual(len(command), 4)
            self.assertNotIn('--extract-certificates', command)
            stage = 'certificate_extract'
        elif command[:4] == ['security', 'cms', '-D', '-i']:
            self.assertEqual(len(command), 5)
            self.assertEqual(Path(command[-1]).name, 'embedded.mobileprovision')
            stage = 'embedded_profile_decode'
        elif command[:5] == ['codesign', '--display', '--entitlements', '-', '--xml']:
            self.assertEqual(len(command), 6)
            stage = 'entitlements_display'
        else:
            self.fail('Unexpected signing command shape: ' + repr(command))
        self.commands.append((stage, list(command)))
        if self.active_diagnostics is not None:
            report = json.loads(self.active_diagnostics.read_text())
            self.assertEqual(report, {
                'schema': 1, 'operation': 'signed-ipa', 'status': 'running',
                'current_stage': stage,
                'checks': [{'stage': name, 'status': 'passed'} for name in STAGES[:STAGES.index(stage)]],
            })
            self.assert_safe(report)
        if command[0] == 'codesign':
            self.extracted_apps.append(Path(command[-1]))
        if stage == self.failure_stage:
            if self.failure_kind == 'nonzero':
                return subprocess.CompletedProcess(command, 23, PRIVATE_TEXT.encode(), PRIVATE_TEXT.encode())
            if self.failure_kind == 'signal':
                return subprocess.CompletedProcess(command, -9, PRIVATE_TEXT.encode(), PRIVATE_TEXT.encode())
            if self.failure_kind == 'timeout':
                raise subprocess.TimeoutExpired(command, 60, output=PRIVATE_TEXT.encode(), stderr=PRIVATE_TEXT.encode())
            if self.failure_kind == 'unavailable':
                raise FileNotFoundError(PRIVATE_TEXT + ' ' + str(self.root))
            if self.failure_kind == 'interrupted':
                raise KeyboardInterrupt
            if self.failure_kind == 'large_stderr':
                return subprocess.CompletedProcess(command, 23, PRIVATE_TEXT.encode(),
                                                   (PRIVATE_TEXT * 10000).encode())
            self.fail('Unknown synthetic failure kind')
        stdout = b''
        if stage == 'certificate_extract' and self.leaf is not None:
            prefix = command[2].split('=', 1)[1]
            Path(prefix + '0').write_bytes(self.leaf)
        elif stage == 'embedded_profile_decode':
            stdout = self.profile_bytes if self.profile_bytes is not None else plistlib.dumps(self.profile)
        elif stage == 'entitlements_display':
            stdout = self.entitlement_bytes if self.entitlement_bytes is not None else plistlib.dumps(self.entitlements)
        return subprocess.CompletedProcess(command, 0, stdout, PRIVATE_TEXT.encode())

    def invoke(self, *, diagnostics=None):
        self.commands = []
        self.extracted_apps = []
        self.active_diagnostics = diagnostics
        with patch.object(gate.subprocess, 'run', side_effect=self.run_tool):
            return gate.verify_signed(self.archive, self.selection, diagnostics=diagnostics)

    def assert_safe(self, value):
        text = value if isinstance(value, str) else json.dumps(value)
        for secret in (PRIVATE_TEXT, PRIVATE_NAME, CERTIFICATE.decode(), FINGERPRINT,
                       FINGERPRINT.lower(), UUID, str(self.root), str(self.archive)):
            self.assertNotIn(secret, text)
        for path in self.extracted_apps:
            self.assertNotIn(str(path), text)

    def assert_failed(self, stage, reason):
        with self.assertRaises(gate.SignedIpaInspectionError) as caught:
            self.invoke()
        report = caught.exception.report
        self.assert_safe(report)
        self.assert_safe(str(caught.exception))
        self.assertEqual(report['schema'], 1)
        self.assertEqual(report['operation'], 'signed-ipa')
        self.assertEqual(report['status'], 'failed')
        self.assertEqual(report['failed_stage'], stage)
        self.assertEqual(report['reason'], reason)
        checks = report['checks']
        self.assertEqual([item['stage'] for item in checks], list(STAGES[:STAGES.index(stage) + 1]))
        self.assertEqual(checks[:-1], [{'stage': name, 'status': 'passed'} for name in STAGES[:STAGES.index(stage)]])
        self.assertEqual(checks[-1]['status'], 'failed')
        self.assertLessEqual(set(checks[-1]), {'stage', 'status', 'reason', 'return_code',
                                             'error_class', 'stderr_truncated'})
        self.assertEqual([name for name, _ in self.commands],
                         [name for name in TOOL_STAGES if STAGES.index(name) <= STAGES.index(stage)])
        for app in self.extracted_apps:
            self.assertFalse(app.exists(), 'Private extraction must be cleaned after failure')
        return report

    def test_success_retains_every_strict_check_and_reviewed_codesign_argument_forms(self):
        original = self.archive.read_bytes()
        result = self.invoke()
        self.assertEqual(result['inspection'], {
            'schema': 1, 'operation': 'signed-ipa', 'status': 'passed',
            'checks': [{'stage': name, 'status': 'passed'} for name in STAGES],
        })
        self.assertEqual([name for name, _ in self.commands], list(TOOL_STAGES))
        self.assertEqual(result['bundle'], gate.BUNDLE)
        self.assertEqual(result['team'], gate.TEAM)
        self.assertEqual(result['build'], gate.UPLOAD_BUILD)
        self.assertEqual(result['ipa_sha256'], hashlib.sha256(original).hexdigest())
        self.assertEqual(self.archive.read_bytes(), original)
        self.assert_safe(result)
        for app in self.extracted_apps:
            self.assertFalse(app.exists())

    def test_each_tool_nonzero_exit_reports_exact_stage_and_stops_later_checks(self):
        for stage in TOOL_STAGES:
            with self.subTest(stage=stage):
                self.failure_stage, self.failure_kind = stage, 'nonzero'
                report = self.assert_failed(stage, 'command_failed')
                self.assertEqual(report['checks'][-1]['return_code'], 23)
                self.assertEqual(report['checks'][-1]['error_class'], 'unclassified')
                self.assertIs(report['checks'][-1]['stderr_truncated'], False)

    def test_each_tool_timeout_is_sanitized_and_cannot_become_success(self):
        for stage in TOOL_STAGES:
            with self.subTest(stage=stage):
                self.failure_stage, self.failure_kind = stage, 'timeout'
                report = self.assert_failed(stage, 'command_timeout')
                self.assertIsNone(report['checks'][-1]['return_code'])

    def test_each_missing_tool_is_sanitized_and_stops_at_its_stage(self):
        for stage in TOOL_STAGES:
            with self.subTest(stage=stage):
                self.failure_stage, self.failure_kind = stage, 'unavailable'
                report = self.assert_failed(stage, 'command_unavailable')
                self.assertIsNone(report['checks'][-1]['return_code'])

    def test_large_secret_stderr_is_only_reported_as_truncated_metadata(self):
        self.failure_stage, self.failure_kind = 'signature_strict', 'large_stderr'
        report = self.assert_failed('signature_strict', 'command_failed')
        self.assertIs(report['checks'][-1]['stderr_truncated'], True)
        self.assertLess(len(json.dumps(report)), 2000)

    def test_signal_termination_retains_negative_status_without_tool_payloads(self):
        self.failure_stage, self.failure_kind = 'signature_strict', 'signal'
        report = self.assert_failed('signature_strict', 'command_failed')
        self.assertEqual(report['checks'][-1]['return_code'], -9)

    def test_stderr_classifications_are_closed_bounded_metadata_not_excerpts(self):
        examples = {
            b'unrecognized option': 'invalid_option',
            b'no such file or directory': 'missing_target_or_file',
            b'does not satisfy': 'requirement_not_satisfied',
            b'a sealed resource is missing or invalid': 'invalid_resources',
            b'code object is not signed': 'invalid_signature',
            b'unable to build chain': 'trust_or_certificate_diagnostic',
            b'permission denied': 'permission_diagnostic',
        }
        for text, category in examples.items():
            with self.subTest(category=category):
                report = gate.classify_stderr(PRIVATE_TEXT.encode() + b'\n' + text)
                self.assertEqual(report, {'error_class': category, 'stderr_truncated': False})
                self.assert_safe(report)
        beyond_bound = b'x' * gate.STDERR_CLASSIFICATION_LIMIT + b'no such file or directory'
        self.assertEqual(gate.classify_stderr(beyond_bound),
                         {'error_class': 'unclassified', 'stderr_truncated': True})

    def test_missing_extracted_leaf_and_changed_certificate_are_rejected(self):
        for leaf, reason in ((None, 'missing_certificate'), (b'different synthetic leaf', 'certificate_mismatch')):
            with self.subTest(reason=reason):
                self.leaf = leaf
                self.assert_failed('certificate_match', reason)

    def test_embedded_profile_must_retain_selected_uuid_certificate_and_group(self):
        original = copy.deepcopy(self.profile)
        changes = [
            {'UUID': 'DEADBEEF-0123-4567-89AB-0123456789AB'},
            {'DeveloperCertificates': [b'different profile certificate']},
            {'ApplicationIdentifierPrefix': ['OTHERTEAM0'], 'Entitlements': {
                **original['Entitlements'],
                'application-identifier': 'OTHERTEAM0.' + gate.BUNDLE,
                'keychain-access-groups': ['OTHERTEAM0.*'],
            }},
        ]
        for change in changes:
            with self.subTest(field=next(iter(change))):
                self.profile = {**copy.deepcopy(original), **change}
                self.assert_failed('embedded_profile_match', 'profile_mismatch')

    def test_profile_expiry_team_distribution_and_application_scope_guards_remain_strict(self):
        original = copy.deepcopy(self.profile)
        cases = [
            ({'ExpirationDate': datetime(2000, 1, 1)}, 'profile_expired'),
            ({'TeamIdentifier': ['OTHERTEAM0']}, 'team_mismatch'),
            ({'ProvisionedDevices': []}, 'not_app_store_distribution'),
            ({'Entitlements': {**original['Entitlements'], 'application-identifier': 'wrong'}},
             'app_identifier_mismatch'),
        ]
        for change, reason in cases:
            with self.subTest(reason=reason):
                self.profile = {**copy.deepcopy(original), **change}
                self.assert_failed('embedded_profile_match', reason)

    def test_malformed_profile_and_entitlements_payloads_are_not_normalized_or_disclosed(self):
        for data, profile_stage, profile_reason in (
                (b'<plist><dict>' + PRIVATE_TEXT.encode(), 'embedded_profile_decode', 'profile_plist_invalid'),
                (b'not plist ' + PRIVATE_TEXT.encode(), 'embedded_profile_decode', 'profile_plist_invalid'),
                (plistlib.dumps([PRIVATE_TEXT]), 'embedded_profile_match', 'profile_malformed'),
                (plistlib.dumps(PRIVATE_TEXT), 'embedded_profile_match', 'profile_malformed')):
            with self.subTest(kind='profile', data=data[:6]):
                self.profile_bytes = data
                self.assert_failed(profile_stage, profile_reason)
            self.profile_bytes = None
            with self.subTest(kind='entitlements', data=data[:6]):
                self.entitlement_bytes = data
                self.assert_failed('entitlements_match', 'malformed_entitlements')
            self.entitlement_bytes = None
        self.entitlement_bytes = plistlib.dumps(self.entitlements) + b'\x00'
        self.assert_failed('entitlements_match', 'malformed_entitlements')

    def test_exported_entitlements_cannot_broaden_application_or_keychain_scope(self):
        original = copy.deepcopy(self.entitlements)
        for change in ({'application-identifier': 'wrong'},
                       {'com.apple.developer.team-identifier': 'OTHERTEAM0'},
                       {'get-task-allow': True},
                       {'keychain-access-groups': [gate.TEAM + '.*']},
                       {'keychain-access-groups': [self.selection['keychain_group'], 'other']}):
            with self.subTest(field=next(iter(change))):
                self.entitlements = {**original, **change}
                self.assert_failed('entitlements_match', 'entitlements_mismatch')

    def test_real_package_policy_still_blocks_wrong_metadata_privacy_and_executable_assets(self):
        original = self.info.copy()
        for key in ('CFBundleIdentifier', 'CFBundleShortVersionString', 'CFBundleVersion'):
            with self.subTest(field=key):
                self.info = {**original, key: 'wrong'}
                self.make_archive()
                self.assert_failed('package_structure', 'rejected_data')
        self.info = original
        self.make_archive(privacy=False)
        self.assert_failed('package_structure', 'rejected_data')
        self.make_archive(extra=[('Payload/Runner.app/Frameworks/App.framework/flutter_assets/assets/update/hidden.py', b'fixture')])
        self.assert_failed('package_structure', 'rejected_data')

    def test_extraction_and_unique_app_guards_are_not_skipped(self):
        link = zipfile.ZipInfo('Payload/Runner.app/unsafe-link')
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        self.make_archive(extra=[(link, PRIVATE_TEXT.encode())])
        self.assert_failed('safe_extraction', 'rejected_data')
        self.make_archive(extra=[('Payload/Other.app/extra', b'fixture')])
        self.assert_failed('app_selection', 'rejected_data')

    def test_cli_failure_is_machine_readable_safe_json_with_nonzero_exit(self):
        self.failure_stage, self.failure_kind = 'certificate_extract', 'nonzero'
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.object(gate.subprocess, 'run', side_effect=self.run_tool), \
             redirect_stdout(stdout), redirect_stderr(stderr), self.assertRaises(SystemExit) as caught:
            gate.main(['signed-ipa', str(self.archive), str(self.selection_path)])
        self.assertEqual(caught.exception.code, 2)
        report = json.loads(stdout.getvalue())
        self.assertEqual(report['failed_stage'], 'certificate_extract')
        self.assertEqual(report['reason'], 'command_failed')
        self.assert_safe(stdout.getvalue() + stderr.getvalue())

    def test_saved_diagnostics_match_success_and_failure_without_exposing_signing_data(self):
        success_path = self.root / 'success-diagnostics.json'
        result = self.invoke(diagnostics=success_path)
        self.assertEqual(json.loads(success_path.read_text()), result['inspection'])
        self.assert_safe(success_path.read_text())
        failure_path = self.root / 'failure-diagnostics.json'
        self.failure_stage, self.failure_kind = 'entitlements_display', 'timeout'
        with self.assertRaises(gate.SignedIpaInspectionError) as caught:
            self.invoke(diagnostics=failure_path)
        self.assertEqual(json.loads(failure_path.read_text()), caught.exception.report)
        self.assert_safe(failure_path.read_text())
        self.assertFalse(failure_path.with_name(failure_path.name + '.tmp').exists())

    def test_existing_diagnostics_are_preserved_and_prevent_any_tool_execution(self):
        diagnostics = self.root / 'existing-diagnostics.json'
        original = (PRIVATE_TEXT + '\n').encode()
        diagnostics.write_bytes(original)
        with self.assertRaises(gate.SignedIpaInspectionError) as caught:
            self.invoke(diagnostics=diagnostics)
        self.assertEqual(caught.exception.report['reason'], 'diagnostics_write_failed')
        self.assertEqual(caught.exception.report['failed_stage'], 'package_structure')
        self.assertEqual(caught.exception.report['checks'], [])
        self.assertEqual(diagnostics.read_bytes(), original)
        self.assertEqual(self.commands, [])
        self.assert_safe(caught.exception.report)

    def test_simulated_interruption_leaves_current_stage_report_without_success_claim(self):
        diagnostics = self.root / 'interrupted-diagnostics.json'
        self.failure_stage, self.failure_kind = 'certificate_extract', 'interrupted'
        with self.assertRaises(KeyboardInterrupt):
            self.invoke(diagnostics=diagnostics)
        report = json.loads(diagnostics.read_text())
        self.assertEqual(report['status'], 'running')
        self.assertEqual(report['current_stage'], 'certificate_extract')
        self.assertEqual(report['checks'], [
            {'stage': name, 'status': 'passed'} for name in STAGES[:STAGES.index('certificate_extract')]])
        self.assert_safe(report)
        self.assertEqual([name for name, _ in self.commands], list(TOOL_STAGES[:3]))

    def test_setup_and_final_digest_io_failures_are_safe_and_block_success(self):
        with patch.object(gate.tempfile, 'TemporaryDirectory', side_effect=PermissionError(PRIVATE_TEXT)):
            self.assert_failed('inspection_setup', 'io_failed')
        original = Path.read_bytes

        def read_bytes(path):
            if path == self.archive:
                raise PermissionError(PRIVATE_TEXT + ' ' + str(self.archive))
            return original(path)

        with patch.object(Path, 'read_bytes', autospec=True, side_effect=read_bytes):
            self.assert_failed('ipa_digest', 'io_failed')


if __name__ == '__main__':
    unittest.main()

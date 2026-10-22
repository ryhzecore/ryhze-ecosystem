"""Synthetic signing diagnostics; no Apple credentials or signing are exercised."""

from contextlib import redirect_stderr, redirect_stdout
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import testflight_gate as gate


NOW = datetime(2030, 1, 1, tzinfo=timezone.utc)
PRIVATE_NAME = 'PRIVATE_PROFILE_NAME_MUST_NOT_REACH_DIAGNOSTICS'
PRIVATE_TOOL_TEXT = 'PRIVATE_TOOL_OUTPUT_MUST_NOT_REACH_DIAGNOSTICS'
CERTIFICATE = b'PRIVATE_CERTIFICATE_BYTES_MUST_NOT_REACH_DIAGNOSTICS'
FINGERPRINT = hashlib.sha1(CERTIFICATE).hexdigest().upper()
UUID = 'FEEDFACE-0123-4567-89AB-0123456789AB'
PROFILE_DIRECTORIES = (
    'Library/MobileDevice/Provisioning Profiles',
    'Library/Developer/Xcode/UserData/Provisioning Profiles',
)


def profile():
    return {
        'Name': PRIVATE_NAME,
        'UUID': UUID,
        'TeamName': PRIVATE_NAME,
        'TeamIdentifier': [gate.TEAM],
        'Platform': ['iOS'],
        'ExpirationDate': datetime(2090, 1, 1, tzinfo=timezone.utc),
        'ApplicationIdentifierPrefix': [gate.TEAM],
        'DeveloperCertificates': [CERTIFICATE],
        'Entitlements': {
            'application-identifier': gate.TEAM + '.' + gate.BUNDLE,
            'com.apple.developer.team-identifier': gate.TEAM,
            'get-task-allow': False,
            'keychain-access-groups': [gate.TEAM + '.*'],
        },
    }


def identity(fingerprint=FINGERPRINT, name=PRIVATE_NAME):
    return f'  1) {fingerprint} "Apple Distribution: {name}"\n'


class SigningDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='ryhze-synthetic-signing-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.home = self.root / PRIVATE_NAME
        self.home.mkdir()
        self.output = self.root / 'private-selection'
        self.diagnostics = self.root / 'public-diagnostics.json'

    def create_profile(self, filename=PRIVATE_NAME + '.mobileprovision', location=0):
        directory = self.home / PROFILE_DIRECTORIES[location]
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / filename
        path.write_bytes(b'synthetic placeholder, never a real signed profile')
        return path

    def assert_private_values_absent(self, value):
        text = value if isinstance(value, str) else json.dumps(value)
        for private in (PRIVATE_NAME, PRIVATE_TOOL_TEXT, CERTIFICATE.decode(),
                        FINGERPRINT, FINGERPRINT.lower(), UUID, str(self.home),
                        str(self.output), str(self.root)):
            self.assertNotIn(private, text)

    def invoke(self, *, identities=None, decoded=None, command_error=None,
               decode_error=None, diagnostics=True):
        command_options = ({'side_effect': command_error} if command_error is not None
                           else {'return_value': (identity() if identities is None else identities).encode()})
        decode_options = ({'side_effect': decode_error} if decode_error is not None
                          else {'return_value': profile() if decoded is None else decoded})
        with patch.object(gate.Path, 'home', return_value=self.home), \
             patch.object(gate, 'command_bytes', **command_options), \
             patch.object(gate, 'decode_profile', **decode_options):
            return gate.prepare_signing(
                self.output, diagnostics=self.diagnostics if diagnostics else None)

    def read_report(self):
        report = json.loads(self.diagnostics.read_text())
        self.assert_private_values_absent(report)
        allowed = {'schema', 'status', 'reason', 'bundle', 'team', 'locations',
                   'profiles', 'eligible_profiles', 'matching_pairs', 'identities'}
        self.assertEqual(set(report) - {'identities'}, allowed - {'identities'})
        self.assertEqual(report['schema'], 1)
        self.assertEqual(report['bundle'], gate.BUNDLE)
        self.assertEqual(report['team'], gate.TEAM)
        for key in ('eligible_profiles', 'matching_pairs'):
            self.assertIs(type(report[key]), int)
            self.assertGreaterEqual(report[key], 0)
        for location in report['locations']:
            self.assertEqual(set(location), {'location', 'state', 'candidate_count'})
            self.assertIn(location['location'], {'mobiledevice', 'xcode_userdata'})
            self.assertIn(location['state'], {'absent', 'directory', 'not_directory', 'symlink_rejected'})
            self.assertIs(type(location['candidate_count']), int)
        if 'identities' in report:
            self.assertEqual(set(report['identities']), {
                'scope', 'numbered_lines', 'parsed_lines', 'unparsed_lines',
                'non_distribution_lines', 'valid_distribution_identities'})
            self.assertEqual(report['identities']['scope'], 'default_keychain_search_list')
            for key, value in report['identities'].items():
                if key != 'scope':
                    self.assertIs(type(value), int)
                    self.assertGreaterEqual(value, 0)
        for item in report['profiles']:
            self.assertLessEqual(set(item), {'location', 'index', 'status', 'reason',
                                            'matching_identity_count', 'platform_shape'})
            self.assertEqual(set(item) & {'location', 'index', 'status', 'reason'},
                             {'location', 'index', 'status', 'reason'})
            self.assertIn(item['location'], {'mobiledevice', 'xcode_userdata'})
            self.assertIn(item['status'], {'rejected', 'matched'})
            self.assertIs(type(item['index']), int)
            if 'matching_identity_count' in item:
                self.assertIs(type(item['matching_identity_count']), int)
            if 'platform_shape' in item:
                self.assertIn(item['platform_shape'],
                              {'ios_only', 'ios_with_companions', 'unsupported'})
        return report

    def assert_blocked(self, reason, **options):
        with self.assertRaises(gate.SigningGateError) as caught:
            self.invoke(**options)
        self.assertEqual(caught.exception.reason, reason)
        self.assert_private_values_absent(str(caught.exception))
        report = self.read_report()
        self.assertEqual(report['status'], 'blocked')
        self.assertEqual(report['reason'], reason)
        self.assertFalse(self.output.exists())
        return report

    def test_numbered_distribution_identities_normalize_and_ignore_other_classes(self):
        other = 'AB' * 20
        output = ('Policy: Code Signing\n'
                  + identity(FINGERPRINT.lower())
                  + f'  2) {other.lower()} "iPhone Distribution: {PRIVATE_NAME}"\n'
                  + f'  3) {"CD" * 20} "Apple Development: {PRIVATE_NAME}"\n'
                  + '     3 valid identities found\n')
        self.assertEqual(gate.valid_identities(output), {FINGERPRINT, other})
        self.assertEqual(gate.valid_identities(output.replace('\n', '\r\n')), {FINGERPRINT, other})

    def test_identity_parser_rejects_log_embeds_error_suffixes_and_malformed_records(self):
        valid = identity().strip()
        malformed = [
            'diagnostic: ' + valid,
            valid + ' (CSSMERR_TP_CERT_EXPIRED)',
            valid + ' (CSSMERR_TP_CERT_REVOKED)',
            valid + ' ' + PRIVATE_TOOL_TEXT,
            valid + ' # ignored by neither security nor this gate',
            valid + '\r(CSSMERR_TP_CERT_EXPIRED)',
            valid + '\u2028(CSSMERR_TP_CERT_REVOKED)',
            valid + '\u2029' + PRIVATE_TOOL_TEXT,
            valid + '\u0085' + PRIVATE_TOOL_TEXT,
            valid + '\x0b' + PRIVATE_TOOL_TEXT,
            valid + '\x0c' + PRIVATE_TOOL_TEXT,
            valid + '\x7f' + PRIVATE_TOOL_TEXT,
            valid.replace('1)', '-1)', 1),
            valid.replace('1)', '1 )', 1),
            valid.replace('1) ', '', 1),
            valid.replace('1) ', '1)\n', 1),
            valid.replace('1) ', '1)\r', 1),
            valid.replace(PRIVATE_NAME, PRIVATE_NAME + '\nsecond line'),
            valid.replace(PRIVATE_NAME, PRIVATE_NAME + '\rsecond line'),
            valid.replace(FINGERPRINT, FINGERPRINT[:-1]),
            valid.replace(FINGERPRINT, FINGERPRINT + 'A'),
            valid.replace(FINGERPRINT, 'G' + FINGERPRINT[1:]),
            valid.replace('Apple Distribution:', 'Apple Development:'),
            valid.replace('Apple Distribution:', 'Developer ID Application:'),
            valid.replace('Apple Distribution:', 'Untrusted Apple Distribution:'),
            valid[:-1],
        ]
        for output in malformed:
            with self.subTest(record=output):
                self.assertEqual(gate.valid_identities(output), set())

    def test_identity_summary_counts_only_complete_numbered_records(self):
        output = (identity(FINGERPRINT.lower()) + identity()
                  + f'2) {"AB" * 20} "Apple Development: {PRIVATE_NAME}"\n'
                  + identity().strip() + ' (CSSMERR_TP_CERT_EXPIRED)\n'
                  + 'noise: ' + identity() + '0 valid identities found\n')
        identities, metadata = gate.inspect_identities(output)
        self.assertEqual(identities, {FINGERPRINT})
        self.assertEqual(metadata, {
            'scope': 'default_keychain_search_list', 'numbered_lines': 4,
            'parsed_lines': 3, 'unparsed_lines': 1,
            'non_distribution_lines': 1, 'valid_distribution_identities': 1})
        self.assert_private_values_absent(metadata)

    def test_profile_policy_errors_are_specific_and_contain_no_profile_secrets(self):
        edits = [
            ({'TeamIdentifier': [PRIVATE_NAME]}, {}, 'team_mismatch'),
            ({'Platform': [PRIVATE_NAME]}, {}, 'platform_mismatch'),
            ({'ExpirationDate': PRIVATE_NAME}, {}, 'profile_expiry_malformed'),
            ({'ExpirationDate': NOW}, {}, 'profile_expired'),
            ({'Entitlements': PRIVATE_NAME}, {}, 'entitlements_malformed'),
            ({}, {'com.apple.developer.team-identifier': PRIVATE_NAME}, 'entitlement_team_mismatch'),
            ({}, {'get-task-allow': True}, 'not_app_store_distribution'),
            ({}, {'get-task-allow': 0}, 'not_app_store_distribution'),
            ({'ProvisionedDevices': []}, {}, 'not_app_store_distribution'),
            ({'ProvisionsAllDevices': True}, {}, 'not_app_store_distribution'),
            ({'ApplicationIdentifierPrefix': [PRIVATE_NAME]}, {}, 'app_identifier_prefix_malformed'),
            ({'UUID': PRIVATE_NAME}, {}, 'profile_uuid_malformed'),
            ({}, {'application-identifier': gate.TEAM + '.*'}, 'app_identifier_mismatch'),
            ({}, {'keychain-access-groups': [PRIVATE_NAME]}, 'keychain_group_mismatch'),
            ({'DeveloperCertificates': []}, {}, 'certificates_missing'),
            ({'DeveloperCertificates': [PRIVATE_NAME]}, {}, 'certificates_missing'),
        ]
        for platforms in (['iOS'], ['iOS', 'xrOS', 'visionOS']):
            for fields, entitlements, reason in edits:
                with self.subTest(platforms=platforms, reason=reason, fields=list(fields)):
                    value = profile()
                    value['Platform'] = platforms
                    value.update(fields)
                    if entitlements:
                        value['Entitlements'].update(entitlements)
                    with self.assertRaises(gate.SigningGateError) as caught:
                        gate.profile_details(value, NOW)
                    self.assertEqual(caught.exception.reason, reason)
                    self.assert_private_values_absent(str(caught.exception))
        with self.assertRaises(gate.SigningGateError) as caught:
            gate.profile_details(PRIVATE_NAME, NOW)
        self.assertEqual(caught.exception.reason, 'profile_malformed')

    def test_supported_ios_companion_platforms_do_not_accept_unknown_or_malformed_values(self):
        supported = [['iOS'], ['iOS', 'xrOS'], ['xrOS', 'iOS'],
                     ['iOS', 'visionOS'], ['visionOS', 'iOS', 'xrOS']]
        for platforms in supported:
            with self.subTest(platforms=platforms):
                details = gate.profile_details({**profile(), 'Platform': platforms}, NOW)
                self.assertEqual(details['certificates'], {FINGERPRINT})
                self.assertEqual(details['keychain_group'], gate.TEAM + '.' + gate.BUNDLE)
        for platforms in (None, 'iOS', [], ['xrOS'], ['visionOS'], ['ios'],
                          ['iOS', 'iOS'], ['iOS', 'OSX'], ['iOS', PRIVATE_NAME],
                          ['iOS', None], ['iOS', []], ('iOS',), {'iOS'}):
            with self.subTest(platforms=platforms), self.assertRaises(gate.SigningGateError) as caught:
                gate.profile_details({**profile(), 'Platform': platforms}, NOW)
            self.assertEqual(caught.exception.reason, 'platform_mismatch')

    def test_success_writes_public_metadata_and_separate_private_signing_inputs(self):
        self.create_profile()
        result = self.invoke()
        report = self.read_report()
        self.assertEqual(report['status'], 'selected')
        self.assertEqual(report['reason'], 'matching_existing_pair_selected')
        self.assertEqual(report['eligible_profiles'], 1)
        self.assertEqual(report['matching_pairs'], 1)
        self.assertEqual(report['identities']['valid_distribution_identities'], 1)
        self.assertEqual(report['profiles'][0]['reason'], 'matched')
        self.assertEqual([item['state'] for item in report['locations']], ['directory', 'absent'])
        self.assertEqual(result['distribution'], 'internal TestFlight only')
        selection = json.loads((self.output / 'selection.json').read_text())
        options = plistlib.loads((self.output / 'export-options.plist').read_bytes())
        self.assertEqual(selection['profile_uuid'], UUID)
        self.assertEqual(selection['certificate'], FINGERPRINT)
        self.assertEqual(options['provisioningProfiles'], {gate.BUNDLE: UUID})
        self.assertTrue(options['testFlightInternalTestingOnly'])

    def test_valid_profile_without_matching_private_key_is_diagnosed_without_selection(self):
        self.create_profile(location=1)
        report = self.assert_blocked('no_qualifying_signing_pair', identities=identity('AB' * 20))
        self.assertEqual(report['eligible_profiles'], 1)
        self.assertEqual(report['matching_pairs'], 0)
        self.assertEqual(report['profiles'][0]['reason'], 'no_matching_valid_distribution_identity')
        self.assertEqual(report['profiles'][0]['matching_identity_count'], 0)
        self.assertEqual([item['state'] for item in report['locations']], ['absent', 'directory'])

    def test_missing_profiles_and_zero_identities_are_distinguishable(self):
        report = self.assert_blocked('no_qualifying_signing_pair', identities='0 valid identities found\n')
        self.assertEqual(report['identities']['valid_distribution_identities'], 0)
        self.assertEqual(report['profiles'], [])
        self.assertEqual([item['state'] for item in report['locations']], ['absent', 'absent'])

    def test_identity_tool_failure_timeout_and_invalid_utf8_always_emit_safe_diagnostics(self):
        errors = [ValueError(PRIVATE_TOOL_TEXT), OSError(PRIVATE_TOOL_TEXT),
                  subprocess.TimeoutExpired(
                      [PRIVATE_NAME], 60, output=PRIVATE_TOOL_TEXT.encode(), stderr=PRIVATE_TOOL_TEXT.encode())]
        for error in errors:
            with self.subTest(error=type(error).__name__):
                report = self.assert_blocked('identity_inspection_failed', command_error=error)
                self.assertNotIn('identities', report)
                self.assertEqual(report['profiles'], [])
                self.assertEqual(report['locations'], [])
                self.diagnostics.unlink()
        with patch.object(gate.Path, 'home', return_value=self.home), \
             patch.object(gate, 'command_bytes', return_value=b'\xff' + PRIVATE_TOOL_TEXT.encode()), \
             self.assertRaises(gate.SigningGateError) as caught:
            gate.prepare_signing(self.output, diagnostics=self.diagnostics)
        self.assertEqual(caught.exception.reason, 'identity_inspection_failed')
        self.assertEqual(self.read_report()['reason'], 'identity_inspection_failed')

    def test_profile_decode_failures_are_counted_without_raw_exception_text(self):
        self.create_profile()
        for error, reason in (
                (gate.SigningGateError('profile_decode_command_failed'), 'profile_decode_command_failed'),
                (gate.SigningGateError('profile_plist_invalid'), 'profile_plist_invalid'),
                (ValueError(PRIVATE_TOOL_TEXT), 'profile_inspection_failed'),
                (OSError(PRIVATE_TOOL_TEXT), 'profile_inspection_failed'),
                (plistlib.InvalidFileException(PRIVATE_TOOL_TEXT), 'profile_inspection_failed')):
            with self.subTest(reason=reason):
                report = self.assert_blocked('no_qualifying_signing_pair', decode_error=error)
                self.assertEqual(report['profiles'][0]['reason'], reason)
                self.assertEqual(report['eligible_profiles'], 0)
                self.diagnostics.unlink()

    def test_mixed_rejected_and_matching_profiles_keep_only_counts_and_safe_reasons(self):
        rejected = self.create_profile('00-' + PRIVATE_NAME + '.mobileprovision')
        accepted = self.create_profile('10-' + PRIVATE_NAME + '.mobileprovision', location=1)

        def decode(path):
            if path == rejected:
                raise ValueError(PRIVATE_TOOL_TEXT)
            self.assertEqual(path, accepted)
            return profile()

        self.invoke(decode_error=decode)
        report = self.read_report()
        self.assertEqual(report['status'], 'selected')
        self.assertEqual(report['eligible_profiles'], 1)
        self.assertEqual(report['matching_pairs'], 1)
        self.assertEqual([item['reason'] for item in report['profiles']],
                         ['profile_inspection_failed', 'matched'])

    def test_decode_profile_masks_tool_errors_and_invalid_plist_payloads(self):
        path = self.home / (PRIVATE_NAME + '.mobileprovision')
        for error in (ValueError(PRIVATE_TOOL_TEXT), OSError(PRIVATE_TOOL_TEXT),
                      subprocess.TimeoutExpired(PRIVATE_NAME, 60, output=PRIVATE_TOOL_TEXT)):
            with self.subTest(error=type(error).__name__), \
                 patch.object(gate, 'command_bytes', side_effect=error), \
                 self.assertRaises(gate.SigningGateError) as caught:
                gate.decode_profile(path)
            self.assertEqual(caught.exception.reason, 'profile_decode_command_failed')
            self.assert_private_values_absent(str(caught.exception))
        with patch.object(gate, 'command_bytes', return_value=PRIVATE_TOOL_TEXT.encode()), \
             self.assertRaises(gate.SigningGateError) as caught:
            gate.decode_profile(path)
        self.assertEqual(caught.exception.reason, 'profile_plist_invalid')
        self.assert_private_values_absent(str(caught.exception))

    def test_unsafe_profile_location_emits_only_its_fixed_location_label(self):
        directory = self.home / PROFILE_DIRECTORIES[0]
        directory.parent.mkdir(parents=True)
        target = self.root / PRIVATE_TOOL_TEXT
        target.mkdir()
        directory.symlink_to(target, target_is_directory=True)
        report = self.assert_blocked('profile_directory_unsafe')
        self.assertEqual(report['locations'], [
            {'location': 'mobiledevice', 'state': 'symlink_rejected', 'candidate_count': 0}])
        self.assertEqual(list(target.iterdir()), [])

    def test_unsafe_profile_file_is_not_decoded_or_selected(self):
        path = self.create_profile()
        path.unlink()
        target = self.root / PRIVATE_TOOL_TEXT
        target.write_bytes(b'synthetic untouched target')
        path.symlink_to(target)
        with patch.object(gate.Path, 'home', return_value=self.home), \
             patch.object(gate, 'command_bytes', return_value=identity().encode()), \
             patch.object(gate, 'decode_profile') as decode, \
             self.assertRaises(gate.SigningGateError):
            gate.prepare_signing(self.output, diagnostics=self.diagnostics)
        decode.assert_not_called()
        report = self.read_report()
        self.assertEqual(report['profiles'][0]['reason'], 'profile_file_unsafe')
        self.assertFalse(self.output.exists())
        self.assertEqual(target.read_bytes(), b'synthetic untouched target')

    def test_failed_selection_io_still_writes_safe_public_diagnostics(self):
        self.create_profile()
        self.output.write_bytes(b'preexisting private output must remain untouched')
        with self.assertRaises(gate.SigningGateError) as caught:
            self.invoke()
        self.assertEqual(caught.exception.reason, 'signing_io_failed')
        self.assert_private_values_absent(str(caught.exception))
        self.assertEqual(self.read_report()['reason'], 'signing_io_failed')
        self.assertEqual(self.output.read_bytes(), b'preexisting private output must remain untouched')

    def test_optional_diagnostics_keeps_existing_private_selection_api_compatible(self):
        self.create_profile()
        self.invoke(diagnostics=False)
        self.assertTrue((self.output / 'selection.json').is_file())
        self.assertFalse(self.diagnostics.exists())

    def test_unknown_error_reasons_cannot_inject_private_values_into_diagnostics(self):
        for reason in (PRIVATE_TOOL_TEXT, {'private': PRIVATE_NAME},
                       [PRIVATE_NAME], None, 5):
            with self.subTest(value_type=type(reason).__name__):
                error = gate.SigningGateError(reason)
                self.assertEqual(error.reason, 'signing_inspection_failed')
                self.assert_private_values_absent(str(error))
        self.create_profile()
        report = self.assert_blocked(
            'no_qualifying_signing_pair', decode_error=gate.SigningGateError(PRIVATE_TOOL_TEXT))
        self.assertEqual(report['profiles'][0]['reason'], 'signing_inspection_failed')

    def test_actual_malformed_xml_profile_is_skipped_before_valid_profile_selection(self):
        rejected = self.create_profile('00-' + PRIVATE_NAME + '.mobileprovision')
        accepted = self.create_profile('10-' + PRIVATE_NAME + '.mobileprovision')
        malformed = ('<?xml version="1.0"?><plist><dict><key>'
                     + PRIVATE_TOOL_TEXT + '</dict></plist>').encode()
        valid_plist = plistlib.dumps(profile())
        commands = []

        def command_bytes(command):
            commands.append(command)
            if command == ['security', 'find-identity', '-v', '-p', 'codesigning']:
                return identity().encode()
            self.assertEqual(command[:4], ['security', 'cms', '-D', '-i'])
            if command[4] == str(rejected):
                return malformed
            self.assertEqual(command[4], str(accepted))
            return valid_plist

        with patch.object(gate.Path, 'home', return_value=self.home), \
             patch.object(gate, 'command_bytes', side_effect=command_bytes):
            gate.prepare_signing(self.output, diagnostics=self.diagnostics)
        report = self.read_report()
        self.assertEqual(report['status'], 'selected')
        self.assertEqual(report['eligible_profiles'], 1)
        self.assertEqual([item['reason'] for item in report['profiles']],
                         ['profile_plist_invalid', 'matched'])
        self.assertEqual(len(commands), 3)

    def test_diagnostics_refuse_existing_file_or_symlink_without_exposing_path(self):
        original = b'prior diagnostic evidence must not be overwritten'
        self.diagnostics.write_bytes(original)
        with self.assertRaises(gate.SigningGateError) as caught:
            self.invoke(identities='0 valid identities found')
        self.assertEqual(caught.exception.reason, 'diagnostics_write_failed')
        self.assert_private_values_absent(str(caught.exception))
        self.assertEqual(self.diagnostics.read_bytes(), original)
        self.diagnostics.unlink()
        target = self.root / PRIVATE_TOOL_TEXT
        target.write_bytes(original)
        self.diagnostics.symlink_to(target)
        with self.assertRaises(gate.SigningGateError) as caught:
            self.invoke(identities='0 valid identities found')
        self.assertEqual(caught.exception.reason, 'diagnostics_write_failed')
        self.assert_private_values_absent(str(caught.exception))
        self.assertTrue(self.diagnostics.is_symlink())
        self.assertEqual(target.read_bytes(), original)
        self.assertFalse(self.output.exists())

    def test_public_cli_failure_reports_safe_reason_and_writes_failure_artifact(self):
        stdout, stderr = io.StringIO(), io.StringIO()
        arguments = ['testflight_gate.py', 'signing', str(self.output),
                     '--diagnostics', str(self.diagnostics)]
        error = subprocess.TimeoutExpired(
            [PRIVATE_NAME], 60, output=PRIVATE_TOOL_TEXT.encode(), stderr=PRIVATE_TOOL_TEXT.encode())
        with patch.object(sys, 'argv', arguments), \
             patch.object(gate.Path, 'home', return_value=self.home), \
             patch.object(gate, 'command_bytes', side_effect=error), \
             redirect_stdout(stdout), redirect_stderr(stderr), \
             self.assertRaises(SystemExit) as caught:
            gate.main()
        self.assertEqual(caught.exception.code, 2)
        self.assertEqual(stdout.getvalue(), '')
        self.assertIn('identity_inspection_failed', stderr.getvalue())
        self.assert_private_values_absent(stderr.getvalue())
        self.assertEqual(self.read_report()['reason'], 'identity_inspection_failed')
        self.assertFalse(self.output.exists())

    def test_public_cli_diagnostic_write_failure_masks_filesystem_exception(self):
        self.diagnostics = self.root / PRIVATE_NAME / 'missing-parent' / 'public.json'
        stdout, stderr = io.StringIO(), io.StringIO()
        arguments = ['testflight_gate.py', 'signing', str(self.output),
                     '--diagnostics', str(self.diagnostics)]
        with patch.object(sys, 'argv', arguments), \
             patch.object(gate.Path, 'home', return_value=self.home), \
             patch.object(gate, 'command_bytes', return_value=b'0 valid identities found'), \
             redirect_stdout(stdout), redirect_stderr(stderr), \
             self.assertRaises(SystemExit) as caught:
            gate.main()
        self.assertEqual(caught.exception.code, 2)
        self.assertEqual(stdout.getvalue(), '')
        self.assertIn('diagnostics_write_failed', stderr.getvalue())
        self.assert_private_values_absent(stderr.getvalue())
        self.assertFalse(self.output.exists())
        self.assertFalse(self.diagnostics.exists())


if __name__ == '__main__':
    unittest.main()

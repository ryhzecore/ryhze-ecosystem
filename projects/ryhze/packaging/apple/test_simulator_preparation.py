"""Synthetic simulator orchestration; no Xcode, signing or Apple services run."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import check_native_tests
import select_simulator


RUNTIME = 'com.apple.CoreSimulator.SimRuntime.iOS-26-5'
UDID = '01234567-89AB-CDEF-0123-456789ABCDEF'
OTHER_UDID = '01234567-89AB-CDEF-0123-456789ABCDE0'
NATIVE_METHODS = {
    'core': {
        'testKeychainRoundTripAndScopeIsolation',
        'testDurableSnapshotReopensAndRejectsStaleWrite',
        'testUnsupportedSchemaIsPreserved', 'testStateSymlinksAreRejected',
        'testAtomicMigrationPreservesBackupAndReopens',
        'testInterruptedMigrationRollsBackBothWrites',
        'testFutureDocumentSchemaIsPreserved',
    },
    'seed': {'testKeychainSeed'},
    'restore': {'testKeychainRestoreAfterProcessRestart'},
}


def enumeration(**changes):
    device = {'isAvailable': True, 'name': 'iPhone synthetic',
              'state': 'Booted', 'udid': UDID}
    device.update(changes)
    return {'devices': {RUNTIME: [device]}}


FAKE_TOOL = r'''
import base64, json, os, pathlib, re, shutil, sys, time
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
root = pathlib.Path(os.environ['RYHZE_SIMULATOR_FIXTURE_ROOT'])
mode = os.environ['RYHZE_SIMULATOR_FIXTURE_MODE']
with (root / 'calls.jsonl').open('a') as trace:
    trace.write(json.dumps([name, *args]) + '\n')
if name == 'python3':
    if args[:1] == ['packaging/apple/testflight_gate.py']:
        if args[1] == 'signing':
            output = pathlib.Path(args[2]); output.mkdir(parents=True)
            (output / 'selection.json').write_text(json.dumps({
                'certificate': 'A' * 40,
                'profile_uuid': '01234567-89AB-CDEF-0123-456789ABCDEF'}))
            (output / 'export-options.plist').write_text('synthetic')
            pathlib.Path(args[args.index('--diagnostics') + 1]).write_text('{}')
        print('{}'); sys.exit(0)
    if args[:1] == ['packaging/apple/runner_signing.py']:
        if args[1] == 'generate':
            pathlib.Path(args[3]).write_text('// Synthetic Runner-only signing fixture\n')
        elif args[1] == 'verify-build-settings':
            json.loads(pathlib.Path(args[3]).read_text())
        else:
            raise RuntimeError('Unexpected Runner signing command')
        print(json.dumps({'status': 'passed', 'synthetic': True})); sys.exit(0)
    if args[:2] == ['packaging/apple/simulator_ui_readiness.py', 'wait'] and mode == 'ui_timeout':
        # Inject only the hard-to-wait-for status; other modes exercise the real
        # readiness parser against synthetic request, launch and marker bytes.
        report = {'state': 'blocked', 'reason': 'readiness_timeout_starting', 'synthetic': True}
        pathlib.Path(args[args.index('--report') + 1]).write_text(json.dumps(report))
        print(json.dumps(report)); sys.exit(124)
    if args[:2] == ['-m', 'unittest']:
        if mode == 'budget_write_failure' and 'packaging/windows' in args:
            (root / 'build/apple-testflight/simulator-devices-1-budget.json').mkdir()
        print('Synthetic packaging suite placeholder'); sys.exit(0)
    if args[:1] == ['packaging/windows/component_inventory.py']:
        print('Synthetic component inventory placeholder'); sys.exit(0)
    if args[:1] == ['-c'] and 'source_version' in args[1]:
        print('0.6.0'); sys.exit(0)
    python = os.environ['RYHZE_SIMULATOR_FIXTURE_PYTHON']
    os.execv(python, [python, *args])
elif name == 'uname': print('Darwin')
elif name == 'git':
    if args[:1] == ['rev-parse']: print('a' * 40)
elif name == 'flutter':
    if '--version' in args:
        print(json.dumps({'frameworkVersion': '3.47.6',
            'frameworkRevision': '5fc346839b5d0eef006ed8404392afb4dfae428d',
            'dartSdkVersion': '3.13.5', 'flutterRoot': str(root)}))
    elif 'deps' in args: print('{"packages": []}')
elif name == 'xcodebuild':
    if args == ['-version']: print('Xcode 26.6\nBuild version synthetic')
    elif args[:2] == ['-showBuildSettings', '-json']:
        print(json.dumps([{'target': 'Runner', 'buildSettings': {
            'SYNTHETIC_PRIVATE_SIGNING_VALUE': 'private-settings-sentinel'}}]))
        print('private-settings-stderr-sentinel', file=sys.stderr)
    elif args[0] == 'test':
        phase = pathlib.Path(args[args.index('-resultBundlePath') + 1]).stem.removeprefix('ios-')
        if phase == os.environ.get('RYHZE_SIMULATOR_FIXTURE_NATIVE_FAILURE_PHASE'):
            print('Synthetic native command failure; no Swift execution')
            sys.exit(int(os.environ['RYHZE_SIMULATOR_FIXTURE_NATIVE_FAILURE_STATUS']))
        if mode == 'restore_pid_assertion' and phase == 'restore':
            print('Synthetic same-process PID assertion failure; no Swift execution')
            print("Test Case '-[RunnerTests.RunnerTests testKeychainRestoreAfterProcessRestart]' failed")
            print('** TEST FAILED **')
            sys.exit(1)
        omitted = os.environ.get('RYHZE_SIMULATOR_FIXTURE_OMIT', '')
        for arg in args:
            if arg.startswith('-only-testing:'):
                method = arg.rsplit('/', 1)[1]
                if method != omitted:
                    print("Test Case '-[RunnerTests.RunnerTests " + method + "]' passed")
        print('** TEST SUCCEEDED **')
    elif args[0] == '-exportArchive':
        output = pathlib.Path(args[args.index('-exportPath') + 1])
        output.mkdir(parents=True)
        (output / 'Ryhze.ipa').write_bytes(b'synthetic unverified IPA')
elif name == 'xcrun':
    if args == ['simctl', 'list', 'devices', 'available', '-j']:
        counter = root / 'list-count.txt'
        attempt = int(counter.read_text()) + 1 if counter.exists() else 1
        counter.write_text(str(attempt))
        print('Synthetic CoreSimulator diagnostic stays out of JSON', file=sys.stderr)
        if mode == 'final_timeout' or (mode in ('cold', 'cold_budget') and attempt == 1):
            print('{"devices": partial timed-out JSON')
            if mode == 'cold_budget': time.sleep(1.1)
            sys.exit(124)
        if mode == 'list_failure': sys.exit(77)
        if mode == 'malformed': print('{malformed'); sys.exit(0)
        if mode == 'no_runtime': print('{"devices": {}}'); sys.exit(0)
        state = 'Shutdown' if mode == 'shutdown' else 'Booted'
        print(json.dumps({'devices': {'com.apple.CoreSimulator.SimRuntime.iOS-26-5': [{
            'isAvailable': True, 'name': 'iPhone synthetic', 'state': state,
            'udid': '01234567-89AB-CDEF-0123-456789ABCDEF'}]}}))
    elif args[:2] == ['simctl', 'bootstatus']:
        if mode == 'ready_timeout': sys.exit(124)
    elif args[:2] == ['simctl', 'terminate']:
        # Poison this removed cold-service request without sleeping. Even if a
        # future script ignores the timeout, trace assertions still reject it.
        print('Unexpected standalone termination request', file=sys.stderr)
        sys.exit(124)
    elif args == ['simctl', 'get_app_container',
                  '01234567-89AB-CDEF-0123-456789ABCDEF', 'com.ryhze.ryhze', 'data']:
        directory = root / 'simulator-container'
        (directory / 'Library/Caches').mkdir(parents=True)
        print(directory)
    elif args == ['simctl', 'launch', '--terminate-running-process',
                  '01234567-89AB-CDEF-0123-456789ABCDEF', 'com.ryhze.ryhze']:
        nonce = os.environ['SIMCTL_CHILD_RYHZE_FIRST_FRAME_NONCE']
        if re.fullmatch('[0-9a-f]{32}', nonce) is None:
            raise RuntimeError('Expected launch-bound nonce')
        marker = {'schemaVersion': 1, 'nonce': nonce, 'processID': 4242,
                  'state': 'ready', 'bootstrap': 'healthy', 'shellLayout': True,
                  'firstFrameRasterized': True, 'failureCode': None}
        if mode == 'ui_failure':
            marker.update(state='failed', bootstrap='degraded', failureCode='bootstrap-degraded')
        elif mode == 'ui_timeout':
            marker.update(state='starting', bootstrap='pending', shellLayout=False,
                          firstFrameRasterized=False)
        (root / 'simulator-container/Library/Caches/ryhze-first-frame-v1.json').write_text(json.dumps(marker))
        print('com.ryhze.ryhze: 4242')
    elif len(args) == 5 and args[:4] == ['simctl', 'io',
            '01234567-89AB-CDEF-0123-456789ABCDEF', 'screenshot']:
        # Valid fixture PNG bytes, not a rendered Flutter screenshot.
        pathlib.Path(args[-1]).write_bytes(base64.b64decode(
            'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl6nWQAAAAASUVORK5CYII='))
        if mode == 'ui_capture_failure':
            marker_path = root / 'simulator-container/Library/Caches/ryhze-first-frame-v1.json'
            marker = json.loads(marker_path.read_text())
            marker.update(state='failed', failureCode='flutter-error')
            marker_path.write_text(json.dumps(marker))
    else:
        raise RuntimeError('Unexpected extra service query, app-state probe or simulator mutation')
elif name == 'app-store-connect': print('0.70.0 synthetic')
elif name == 'sw_vers': print('synthetic macOS')
elif name == 'codesign': print('synthetic entitlements')
elif name == 'cp':
    if mode == 'publish_copy_failure':
        pathlib.Path(args[-1]).write_bytes(b'synthetic partial copied IPA')
        sys.exit(76)
    shutil.copyfile(args[0], args[1])
elif name == 'mv':
    pathlib.Path(args[0]).rename(args[1])
    if mode == 'publish_rename_failure': sys.exit(76)
elif name != 'dart': raise RuntimeError('Unexpected synthetic command: ' + name)
'''


class SimulatorSelectionTests(unittest.TestCase):
    def test_selects_installed_booted_iphone_deterministically(self):
        value = enumeration(state='Shutdown')
        preferred = {**value['devices'][RUNTIME][0], 'state': 'Booted',
                     'udid': OTHER_UDID, 'name': 'iPhone second'}
        value['devices'][RUNTIME].append(preferred)
        expected = {'runtime': RUNTIME, 'udid': OTHER_UDID,
                    'name': 'iPhone second', 'state': 'Booted'}
        self.assertEqual(select_simulator.select_device(value), expected)
        value['devices'][RUNTIME].reverse()
        self.assertEqual(select_simulator.select_device(value), expected)
        for state in ('Booted', 'Shutdown', 'Booting'):
            self.assertEqual(select_simulator.select_device(enumeration(state=state))['state'], state)
        value = enumeration(state='Shutdown')
        value['devices'][RUNTIME].append({**preferred, 'state': 'Booting'})
        value['devices'][RUNTIME].append({**preferred, 'state': 'Shutting Down', 'name': 'iPhone first'})
        self.assertEqual(select_simulator.select_device(value)['state'], 'Booting')

    def test_rejects_missing_malformed_or_unavailable_runtime_and_device_data(self):
        values = [None, [], {}, {'devices': []}, {'devices': {}},
                  {'devices': {RUNTIME: {}}}, {'devices': {RUNTIME: [None]}}]
        for runtime in ('iOS-26', 'not-' + RUNTIME,
                        'com.apple.CoreSimulator.SimRuntime.tvOS-26-6',
                        'com.apple.CoreSimulator.SimRuntime.iOS-not-a-version'):
            values.append({'devices': {runtime: enumeration()['devices'][RUNTIME]}})
        for changes in ({'name': None}, {'name': 'iPad synthetic'}, {'udid': '--create'},
                        {'udid': 'synthetic-udid'}, {'udid': None}, {'state': 'Unknown'},
                        {'state': 'Shutting Down'},
                        {'state': []}, {'isAvailable': False}, {'isAvailable': 1},
                        {'isAvailable': 'true'}, {'isAvailable': None}):
            values.append(enumeration(**changes))
        for value in values:
            with self.subTest(value=value), self.assertRaises(ValueError):
                select_simulator.select_device(value)

    def test_all_nine_native_gate_methods_are_required_even_with_success_banner(self):
        self.assertEqual(sum(map(len, NATIVE_METHODS.values())), 9)
        for phase, methods in NATIVE_METHODS.items():
            for missing in methods:
                text = '\n'.join(
                    f"Test Case '-[RunnerTests.RunnerTests {method}]' passed"
                    for method in methods - {missing}) + '\n** TEST SUCCEEDED **\n'
                with self.subTest(phase=phase, missing=missing), self.assertRaises(ValueError):
                    check_native_tests.verify(phase, text)


class SimulatorPreparationTests(unittest.TestCase):
    def run_fixture(self, mode='normal', *, omitted='', budget=None,
                    native_failure_phase='', native_failure_status=1):
        temporary = tempfile.TemporaryDirectory(prefix='ryhze-simulator-pipeline-')
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name).resolve()
        sources = Path(__file__).parent
        directory = root / 'packaging/apple'
        directory.mkdir(parents=True)
        for name in ('prepare_testflight.sh', 'run_bounded.py', 'check_sdk.py',
                     'check_native_tests.py', 'select_simulator.py', 'simulator_ui_readiness.py'):
            shutil.copyfile(sources / name, directory / name)
        if budget is not None:
            # Test-only copy: exercise the real deadline arithmetic without a
            # production environment override or a 55-minute test delay.
            script = directory / 'prepare_testflight.sh'
            source = script.read_text()
            original = 'deadline=$((SECONDS + 3300))'
            self.assertEqual(source.count(original), 1)
            script.write_text(source.replace(original, f'deadline=$((SECONDS + {budget}))'))
        binary = root / 'bin'
        binary.mkdir()
        for name in ('python3', 'uname', 'git', 'flutter', 'xcodebuild', 'xcrun',
                     'app-store-connect', 'sw_vers', 'codesign', 'dart', 'cp', 'mv',
                     'launchctl', 'pgrep', 'ps'):
            target = binary / name
            target.write_text('#!' + sys.executable + '\n' + FAKE_TOOL)
            target.chmod(0o755)
        environment = {
            **os.environ, 'PATH': str(binary) + os.pathsep + os.environ['PATH'],
            'RYHZE_FLUTTER': str(binary / 'flutter'),
            'RYHZE_SIMULATOR_FIXTURE_ROOT': str(root),
            'RYHZE_SIMULATOR_FIXTURE_MODE': mode,
            'RYHZE_SIMULATOR_FIXTURE_OMIT': omitted,
            'RYHZE_SIMULATOR_FIXTURE_NATIVE_FAILURE_PHASE': native_failure_phase,
            'RYHZE_SIMULATOR_FIXTURE_NATIVE_FAILURE_STATUS': str(native_failure_status),
            'RYHZE_SIMULATOR_FIXTURE_PYTHON': sys.executable,
        }
        result = subprocess.run(['bash', str(directory / 'prepare_testflight.sh')],
                                env=environment, capture_output=True, text=True, timeout=30)
        trace = root / 'calls.jsonl'
        calls = [json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []
        self.assertFalse(any(call[:3] == ['xcrun', 'simctl', 'terminate'] for call in calls))
        self.assertFalse(any(call[0] in {'launchctl', 'pgrep', 'ps'} for call in calls))
        for call in calls:
            if call[0] != 'xcrun':
                continue
            self.assertTrue(
                call == ['xcrun', 'simctl', 'list', 'devices', 'available', '-j']
                or call == ['xcrun', 'simctl', 'bootstatus', UDID, '-b']
                or call == ['xcrun', 'simctl', 'get_app_container', UDID, 'com.ryhze.ryhze', 'data']
                or call == ['xcrun', 'simctl', 'launch', '--terminate-running-process', UDID, 'com.ryhze.ryhze']
                or (len(call) == 6 and call[:5] == ['xcrun', 'simctl', 'io', UDID, 'screenshot']),
                f'Unexpected service call or app-state probe: {call}')
        self.assertFalse(list((root / 'build/apple-testflight').glob('simulator-stop*-budget.json')))
        return root, result, calls

    def budget(self, root, phase):
        return json.loads((root / f'build/apple-testflight/{phase}-budget.json').read_text())

    def assert_no_native_or_publish(self, root, result, calls):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('no validated IPA', result.stderr)
        self.assertFalse(any(call[:2] in (['xcodebuild', 'test'], ['xcodebuild', 'archive']) for call in calls))
        self.assertFalse(list((root / 'build/apple-testflight').glob('publish/*')))

    def test_cold_enumeration_retries_once_uses_complete_second_json_and_runs_all_gates(self):
        root, result, calls = self.run_fixture('cold')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lists = [call for call in calls if call[:3] == ['xcrun', 'simctl', 'list']]
        self.assertEqual(lists, [['xcrun', 'simctl', 'list', 'devices', 'available', '-j']] * 2)
        evidence = root / 'build/apple-testflight'
        self.assertIn('partial timed-out JSON', (evidence / 'simulator-devices-1.json').read_text())
        self.assertEqual(json.loads((evidence / 'simulator-selection.json').read_text())['udid'], UDID)
        parser_calls = [call for call in calls if call[:2] == ['python3', 'packaging/apple/select_simulator.py']]
        self.assertEqual(len(parser_calls), 1)
        self.assertTrue(parser_calls[0][2].endswith('/simulator-devices-2.json'))
        for phase in ('simulator-devices-1', 'simulator-devices-2'):
            self.assertEqual(self.budget(root, phase), {
                'requested_seconds': 180, 'allowed_seconds': 180, 'cleanup_reserve_seconds': 30})
        self.assertEqual([call for call in calls if call[:3] == ['xcrun', 'simctl', 'bootstatus']],
                         [['xcrun', 'simctl', 'bootstatus', UDID, '-b']])
        self.assertEqual(self.budget(root, 'simulator-ready'), {
            'requested_seconds': 300, 'allowed_seconds': 300, 'cleanup_reserve_seconds': 30})
        native = [call for call in calls if call[:2] == ['xcodebuild', 'test']]
        self.assertEqual(len(native), 3)
        first_native = next(index for index, call in enumerate(calls)
                            if call[:2] == ['xcodebuild', 'test'])
        precore_simctl = [call for call in calls[:first_native] if call[0] == 'xcrun']
        self.assertEqual(precore_simctl, lists + [['xcrun', 'simctl', 'bootstatus', UDID, '-b']])
        configurations = [index for index, call in enumerate(calls)
                          if call[0] == 'flutter' and 'build' in call
                          and '--simulator' in call and '--config-only' in call]
        self.assertEqual(len(configurations), 1)
        self.assertLess(configurations[0], first_native)
        previous_checker = -1
        for call, phase in zip(native, ('core', 'seed', 'restore')):
            actual = {arg.rsplit('/', 1)[-1] for arg in call if arg.startswith('-only-testing:')}
            self.assertEqual(actual, NATIVE_METHODS[phase])
            self.assertIn('platform=iOS Simulator,id=' + UDID, call)
            self.assertEqual(call[call.index('-parallel-testing-enabled') + 1], 'NO')
            checker = ['python3', 'packaging/apple/check_native_tests.py', phase]
            checker_positions = [index for index, entry in enumerate(calls) if entry[:3] == checker]
            self.assertEqual(len(checker_positions), 1)
            native_position = calls.index(call)
            self.assertLess(previous_checker, native_position)
            self.assertLess(native_position, checker_positions[0])
            previous_checker = checker_positions[0]
            self.assertEqual(self.budget(root, 'ios-' + phase), {
                'requested_seconds': 900, 'allowed_seconds': 900, 'cleanup_reserve_seconds': 30})
        # Use the real host readiness checker with synthetic marker/PNG bytes.
        # This proves ordering and launch binding, not actual Flutter rendering.
        ordered = [
            ['python3', 'packaging/apple/simulator_ui_readiness.py', 'prepare'],
            ['xcrun', 'simctl', 'get_app_container'],
            ['xcrun', 'simctl', 'launch'],
            ['python3', 'packaging/apple/simulator_ui_readiness.py', 'wait'],
            ['xcrun', 'simctl', 'io', UDID, 'screenshot'],
            ['python3', 'packaging/apple/simulator_ui_readiness.py', 'capture'],
            ['python3', 'packaging/apple/runner_signing.py', 'generate'],
            ['xcodebuild', '-showBuildSettings', '-json'],
            ['python3', 'packaging/apple/runner_signing.py', 'verify-build-settings'],
            ['xcodebuild', 'archive'],
        ]
        previous = previous_checker
        for prefix in ordered:
            positions = [index for index, call in enumerate(calls) if call[:len(prefix)] == prefix]
            self.assertEqual(len(positions), 1, prefix)
            self.assertLess(previous, positions[0], prefix)
            previous = positions[0]
        private = root / 'build/apple-testflight-private'
        request = json.loads((private / 'ui-request.json').read_text())
        marker = json.loads((root / 'simulator-container/Library/Caches/ryhze-first-frame-v1.json').read_text())
        self.assertEqual(request['nonce'], marker['nonce'])
        self.assertEqual(marker['processID'], 4242)
        self.assertIn('com.ryhze.ryhze: 4242', (evidence / 'simulator-launch.log').read_text())
        wait = next(call for call in calls if call[:3] == ordered[3])
        capture = next(call for call in calls if call[:3] == ordered[5])
        self.assertEqual(wait[3:6], capture[3:6])
        self.assertEqual(wait[3:6], [str(private / 'ui-request.json'),
                                    str(evidence / 'simulator-launch.log'),
                                    str(private / 'simulator-container.txt')])
        self.assertEqual(wait[wait.index('--timeout') + 1], '80')
        for path in ('simulator-first-frame.json', 'simulator-capture.json'):
            report = json.loads((evidence / path).read_text())
            self.assertEqual(report['state'], 'ready')
            self.assertTrue(report['launchBound'])
            self.assertTrue(report['visualReviewRequired'])
            self.assertFalse(report['physicalDeviceAcceptance'])
        for phase, cap in (('simulator-probe-request', 30), ('simulator-container', 30),
                           ('simulator-launch', 30), ('simulator-first-frame', 90),
                           ('simulator-screenshot', 30), ('simulator-capture-check', 30),
                           ('runner-signing', 30), ('archive-build-settings', 120),
                           ('runner-signing-check', 30), ('archive', 900)):
            self.assertEqual(self.budget(root, phase), {
                'requested_seconds': cap, 'allowed_seconds': cap, 'cleanup_reserve_seconds': 30})
        for call in calls:
            self.assertNotIn('-allowProvisioningUpdates', call)
            self.assertNotIn('-allowProvisioningDeviceRegistration', call)
            if call[:2] in (['xcodebuild', 'archive'], ['xcodebuild', '-showBuildSettings']):
                self.assertFalse(any(arg.startswith(('CODE_SIGN', 'DEVELOPMENT_TEAM=', 'PROVISIONING_PROFILE'))
                                     for arg in call))
        self.assertIn('private-settings-sentinel', (private / 'archive-build-settings.json').read_text())
        self.assertIn('private-settings-stderr-sentinel', (private / 'archive-build-settings.stderr').read_text())
        for path in evidence.glob('*.log'):
            self.assertNotIn('private-settings-', path.read_text())
        self.assertTrue((evidence / 'publish/Ryhze-internal-37.ipa').is_file())

    def test_final_timeout_and_non_timeout_enumeration_failure_abort_without_selection(self):
        for mode, expected_status, attempts in (('final_timeout', 124, 2), ('list_failure', 77, 1)):
            with self.subTest(mode=mode):
                root, result, calls = self.run_fixture(mode)
                self.assertEqual(result.returncode, expected_status)
                self.assert_no_native_or_publish(root, result, calls)
                self.assertEqual(sum(call[:3] == ['xcrun', 'simctl', 'list'] for call in calls), attempts)
                self.assertFalse((root / 'build/apple-testflight/simulator-selection.json').exists())
                self.assertFalse(any(call[:2] == ['python3', 'packaging/apple/select_simulator.py'] for call in calls))

    def test_malformed_json_or_no_installed_runtime_abort_without_retry_or_readiness(self):
        for mode in ('malformed', 'no_runtime'):
            with self.subTest(mode=mode):
                root, result, calls = self.run_fixture(mode)
                self.assertEqual(result.returncode, 2)
                self.assert_no_native_or_publish(root, result, calls)
                self.assertEqual(sum(call[:3] == ['xcrun', 'simctl', 'list'] for call in calls), 1)
                self.assertFalse(any(call[:3] == ['xcrun', 'simctl', 'bootstatus'] for call in calls))

    def test_readiness_is_bounded_and_uses_selected_device_without_another_query_or_boot(self):
        root, result, calls = self.run_fixture('shutdown')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual([call for call in calls if call[:3] == ['xcrun', 'simctl', 'bootstatus']],
                         [['xcrun', 'simctl', 'bootstatus', UDID, '-b']])
        self.assertEqual(self.budget(root, 'simulator-ready'), {
            'requested_seconds': 300, 'allowed_seconds': 300, 'cleanup_reserve_seconds': 30})
        self.assertEqual(sum(call[:3] == ['xcrun', 'simctl', 'list'] for call in calls), 1)
        forbidden = {'boot', 'create', 'erase', 'delete', 'runtime'}
        self.assertFalse(any(call[0] == 'xcrun' and len(call) > 2 and call[2] in forbidden for call in calls))

    def test_readiness_timeout_aborts_before_native_gates(self):
        root, result, calls = self.run_fixture('ready_timeout')
        self.assertEqual(result.returncode, 124)
        self.assert_no_native_or_publish(root, result, calls)

    def test_native_failure_timeout_and_cleanup_failure_abort_later_phases_without_retry(self):
        # Synthetic xcodebuild statuses test orchestration only. Actual process
        # cleanup is exercised in test_run_bounded.py; no Swift is executed here.
        phases = ('core', 'seed', 'restore')
        for index, (phase, status) in enumerate(zip(phases, (1, 124, 125))):
            with self.subTest(phase=phase, status=status):
                root, result, calls = self.run_fixture(
                    native_failure_phase=phase, native_failure_status=status)
                self.assertEqual(result.returncode, status, result.stdout + result.stderr)
                native = [call for call in calls if call[:2] == ['xcodebuild', 'test']]
                actual_phases = [Path(call[call.index('-resultBundlePath') + 1]).stem.removeprefix('ios-')
                                 for call in native]
                self.assertEqual(actual_phases, list(phases[:index + 1]))
                validators = [call[2] for call in calls
                              if call[:2] == ['python3', 'packaging/apple/check_native_tests.py']]
                self.assertEqual(validators, list(phases[:index]))
                self.assertFalse(any(call[:2] == ['xcodebuild', 'archive'] for call in calls))
                self.assertFalse(list((root / 'build/apple-testflight').glob('publish/*')))
                self.assertIn('no validated IPA', result.stderr)

    def test_synthetic_same_process_restore_assertion_failure_blocks_ipa(self):
        root, result, calls = self.run_fixture('restore_pid_assertion')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('Synthetic same-process PID assertion failure; no Swift execution', result.stdout)
        native = [call for call in calls if call[:2] == ['xcodebuild', 'test']]
        self.assertEqual(len(native), 3)
        self.assertFalse(any(call[:3] == ['python3', 'packaging/apple/check_native_tests.py', 'restore']
                             for call in calls))
        self.assertFalse(any(call[:2] == ['xcodebuild', 'archive'] for call in calls))
        self.assertFalse(list((root / 'build/apple-testflight').glob('publish/*')))

    def test_failed_ui_readiness_timeout_or_lost_readiness_blocks_archive_and_publish(self):
        for mode, status in (('ui_failure', 2), ('ui_timeout', 124), ('ui_capture_failure', 2)):
            with self.subTest(mode=mode):
                root, result, calls = self.run_fixture(mode)
                self.assertEqual(result.returncode, status, result.stdout + result.stderr)
                native = [call for call in calls if call[:2] == ['xcodebuild', 'test']]
                self.assertEqual(len(native), 3)
                self.assertEqual([call[2] for call in calls
                                  if call[:2] == ['python3', 'packaging/apple/check_native_tests.py']],
                                 ['core', 'seed', 'restore'])
                waits = [call for call in calls
                         if call[:3] == ['python3', 'packaging/apple/simulator_ui_readiness.py', 'wait']]
                self.assertEqual(len(waits), 1)
                screenshots = [call for call in calls
                               if call[:5] == ['xcrun', 'simctl', 'io', UDID, 'screenshot']]
                self.assertEqual(len(screenshots), int(mode == 'ui_capture_failure'))
                self.assertFalse(any(call[:2] in (['xcodebuild', 'archive'], ['xcodebuild', '-exportArchive'])
                                     for call in calls))
                self.assertFalse(list((root / 'build/apple-testflight').glob('publish/*')))
                self.assertIn('no validated IPA', result.stderr)

    def test_missing_required_native_pass_in_each_phase_blocks_archive(self):
        for missing in ('testInterruptedMigrationRollsBackBothWrites', 'testKeychainSeed',
                        'testKeychainRestoreAfterProcessRestart'):
            with self.subTest(missing=missing):
                root, result, calls = self.run_fixture(omitted=missing)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(call[:2] == ['xcodebuild', 'archive'] for call in calls))
                self.assertFalse(list((root / 'build/apple-testflight').glob('publish/*')))

    def test_shared_deadline_keeps_cleanup_reserve_and_retry_does_not_reset_budget(self):
        # Leave enough synthetic command-startup headroom on a cold Mac runner;
        # the assertions still prove that 30 seconds stay outside command caps.
        root, result, calls = self.run_fixture('cold_budget', budget=70)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        first = self.budget(root, 'simulator-devices-1')
        second = self.budget(root, 'simulator-devices-2')
        self.assertLessEqual(first['allowed_seconds'], 40)
        self.assertLess(second['allowed_seconds'], first['allowed_seconds'])
        self.assertGreater(second['allowed_seconds'], 0)
        for path in (root / 'build/apple-testflight').glob('*-budget.json'):
            value = json.loads(path.read_text())
            self.assertEqual(value['cleanup_reserve_seconds'], 30)
            self.assertLessEqual(value['allowed_seconds'], 40)
            self.assertLessEqual(value['allowed_seconds'], value['requested_seconds'])
        root, result, calls = self.run_fixture(budget=30)
        self.assertEqual(result.returncode, 124)
        self.assert_no_native_or_publish(root, result, calls)
        self.assertEqual(self.budget(root, 'sdk')['allowed_seconds'], 0)
        self.assertFalse(any(call[0] == 'flutter' for call in calls))

    def test_failed_budget_receipt_write_prevents_even_conditional_enumeration(self):
        root, result, calls = self.run_fixture('budget_write_failure')
        self.assert_no_native_or_publish(root, result, calls)
        self.assertFalse(any(call[0] == 'xcrun' for call in calls))
        self.assertTrue((root / 'build/apple-testflight/simulator-devices-1-budget.json').is_dir())
        self.assertFalse((root / 'build/apple-testflight/simulator-devices-2-budget.json').exists())

    def test_failed_bounded_publish_removes_partial_and_final_ipa(self):
        for mode in ('publish_copy_failure', 'publish_rename_failure'):
            with self.subTest(mode=mode):
                root, result, calls = self.run_fixture(mode)
                self.assertEqual(result.returncode, 76, result.stdout + result.stderr)
                self.assertTrue(any(call[:2] == ['xcodebuild', 'archive'] for call in calls))
                self.assertTrue((root / 'build/apple-testflight-private/export/Ryhze.ipa').is_file())
                self.assertFalse(list((root / 'build/apple-testflight/publish').glob('*')))
                self.assertEqual(self.budget(root, 'publish-artifact'), {
                    'requested_seconds': 30, 'allowed_seconds': 30, 'cleanup_reserve_seconds': 30})


if __name__ == '__main__':
    unittest.main()

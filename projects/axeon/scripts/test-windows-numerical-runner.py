#!/usr/bin/env python3
"""Native helper-backed numerical Runner regressions; no builds or WSL.

Run after host admission using a qualified --helper supporting CPU/accounting
options, and a fresh --output-dir below a reserved dated Evidence attempt. The
timeout case intentionally runs the complete 30-second child deadline. Synthetic
accounting/admission/budget failures are labelled separately from native calls.
This file is outside numerics-tests/test_*.py; it does not alter the 20 oracles.
A genuine host admission hold stops the remaining cases with status resource_hold
and exit75. The deliberately caught synthetic hold remains a passing test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import time
import traceback
from unittest.mock import patch


ROOT = Path(__file__).absolute().parent.parent
MODULE = runpy.run_path(str(ROOT / 'numerics-tests' / 'windows_runner.py'))
LAYOUT = runpy.run_path(str(ROOT / 'scripts' / 'reproduce-windows.py'))
Runner = MODULE['Runner']
FIXTURE = r'''
import os, sys, threading, time
mode = sys.argv[1]
if mode == 'streams':
    sys.stdout.buffer.write(sys.stdin.buffer.read())
    sys.stderr.buffer.write(b'diagnostic\r\n')
elif mode == 'emit':
    def emit(stream, count, byte):
        while count:
            piece = byte * min(count, 65536)
            stream.write(piece)
            stream.flush()
            count -= len(piece)
    first = threading.Thread(target=emit, args=(sys.stdout.buffer, int(sys.argv[2]), b'o'))
    second = threading.Thread(target=emit, args=(sys.stderr.buffer, int(sys.argv[3]), b'e'))
    first.start(); second.start(); first.join(); second.join()
elif mode == 'invalid-utf8':
    (sys.stdout.buffer if sys.argv[2] == 'stdout' else sys.stderr.buffer).write(b'\xff')
elif mode == 'status':
    sys.stdout.buffer.write(b'preserved output\r\n')
    sys.stderr.buffer.write(b'preserved error\r\n')
    sys.exit(70)
elif mode == 'timeout':
    sys.stdout.buffer.write(b'before timeout\r\n'); sys.stdout.buffer.flush()
    time.sleep(60)
else:
    raise ValueError(mode)
'''


def require(value, message):
    if not value:
        raise AssertionError(message)


def put(path, data):
    LAYOUT['safe_path'](path)
    with path.open('xb') as stream:
        stream.write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    LAYOUT['native_host']()
    require(args.helper.is_absolute(), 'absolute explicitly qualified helper path required')
    helper = LAYOUT['regular'](args.helper)
    root = LAYOUT['fresh'](LAYOUT['evidence_location'](args.output_dir))
    fixture = root / 'runner fixture \u96ea.py'
    put(fixture, FIXTURE.encode('utf-8'))
    recipes = {str(path): LAYOUT['sha'](path) for path in (
        Path(__file__), ROOT / 'numerics-tests' / 'windows_runner.py',
        ROOT / 'scripts' / 'record-windows-tests.py', ROOT / 'scripts' / 'reproduce-windows.py')}
    receipt = {'schema_version': 1, 'status': 'running', 'helper': str(helper),
               'helper_sha256': LAYOUT['sha'](helper), 'recipe_sha256': recipes,
               'native_windows': True, 'wsl_invoked': False, 'compiler_execution': False,
               'python': sys.version, 'tests': []}
    result_path = root / 'runner-tests.json'

    def save():
        LAYOUT['write_json'](result_path, receipt)

    def case(label, body, synthetic=False):
        directory = LAYOUT['fresh'](root / label)
        test = {'name': label, 'status': 'running', 'synthetic_failure_fixture': synthetic,
                'commands': []}
        receipt['tests'].append(test)
        save()
        runner = None
        try:
            with patch.dict(os.environ, {'AXEON_PROCESS_TEST': str(helper)}):
                runner = Runner(directory, test)
                body(runner, directory, test)
            test['status'] = 'passed'
        except BaseException as error:
            held = runner is not None and isinstance(error, runner.api['ResourceHold'])
            test.update(status='resource_hold' if held else 'failed', error=f'{type(error).__name__}: {error}')
            put(directory / ('resource-hold.txt' if held else 'failure.txt'), traceback.format_exc().encode('utf-8'))
        LAYOUT['write_json'](directory / 'test.json', test)
        save()
        return test['status'] == 'passed'

    def legacy_cpp_name(runner, directory, test):
        result = runner.call('c++-fixtures-O0', [sys.executable, '-B', fixture, 'streams'], b'v1 unchanged\n')
        require(result == 'v1 unchanged\n', 'legacy C++ command label changed its bytes')
        require((directory / 'c++-fixtures-O0.command.json').is_file(), 'legacy C++ receipt name changed')

    def invoke(runner, *args, stdin=None, expected_error=None):
        try:
            value = runner.call('child', [sys.executable, '-B', fixture, *args], stdin)
        except Exception as error:
            if isinstance(error, runner.api['ResourceHold']):
                raise  # Admission is a real stop condition, never an expected command-failure assertion.
            require(expected_error is not None and expected_error in str(error),
                    f'unexpected failure: {type(error).__name__}: {error}')
            value = None
        else:
            require(expected_error is None, 'expected bounded Runner rejection')
        entry = runner.receipt['commands'][-1]
        saved = json.loads((runner.output / 'child.command.json').read_text('utf-8'))
        require(saved == entry, 'in-memory and saved command receipts differ')
        require((runner.output / 'child.started.json').is_file(), 'started receipt missing')
        for kind, limit in (('stdout', MODULE['STDOUT_LIMIT']), ('stderr', MODULE['STDERR_LIMIT'])):
            raw = (runner.output / ('child.' + kind)).read_bytes()
            require(len(raw) <= limit, 'retained stream exceeds its fixed quota')
            require(hashlib.sha256(raw).hexdigest() == entry[kind + '_sha256'], 'raw stream hash differs')
        return value, entry

    def raw(runner, directory, test):
        data = b'raw\x00bytes\r\n\n' + 'caf\u00e9 \u96ea'.encode()
        value, entry = invoke(runner, 'streams', stdin=data)
        require(value.encode('utf-8') == data, 'stdout newline or byte normalization occurred')
        require((directory / 'child.stderr').read_bytes() == b'diagnostic\r\n', 'stderr changed')
        require(entry['status'] == 'passed' and entry['exit_status'] == 0, 'normal status differs')
        job = entry['job_accounting']
        require(job['cleanup_succeeded'] and job['cpu_seconds'] == 30, 'native helper limits differ')
        require(job['supervisor_pid'] == entry['supervisor_pid'] and job['child_created']
                and job['accounting_valid'], 'native helper accounting identity differs')
        require(runner.child_cpu() == (job['user_cpu_ticks'] + job['kernel_cpu_ticks']) / 10_000_000,
                'aggregate CPU does not include reported user and kernel time')

    def boundary(runner, directory, test, stdout_bytes, stderr_bytes, overflow):
        value, entry = invoke(runner, 'emit', str(stdout_bytes), str(stderr_bytes),
                              expected_error='output budget exceeded' if overflow else None)
        for kind, amount, limit, byte in (
            ('stdout', stdout_bytes, MODULE['STDOUT_LIMIT'], b'o'),
            ('stderr', stderr_bytes, MODULE['STDERR_LIMIT'], b'e')):
            captured = (directory / ('child.' + kind)).read_bytes()
            require(captured == byte * min(amount, limit), f'{kind} retained prefix differs')
            if amount > limit:
                require(entry['streams'][kind]['overflow'], f'{kind} overflow was not recorded')
        require((value is None) == overflow, 'output boundary status differs')

    def status(runner, directory, test):
        _, entry = invoke(runner, 'status', expected_error='status 70')
        require(entry['exit_status'] == 70 and entry['job_accounting']['exit_code'] == 70, 'trap status changed')
        require((directory / 'child.stdout').read_bytes() == b'preserved output\r\n', 'failure output lost')

    def timeout(runner, directory, test):
        start = time.monotonic()
        _, entry = invoke(runner, 'timeout', expected_error='status 124')
        require(29 <= time.monotonic() - start <= 45, 'full 30-second timeout or cleanup bound differs')
        require(entry['exit_status'] == 124 and entry['job_accounting']['cleanup_succeeded'], 'timeout cleanup unproven')
        require((directory / 'child.stdout').read_bytes() == b'before timeout\r\n', 'timeout prefix lost')

    def malformed(runner, directory, test, mutation):
        value = {'schema_version': 1, 'cpu_seconds': 30, 'wall_seconds': 30, 'memory_bytes': 2 << 30,
                 'active_process_limit': 256, 'user_cpu_ticks': 100, 'kernel_cpu_ticks': 200,
                 'exit_code': 0, 'cleanup_succeeded': True, 'accounting_valid': True,
                 'child_created': True, 'limits_applied': True, 'active_processes': 0,
                 'supervisor_pid': 999, 'child_pid': 1000}
        path = directory / 'synthetic-accounting.json'
        if mutation != 'missing':
            if mutation == 'malformed':
                data = b'{not JSON}'
            elif mutation == 'duplicate':
                data = b'{"schema_version":1,"schema_version":1}'
            else:
                field, replacement = mutation
                value[field] = replacement
                data = json.dumps(value).encode()
            put(path, data)
        try:
            runner._accounting(path, 0, 999)
        except Exception as error:
            test['expected_rejection'] = f'{type(error).__name__}: {error}'
        else:
            raise AssertionError('invalid synthetic accounting was accepted')
        require(not test['commands'], 'synthetic accounting test unexpectedly launched a child')

    def lost_accounting(runner, directory, test):
        missing = directory / 'intentionally-absent-accounting.json'
        read_accounting = runner._accounting
        # Keep the real helper receipt untouched; inject a missing input only at the reader boundary.
        with patch.object(runner, '_accounting', side_effect=lambda _path, code, pid: read_accounting(missing, code, pid)):
            invoke(runner, 'streams', expected_error='intentionally-absent-accounting.json')
        require(not runner.receipt['windows_runner']['aggregate_cpu_accounting_complete'],
                'missing accounting did not invalidate the aggregate CPU measurement')
        require((directory / 'child.accounting.json').is_file(), 'real helper evidence was discarded')
        with patch.object(subprocess, 'Popen', side_effect=AssertionError('lost accounting launched another child')):
            try:
                runner.call('followup', [sys.executable, '-B', fixture, 'streams'])
            except RuntimeError as error:
                require('accounting is incomplete' in str(error), 'unexpected lost-accounting rejection')
            else:
                raise AssertionError('runner reused incomplete aggregate accounting')

    def final_cpu_overrun(runner, directory, test):
        read_accounting = runner._accounting
        def inject(path, code, pid):
            actual, _ = read_accounting(path, code, pid)
            synthetic = dict(actual, user_cpu_ticks=30 * 10_000_000 + 1, kernel_cpu_ticks=0)
            fixture_path = directory / 'synthetic-final-cpu-overrun.json'
            put(fixture_path, json.dumps(synthetic).encode())
            return read_accounting(fixture_path, code, pid)
        with patch.object(runner, '_accounting', side_effect=inject):
            _, entry = invoke(runner, 'streams', expected_error='measured job CPU exceeds')
        require(entry['exit_status'] == 0 and runner.child_cpu() > 30,
                'final CPU excess was not charged and rejected after a zero child exit')
        require((directory / 'child.accounting.json').is_file(), 'original native accounting was discarded')

    def budget(runner, directory, test, dimension):
        if dimension == 'wall':
            runner.started -= 181
        elif dimension == 'cpu':
            runner._cpu_ticks = 181 * 10_000_000
        else:
            runner._accounting_complete = False
        with patch.object(subprocess, 'Popen', side_effect=AssertionError('budget failure launched a subprocess')):
            try:
                runner.call('rejected', [sys.executable, '-B', fixture, 'streams'])
            except RuntimeError as error:
                require('aggregate' in str(error), 'unexpected aggregate rejection')
            else:
                raise AssertionError('aggregate budget failure was accepted')
        require(test['commands'][-1]['status'] == 'failed', 'aggregate failure receipt missing')

    def launch(runner, directory, test):
        with patch.object(subprocess, 'Popen', side_effect=OSError('synthetic launch failure')):
            invoke(runner, 'streams', expected_error='synthetic launch failure')
        require(test['commands'][-1]['exit_status'] is None, 'launch failure fabricated an exit status')

    def hold(runner, directory, test):
        observed = runner.api['admission'](directory)
        observed['ready'] = False
        observed['free_physical_bytes'] = 0
        with patch.dict(runner.api, {'admission': lambda _: observed}), patch.object(
                subprocess, 'Popen', side_effect=AssertionError('resource hold launched a subprocess')):
            try:
                runner.call('held', [sys.executable, '-B', fixture, 'streams'])
            except runner.api['ResourceHold']:
                pass
            else:
                raise AssertionError('resource hold was not preserved')
        require(test['commands'][-1]['status'] == 'resource_hold', 'hold receipt missing')

    save()
    try:
        cases = [
            ('raw-streams-and-accounting', raw, False),
            ('legacy-cpp-command-label', legacy_cpp_name, False),
            ('stdout-exact-boundary', lambda r, d, t: boundary(r, d, t, 8 << 20, 0, False), False),
            ('stderr-exact-boundary', lambda r, d, t: boundary(r, d, t, 0, 1 << 20, False), False),
            ('concurrent-streams', lambda r, d, t: boundary(r, d, t, 4 << 20, 512 << 10, False), False),
            ('stdout-overflow', lambda r, d, t: boundary(r, d, t, (8 << 20) + 65536, 0, True), False),
            ('stderr-overflow', lambda r, d, t: boundary(r, d, t, 0, (1 << 20) + 65536, True), False),
            ('runtime-status', status, False),
            ('strict-stdout-utf8', lambda r, d, t: invoke(r, 'invalid-utf8', 'stdout', expected_error='utf-8'), False),
            ('strict-stderr-utf8', lambda r, d, t: invoke(r, 'invalid-utf8', 'stderr', expected_error='utf-8'), False),
            ('full-child-timeout', timeout, False),
            ('helper-launch-failure', launch, True),
            ('resource-hold', hold, True),
            ('lost-accounting-stops-followup', lost_accounting, True),
            ('final-cpu-overrun-after-zero-exit', final_cpu_overrun, True),
        ]
        for dimension in ('wall', 'cpu', 'missing-accounting'):
            cases.append(('aggregate-' + dimension, lambda r, d, t, field=dimension: budget(r, d, t, field), True))
        mutations = [('missing', 'missing'), ('malformed', 'malformed'), ('duplicate', 'duplicate'),
                     ('wrong-cpu', ('cpu_seconds', 180)), ('wrong-memory', ('memory_bytes', 4 << 30)),
                     ('wrong-status', ('exit_code', 70)), ('cleanup-failed', ('cleanup_succeeded', False)),
                     ('boolean-ticks', ('user_cpu_ticks', True)), ('negative-ticks', ('kernel_cpu_ticks', -1)),
                     ('oversized-ticks', ('user_cpu_ticks', 1 << 64)), ('wrong-schema', ('schema_version', 2)),
                     ('invalid-accounting', ('accounting_valid', False)), ('child-not-created', ('child_created', False)),
                     ('limits-not-applied', ('limits_applied', False)), ('active-process', ('active_processes', 1)),
                     ('wrong-supervisor', ('supervisor_pid', 123)), ('missing-child', ('child_pid', 0))]
        for label, mutation in mutations:
            cases.append(('accounting-' + label, lambda r, d, t, change=mutation: malformed(r, d, t, change), True))
        for label, body, synthetic in cases:
            if not case(label, body, synthetic):
                break  # Preserve the first failure/hold; do not continue an inadmissible run.
        require(LAYOUT['sha'](helper) == receipt['helper_sha256'], 'helper changed during regressions')
        require(all(LAYOUT['sha'](path) == value for path, value in recipes.items()), 'regression inputs changed')
        receipt['expected_tests'] = len(cases)
        receipt['passed'] = sum(item['status'] == 'passed' for item in receipt['tests'])
        held = next((item for item in receipt['tests'] if item['status'] == 'resource_hold'), None)
        if held is not None:
            receipt.update(status='resource_hold', error=held['error'], blocked_case=held['name'])
        else:
            receipt['status'] = 'passed' if receipt['passed'] == len(cases) else 'failed'
    except BaseException as error:
        receipt.update(status='failed', error=f'{type(error).__name__}: {error}')
    finally:
        save()
    print(json.dumps({'status': receipt['status'], 'passed': receipt.get('passed'),
                      'expected_tests': receipt.get('expected_tests'), 'receipt': str(result_path)}))
    return 0 if receipt['status'] == 'passed' else 75 if receipt['status'] == 'resource_hold' else 1


if __name__ == '__main__':
    sys.exit(main())

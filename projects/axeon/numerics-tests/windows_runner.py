"""Bounded Windows adapter for the unchanged numerical oracle Runner API.

The admitted caller must select its qualified native helper using the absolute
AXEON_PROCESS_TEST path. There is no PATH fallback or WSL invocation. The helper
must implement --cpu-seconds and --accounting; its initial hash is pinned for the
whole run. Each call uses a fresh accounting path and preserves raw byte streams.
Job CPU polling can overshoot between samples: final measured CPU above 30s is
still charged to the aggregate and fails the command, even when its child exits0.

Windows job committed-memory limits are not POSIX address-space limits. Stream
prefix quotas are not a quota on arbitrary files written by a compiler or child.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import threading
import time


ROOT = Path(__file__).absolute().parent.parent
STDOUT_LIMIT = 8 << 20
STDERR_LIMIT = 1 << 20
CHILD_SECONDS = 30
AGGREGATE_SECONDS = 180
CLEANUP_SECONDS = 10
TICKS_PER_SECOND = 10_000_000
LIMITS = {"child_wall_seconds": CHILD_SECONDS, "child_job_cpu_seconds": CHILD_SECONDS,
          "child_job_committed_memory_bytes": 2 << 30, "active_process_limit": 256,
          "stdout_prefix_bytes": STDOUT_LIMIT, "stderr_prefix_bytes": STDERR_LIMIT,
          "aggregate_wall_seconds": AGGREGATE_SECONDS,
          "aggregate_reported_job_cpu_seconds": AGGREGATE_SECONDS,
          "helper_cleanup_allowance_seconds": CLEANUP_SECONDS,
          "posix_address_space_limit": False, "arbitrary_file_size_quota": False}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class Runner:
    def __init__(self, output, receipt):
        self.started = time.monotonic()
        self._cpu_ticks = 0
        self.cpu_started = self.child_cpu()
        self._accounting_complete = True
        self.receipt = receipt
        self.api = runpy.run_path(str(ROOT / 'scripts' / 'record-windows-tests.py'))
        layout = self.api['API']
        layout['native_host']()
        self.output = layout['evidence_location'](output)
        require(self.output.is_dir(), 'existing dated Evidence output directory required')
        selected = os.environ.get('AXEON_PROCESS_TEST', '')
        require(selected and Path(selected).is_absolute(),
                'AXEON_PROCESS_TEST must explicitly select an absolute qualified Windows helper')
        self.helper = layout['regular'](selected)
        self.helper_sha256 = layout['sha'](self.helper)
        require(isinstance(receipt.get('commands'), list), 'numerical command receipt list required')
        receipt['windows_runner'] = {
            'schema_version': 1, 'helper': str(self.helper), 'helper_sha256': self.helper_sha256,
            'helper_selection': 'AXEON_PROCESS_TEST; qualification supplied by caller',
            'runner_sha256': layout['sha'](__file__),
            'admission_recipe_sha256': layout['sha'](ROOT / 'scripts' / 'record-windows-tests.py'),
            'path_recipe_sha256': layout['sha'](ROOT / 'scripts' / 'reproduce-windows.py'),
            'limits': dict(LIMITS), 'wsl_invoked_by_runner': False,
            'aggregate_cpu_accounting_complete': True}

    def child_cpu(self):
        return self._cpu_ticks / TICKS_PER_SECOND

    def check_budget(self):
        require(self._accounting_complete, 'aggregate child-CPU accounting is incomplete; stop this run')
        if (time.monotonic() - self.started > AGGREGATE_SECONDS
                or self.child_cpu() - self.cpu_started > AGGREGATE_SECONDS):
            raise RuntimeError('aggregate 180-second wall/child-CPU budget exceeded')

    def _new_json(self, path, value):
        self.api['safe'](path)
        with path.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(value, stream, indent=2, ensure_ascii=True, allow_nan=False)
            stream.write('\n')

    def _accounting(self, path, exit_status, supervisor_pid):
        """Only complete, correctly bounded helper accounting contributes CPU."""
        self.api['regular'](path, 16 << 10)
        before = self.api['sha'](path)
        value = self.api['load'](path)
        require(isinstance(value, dict), 'helper accounting must be an object')
        expected = {'schema_version': 1, 'cpu_seconds': CHILD_SECONDS, 'wall_seconds': CHILD_SECONDS,
                    'memory_bytes': 2 << 30, 'active_process_limit': 256}
        for field, target in expected.items():
            require(type(value.get(field)) is int and value[field] == target,
                    f'helper accounting {field} differs')
        for field in ('user_cpu_ticks', 'kernel_cpu_ticks', 'exit_code'):
            require(type(value.get(field)) is int and value[field] >= 0,
                    f'invalid helper accounting {field}')
        require(all(value[field] <= 0x7fffffffffffffff for field in ('user_cpu_ticks', 'kernel_cpu_ticks')),
                'helper CPU accounting exceeds signed 64-bit ticks')
        require(value['exit_code'] <= 0xffffffff and value['exit_code'] == (exit_status & 0xffffffff),
                'helper accounting exit status differs')
        require(value.get('cleanup_succeeded') is True, 'helper did not establish owned-job cleanup')
        require(value.get('accounting_valid') is True and value.get('child_created') is True
                and value.get('limits_applied') is True,
                'helper did not establish valid accounting for a started child')
        require(type(value.get('active_processes')) is int and value['active_processes'] == 0,
                'helper accounting still has active owned processes')
        require(type(value.get('supervisor_pid')) is int and value['supervisor_pid'] == supervisor_pid,
                'helper accounting supervisor identity differs')
        require(type(value.get('child_pid')) is int and 0 < value['child_pid'] <= 0xffffffff,
                'helper accounting child identity is absent')
        require(self.api['sha'](path) == before, 'helper accounting changed while reading')
        return value, before

    def call(self, name, command, input_data=None):
        require(isinstance(name, str) and re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_+-]*', name),
                'safe single-component command name required')
        command = list(map(str, command))
        require(command and all('\0' not in value for value in command), 'nonempty command without NUL required')
        require(input_data is None or isinstance(input_data, bytes), 'subprocess input must be raw bytes')
        paths = {kind: self.output / f'{name}.{suffix}' for kind, suffix in (
            ('stdout', 'stdout'), ('stderr', 'stderr'), ('accounting', 'accounting.json'),
            ('started', 'started.json'), ('final', 'command.json'))}
        for path in paths.values():
            require(not self.api['safe'](path).exists(), f'preserve existing command evidence: {path}')
        entry = {'name': name, 'command': command, 'status': 'running',
                 'helper_sha256': self.helper_sha256, 'limits': dict(LIMITS),
                 'stdout': paths['stdout'].name, 'stderr': paths['stderr'].name,
                 'accounting': paths['accounting'].name, 'exit_status': None,
                 'input_bytes': len(input_data) if input_data is not None else 0,
                 'input_sha256': hashlib.sha256(input_data).hexdigest() if input_data is not None else None}
        self.receipt['commands'].append(entry)
        self._new_json(paths['started'], entry)
        start = time.monotonic_ns()
        process, out, err, readers, writer = None, None, None, [], None
        failure, result = None, None
        overflow, io_failed = threading.Event(), threading.Event()
        streams = {kind: {'observed_bytes': 0, 'retained_bytes': 0, 'overflow': False}
                   for kind in ('stdout', 'stderr')}

        def drain(kind, pipe, target, limit):
            try:
                with pipe:
                    for chunk in iter(lambda: pipe.read(64 << 10), b''):
                        state = streams[kind]
                        state['observed_bytes'] += len(chunk)
                        retained = chunk[:max(0, limit - state['retained_bytes'])]
                        if retained:
                            target.write(retained)
                            state['retained_bytes'] += len(retained)
                        if state['observed_bytes'] > limit:
                            state['overflow'] = True
                            overflow.set()
            except BaseException as error:
                streams[kind]['error'] = f'{type(error).__name__}: {error}'
                io_failed.set()

        def feed():
            try:
                with process.stdin:
                    view = memoryview(input_data)
                    while view:
                        written = process.stdin.write(view[:64 << 10])
                        if not written:
                            raise OSError('child input write made no progress')
                        view = view[written:]
            except BrokenPipeError:
                # Early rejection by a child is diagnosed from its exit/accounting.
                pass
            except BaseException as error:
                entry['stdin_error'] = f'{type(error).__name__}: {error}'
                io_failed.set()

        try:
            self.check_budget()
            require(self.api['sha'](self.helper) == self.helper_sha256, 'qualified helper changed before call')
            entry['admission'] = self.api['admission'](self.output)
            if not entry['admission']['ready']:
                raise self.api['ResourceHold']('Windows admission requires 2GiB free RAM and 4GiB commit/disk')
            out = paths['stdout'].open('xb')
            err = paths['stderr'].open('xb')
            argv = [str(self.helper), '--timeout-seconds', str(CHILD_SECONDS),
                    '--cpu-seconds', str(CHILD_SECONDS), '--accounting', str(paths['accounting']), '--', *command]
            entry['supervisor_command'] = argv
            process = subprocess.Popen(argv, cwd=ROOT, stdin=subprocess.PIPE if input_data is not None else subprocess.DEVNULL,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0, shell=False)
            entry['supervisor_pid'] = process.pid
            for kind, pipe, target, limit in (('stdout', process.stdout, out, STDOUT_LIMIT),
                                             ('stderr', process.stderr, err, STDERR_LIMIT)):
                thread = threading.Thread(target=drain, args=(kind, pipe, target, limit), daemon=True)
                thread.start()
                readers.append(thread)
            if input_data is not None:
                writer = threading.Thread(target=feed, daemon=True)
                writer.start()
            # Five seconds for helper cleanup, then at most five for forced owned-handle/pipe cleanup.
            deadline = time.monotonic() + CHILD_SECONDS + CLEANUP_SECONDS / 2
            while process.poll() is None:
                require(not overflow.is_set(), f'{name}: output budget exceeded')
                require(not io_failed.is_set(), f'{name}: subprocess stream failure')
                self.check_budget()
                require(time.monotonic() <= deadline, f'{name}: helper exceeded child timeout and cleanup allowance')
                time.sleep(.01)
        except BaseException as error:
            failure = error
        finally:
            if process is not None:
                cleanup_deadline = time.monotonic() + CLEANUP_SECONDS / 2
                try:
                    if process.poll() is None:
                        process.kill()  # Exact owned helper handle; its job closes and kills only owned descendants.
                    process.wait(timeout=max(.001, cleanup_deadline - time.monotonic()))
                    entry['exit_status'] = process.returncode
                except BaseException as error:
                    failure = failure or error
                    entry['helper_cleanup_error'] = f'{type(error).__name__}: {error}'
                for thread in [*readers, *([writer] if writer is not None else [])]:
                    thread.join(timeout=max(0, cleanup_deadline - time.monotonic()))
                if any(thread.is_alive() for thread in readers) or (writer is not None and writer.is_alive()):
                    failure = failure or RuntimeError(f'{name}: owned pipe cleanup exceeded allowance')
                    entry['pipe_cleanup_incomplete'] = True
                try:
                    require(entry['exit_status'] is not None, 'helper exit status unavailable')
                    value, accounting_sha = self._accounting(paths['accounting'], entry['exit_status'], process.pid)
                    self._cpu_ticks += value['user_cpu_ticks'] + value['kernel_cpu_ticks']
                    entry.update(job_accounting=value, accounting_sha256=accounting_sha)
                except BaseException as error:
                    self._accounting_complete = False
                    entry['accounting_error'] = f'{type(error).__name__}: {error}'
                    failure = failure or error
            for stream in (out, err):
                if stream is not None:
                    try:
                        stream.close()
                    except BaseException as error:
                        entry['stream_close_error'] = f'{type(error).__name__}: {error}'
                        failure = failure or error
            entry['streams'] = streams
            entry['wall_ns'] = time.monotonic_ns() - start
            entry['total_child_cpu_seconds'] = self.child_cpu() - self.cpu_started
            self.receipt['windows_runner']['aggregate_cpu_accounting_complete'] = self._accounting_complete
            try:
                require(self.api['sha'](self.helper) == self.helper_sha256, 'qualified helper changed during call')
                for kind in ('stdout', 'stderr'):
                    if paths[kind].exists():
                        entry[kind + '_sha256'] = self.api['sha'](paths[kind])
                require(not overflow.is_set(), f'{name}: output budget exceeded')
                require(not io_failed.is_set(), f'{name}: subprocess stream failure')
                if 'job_accounting' in entry:
                    measured = entry['job_accounting']
                    require(measured['user_cpu_ticks'] + measured['kernel_cpu_ticks'] <= CHILD_SECONDS * TICKS_PER_SECOND,
                            f'{name}: measured job CPU exceeds the 30-second command limit')
                if failure is None:
                    self.check_budget()
                    require(entry['exit_status'] == 0, f'{name}: status {entry["exit_status"]}; see {paths["stderr"]}')
                    # Decode only after bounded capture; preserve CR, LF and all other bytes exactly.
                    result = paths['stdout'].read_bytes().decode('utf-8', errors='strict')
                    paths['stderr'].read_bytes().decode('utf-8', errors='strict')
            except BaseException as error:
                failure = failure or error
            entry['status'] = ('resource_hold' if isinstance(failure, self.api['ResourceHold']) else 'failed') if failure else 'passed'
            if failure is not None:
                entry['error'] = f'{type(failure).__name__}: {failure}'
            self._new_json(paths['final'], entry)
        if failure is not None:
            raise failure
        return result

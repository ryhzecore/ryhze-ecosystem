"""Bounded native watched-authoring checks; every command, stage and failure stays local."""
from output_paths import evidence_path
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import queue
import statistics
import subprocess
import threading
import time
import uuid


def host_io_path(path):
    """Extend Windows fixture I/O paths only; keep ordinary paths in tool argv."""
    path = Path(path)
    if os.name != 'nt':
        return path
    if not path.is_absolute():
        raise ValueError('Windows fixture I/O requires an absolute path')
    value = str(path)
    if value.startswith('\\\\?\\'):
        return path
    if value.startswith('\\\\'):
        return Path('\\\\?\\UNC\\' + value[2:])
    return Path('\\\\?\\' + value)


p = argparse.ArgumentParser()
p.add_argument('--tool', required=True)
p.add_argument('--in-process', required=True)
p.add_argument('--root', required=True)
a = p.parse_args()
root = evidence_path(Path(a.root)) / str(uuid.uuid4())
host_io_path(root).mkdir(parents=True)
identity = '000000000000004d0000000000000001'
small = b'v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n'
changed = b'v 0 0 0\nv 2 0 0\nv 0 3 0\nf 1 2 3\n'
commands, checks, measurements = [], [], {}


def invoke(*args, success=True):
    run = subprocess.run(list(map(str, args)), capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=30)
    commands.append(dict(command=list(map(str, args)), exit_code=run.returncode, stdout=run.stdout, stderr=run.stderr))
    assert (run.returncode == 0) == success, commands[-1]
    return json.loads(run.stdout) if success else None


def replace(path, blob):
    temporary = path.with_suffix('.tmp')
    host_io_path(temporary).write_bytes(blob)
    host_io_path(temporary).replace(host_io_path(path))


class Session:
    def __init__(self, name, blob=small, duration=2500, imports=16, coalesce=False):
        self.path = root / name
        host_io_path(self.path).mkdir()
        self.source, self.output = self.path / 'source', self.path / 'output'
        host_io_path(self.source).mkdir()
        host_io_path(self.output).mkdir()
        host_io_path(self.source / 'source.obj').write_bytes(blob)
        self.command = list(map(str, [a.tool, '--watch', self.source, 'source.obj', self.output,
                                     self.path / 'stages', identity, duration, imports]))
        if coalesce:
            self.command.append('--coalesce')
        self.process = subprocess.Popen(self.command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.records, self.raw, self.q = [], [], queue.Queue(maxsize=129)
        self.reader_error = None

        def read():
            try:
                for _ in range(129):
                    line = self.process.stdout.readline(1025)
                    if not line:
                        return
                    self.raw.append(line.decode('utf-8', errors='replace'))
                    assert len(line) <= 1024 and line.endswith(b'\n'), 'receipt byte bound'
                    record = json.loads(line)
                    self.records.append(record)
                    self.q.put_nowait(record)
                raise AssertionError('receipt count bound')
            except BaseException as error:
                self.reader_error = repr(error)

        self.thread = threading.Thread(target=read)
        self.thread.start()

    def wait(self, predicate, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            assert self.reader_error is None, self.reader_error
            item = self.q.get(timeout=max(0.01, deadline-time.monotonic()))
            if predicate(item):
                return item
        raise AssertionError('receipt wait timeout')

    def close(self, success=True):
        # The tool's deadline stops admissions, then it joins owned child and I/O.
        # Do not kill it merely because the test's expected receipt was missing.
        code = self.process.wait(timeout=70)
        self.thread.join(timeout=5)
        assert not self.thread.is_alive(), 'reader not quiescent'
        stderr = self.process.stderr.read().decode('utf-8', errors='replace')
        commands.append(dict(command=self.command, exit_code=code, stdout=''.join(self.raw), stderr=stderr))
        assert not self.reader_error, self.reader_error
        assert len(self.records) <= 128 and self.records[-1]['phase'] == 2, commands[-1]
        assert not self.records[-1]['worker_retained'] and self.records[-1]['pending_source_bytes'] == 0
        assert (code == 0) == success, commands[-1]
        return self.records[-1]

    def settle(self):
        # An observed quiet interval, not a filesystem barrier. Windows can
        # coalesce/split one write into multiple batches, all retained below.
        deadline = time.monotonic()+2
        latest = self.records[-1]
        while time.monotonic() < deadline:
            try:
                latest = self.q.get(timeout=0.05)
            except queue.Empty:
                if not latest['worker_retained']:
                    return latest
        raise AssertionError('fixture did not reach an observed quiet interval')


def direct(blob, name):
    base = root / name
    host_io_path(base).mkdir()
    host_io_path(base / 'source.obj').write_bytes(blob)
    imported = invoke(a.in_process, 'import', base, 'source.obj', base, identity)
    cooked = invoke(a.in_process, 'cook', base, imported['leaf'], base)
    return host_io_path(base / imported['leaf']).read_bytes(), host_io_path(base / cooked['leaf']).read_bytes(), cooked['revision']


# Inspect only children of the task-owned tool. Holding SYNCHRONIZE on its worker
# proves whether it remained live across an edit; elapsed-time guesses do not.
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
class ProcessEntry(ctypes.Structure):
    _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('th32ProcessID', wintypes.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wintypes.DWORD), ('cntThreads', wintypes.DWORD),
                ('th32ParentProcessID', wintypes.DWORD), ('pcPriClassBase', wintypes.LONG), ('dwFlags', wintypes.DWORD),
                ('szExeFile', wintypes.WCHAR * 260)]
kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel.WaitForSingleObject.restype = wintypes.DWORD
kernel.CloseHandle.argtypes = [wintypes.HANDLE]


def edit_with_live_proof(session, blob):
    # Preparing a temp file inside source would itself notify the watcher and
    # could cancel the child before the intended source edit. Prepare outside
    # the watched directory; only the replacement belongs between observations.
    temporary = session.path / 'pending-source.obj'
    host_io_path(temporary).write_bytes(blob)
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    assert snapshot != ctypes.c_void_p(-1).value
    worker = None
    open_error = 0
    enumeration_error = 0
    try:
        item = ProcessEntry()
        item.dwSize = ctypes.sizeof(item)
        present = kernel.Process32FirstW(snapshot, ctypes.byref(item))
        while present:
            if item.th32ParentProcessID == session.process.pid and item.szExeFile.lower() == 'race_mesh_import_worker.exe':
                worker = kernel.OpenProcess(0x100000, False, item.th32ProcessID)
                if not worker:
                    open_error = ctypes.get_last_error()
                break
            present = kernel.Process32NextW(snapshot, ctypes.byref(item))
        if not present:
            enumeration_error = ctypes.get_last_error()
    finally:
        kernel.CloseHandle(snapshot)
    if not worker:
        proof = dict(before=False, after=False, open_error=open_error, enumeration_error=enumeration_error,
                     inconclusive='worker no longer observed')
        measurements.setdefault('edit_live_setups', []).append(proof)
        # ERROR_INVALID_PARAMETER means that the enumerated PID disappeared;
        # ERROR_NO_MORE_FILES is normal enumeration completion. Other API
        # failures are diagnostics, never timing retries.
        assert open_error in (0, 87) and enumeration_error in (0, 18), proof
        return False
    try:
        before_wait = kernel.WaitForSingleObject(worker, 0)
        before_error = ctypes.get_last_error() if before_wait == 0xffffffff else 0
        host_io_path(temporary).replace(host_io_path(session.source / 'source.obj'))
        after_wait = kernel.WaitForSingleObject(worker, 0)
        after_error = ctypes.get_last_error() if after_wait == 0xffffffff else 0
        before, after = before_wait == 258, after_wait == 258
        proof = dict(before=before, after=after, before_wait=before_wait, after_wait=after_wait,
                     before_error=before_error, after_error=after_error)
        measurements.setdefault('edit_live_setups', []).append(proof)
        assert before_wait in (0, 258) and after_wait in (0, 258), proof
        if before and after:
            measurements['edit_live_proof'] = dict(before=True, after=True)
            return True
        return False
    finally:
        kernel.CloseHandle(worker)


active = None
try:
    expected_imported, expected_cooked, revision = direct(changed, 'direct-changed')
    # A valid large source keeps a real child live long enough to obtain evidence.
    dense = bytearray()
    for n in range(18000):
        dense.extend(f'v {n} 0 0\nv {n+1} 0 0\nv {n} 1 0\n'.encode())
    for n in range(18000):
        dense.extend(f'f {n*3+1} {n*3+2} {n*3+3}\n'.encode())
    # Scheduling or prompt cancellation may prevent an overlap proof even for
    # correct execution. Preserve each inconclusive setup, join it, and require
    # an actual before+after proof within three fresh bounded attempts.
    for setup in range(3):
        active = Session(f'live-edit-{setup}', bytes(dense), duration=3000)
        active.wait(lambda r: r['outcome'] == 2)
        if edit_with_live_proof(active, changed):
            break
        active.close()
        active = None
    else:
        raise AssertionError('No proven edit during a live worker in three retained setups')
    published = active.wait(lambda r: r['outcome'] == 3 and r['source'] == hashlib.sha256(changed).hexdigest())
    assert published['superseded'] >= 1 and published['current_revision'] == revision
    for blob in (expected_imported, expected_cooked):
        assert host_io_path(active.output / (hashlib.sha256(blob).hexdigest()+'.rasset')).read_bytes() == blob
    host_io_path(active.source / 'source.obj').unlink()
    missing = active.wait(lambda r: r['outcome'] == 5 and r['read_failures'] == 3)
    assert missing['current_revision'] == revision
    replace(active.source / 'source.obj', b'mtllib forbidden.mtl\n')
    rejected = active.wait(lambda r: r['outcome'] == 4)
    assert rejected['current_revision'] == revision
    replace(active.source / 'source.obj', small)
    recovered = active.wait(lambda r: r['outcome'] == 3 and r['source'] == hashlib.sha256(small).hexdigest())
    assert recovered['current_revision'] != revision
    terminal = active.close()
    assert terminal['attempts'] <= 16 and max(r['read_failures'] for r in active.records) <= 3
    active = None
    checks.append('proven edit while real child live; supersession; missing/invalid source preservation and recovery; exact products')

    for coalesce in (False, True):
        active = Session('coalesce' if coalesce else 'baseline', duration=6000, imports=64, coalesce=coalesce)
        first = active.wait(lambda r: r['outcome'] == 3)
        costs = []
        previous = active.settle()['events']
        for trial in range(11):
            started = time.perf_counter_ns()
            # A separate leaf invalidates the directory but leaves the declared
            # source and its full importer/cooker recipe unchanged.
            replace(active.source / 'unrelated.obj', str(trial).encode())
            item = active.wait(lambda r: r['events'] > previous and r['outcome'] == (6 if coalesce else 3))
            assert item['current_revision'] == first['current_revision']
            if trial >= 2:
                costs.append((time.perf_counter_ns()-started)/1e6)
            previous = active.settle()['events']
        final = active.close()
        measurements['coalesce' if coalesce else 'baseline'] = dict(warmups=2, trials=9, observed_completion_ms=costs,
            median_ms=statistics.median(costs), attempts=final['attempts'], unchanged=final['unchanged'],
            retained_bytes=final['retained_bytes'], max_poll_ms=final['max_poll_ms'])
        assert (final['attempts'] == 1 if coalesce else final['attempts'] >= 12)
        active = None
    checks.append('paired identical-source work: full recipe identity, same revision, fixed two warmups and nine trials')

    active = Session('last-attempt', duration=1200, imports=1)
    first = active.wait(lambda r: r['outcome'] == 3)
    replace(active.source / 'source.obj', changed)
    final = active.close(success=False)
    assert final['terminal_error'] == 2 and final['attempts'] == 1 and final['current_revision'] == first['current_revision']
    active = None
    checks.append('last admitted attempt publishes; next distinct request terminates explicitly without discarding current')

    measurements['deadline_setups'] = []
    for setup in range(3):
        active = Session(f'deadline-during-admission-{setup}', bytes(dense), duration=1)
        final = active.close()
        admitted = any(r['outcome'] == 2 for r in active.records)
        measurements['deadline_setups'].append(dict(admitted=admitted, final=final))
        assert final['current_revision'] == '0'*64 and not list(host_io_path(active.output).glob('*.rasset'))
        active = None
        if admitted:
            assert final['attempts'] == 1
            break
        # Scheduling may consume 1 ms before the first poll. Preserve that clean
        # zero-attempt stop as an inconclusive setup, never as crossing evidence.
        assert final['attempts'] == 0
    assert any(s['admitted'] for s in measurements['deadline_setups']), 'no admitted deadline-crossing setup'
    checks.append('admission crosses run deadline; stop drains retained child and rejects its publication')

    # Quota remains observable with one terminal slot even when identical-source
    # coalescence allows many notifications without spending worker attempts.
    active = Session('receipt-bound', duration=8000, imports=1, coalesce=True)
    active.wait(lambda r: r['outcome'] == 3)
    for trial in range(125):
        replace(active.source / 'noise.obj', str(trial).encode())
        item = active.wait(lambda r: r['outcome'] == 6 or r['phase'] == 2)
        if item['phase'] == 2:
            break
    final = active.close(success=False)
    assert final['receipt_limit'] and len(active.records) == 128
    active = None
    checks.append('128 records of at most 1024 bytes, reserved physical-quiescence terminal record')

    rejected_root = root / 'containment'
    host_io_path(rejected_root).mkdir()
    host_io_path(rejected_root / 'source.obj').write_bytes(small)
    host_io_path(rejected_root / 'output').mkdir()
    for output in (rejected_root, rejected_root / 'output'):
        invoke(a.tool, '--watch', rejected_root, 'source.obj', output, root / 'never-created-stage', identity, 100, 1, success=False)
    invoke(a.tool, '--watch', rejected_root, 'source.obj', root, rejected_root / 'nested-stage', identity, 100, 1, success=False)
    assert not host_io_path(rejected_root / 'nested-stage').exists() and not host_io_path(root / 'never-created-stage').exists()
    checks.append('equal/descendant output and descendant staging rejected before creating stages')
    kernel.GetShortPathNameW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
    short = ctypes.create_unicode_buffer(32768)
    count = kernel.GetShortPathNameW(str(rejected_root), short, len(short))
    measurements['short_alias_available'] = bool(count and short.value.lower() != str(rejected_root).lower())
    if measurements['short_alias_available']:
        invoke(a.tool, '--watch', rejected_root, 'source.obj', short.value, root / 'alias-never-stage', identity, 100, 1, success=False)
        invoke(a.tool, '--watch', rejected_root, 'source.obj', root, str(Path(short.value)/'alias-stage'), identity, 100, 1, success=False)
        assert not host_io_path(rejected_root / 'alias-stage').exists() and not host_io_path(root / 'alias-never-stage').exists()
        checks.append('existing short-path alias cannot bypass actual-root containment')
finally:
    if active is not None:
        # Preserve owned tool cleanup even when a fixture assertion failed.
        if active.process.poll() is None:
            active.process.wait(timeout=70)
        active.thread.join(timeout=5)
        commands.append(dict(command=active.command, exit_code=active.process.returncode, stdout=''.join(active.raw),
                             stderr=active.process.stderr.read().decode('utf-8', errors='replace'), fixture_failed=True))
    receipt = dict(checks=checks, measurements=measurements, commands=commands,
                   binaries={name: hashlib.sha256(host_io_path(Path(os.path.abspath(name))).read_bytes()).hexdigest() for name in (a.tool, a.in_process)})
    host_io_path(root / 'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(json.dumps(dict(root=str(root), checks=len(checks), measurements=measurements)))

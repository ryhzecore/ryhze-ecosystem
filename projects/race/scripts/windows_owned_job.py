"""Windows tooling-only descendant ownership; no engine or qualification policy.

run_owned is a synchronous, main-thread API. Commands use an absolute .exe,
explicit byte bindings, a fresh Evidence directory and the existing admission
guard. The caller owns suite/configuration acceptance. Natural completion waits
for the private job to empty; root exit alone never releases descendants.

Pattern reviewed read-only in Axeon runtime/windows_process.c; independently
implemented with stdlib ctypes, without adopting Axeon's resource budgets.
API contract: Microsoft UpdateProcThreadAttribute (JOB_LIST/HANDLE_LIST),
QueryInformationJobObject (BasicAccountingInformation), and TerminateJobObject.
"""
from __future__ import annotations

from contextlib import ExitStack
import ctypes as c
from ctypes import wintypes as w
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import threading
import time

from output_paths import evidence_directory
from source_manifest import file_sha256
from verify_desktop_surfaces import native_guard

MAX_STAGE_SECONDS = 1200  # Existing ordinary-suite stage ceiling.
CLEANUP_SECONDS = 5       # Existing bounded cleanup convention; never a success assumption.
POLL_SECONDS = 0.05
SELF = Path(__file__).resolve()


class _BasicLimits(c.Structure):
    _fields_ = [("process_time", c.c_int64), ("job_time", c.c_int64),
                ("flags", w.DWORD), ("min_ws", c.c_size_t), ("max_ws", c.c_size_t),
                ("active_limit", w.DWORD), ("affinity", c.c_size_t),
                ("priority", w.DWORD), ("scheduling", w.DWORD)]


class _IO(c.Structure):
    _fields_ = [(name, c.c_uint64) for name in ("ro", "wo", "oo", "rb", "wb", "ob")]


class _Limits(c.Structure):
    _fields_ = [("basic", _BasicLimits), ("io", _IO),
                ("process_memory", c.c_size_t), ("job_memory", c.c_size_t),
                ("peak_process", c.c_size_t), ("peak_job", c.c_size_t)]


class _Accounting(c.Structure):
    _fields_ = [(name, c.c_int64) for name in ("user", "kernel", "period_user", "period_kernel")] + [
        (name, w.DWORD) for name in ("faults", "total", "active", "terminated")]


class _Startup(c.Structure):
    _fields_ = [("cb", w.DWORD), ("reserved", w.LPWSTR), ("desktop", w.LPWSTR),
                ("title", w.LPWSTR)] + [(name, w.DWORD) for name in
                ("x", "y", "xsize", "ysize", "xchars", "ychars", "fill", "flags")] + [
                ("show", w.WORD), ("reserved_bytes", w.WORD), ("reserved_ptr", c.c_void_p),
                ("stdin", w.HANDLE), ("stdout", w.HANDLE), ("stderr", w.HANDLE)]


class _StartupEx(c.Structure):
    _fields_ = [("startup", _Startup), ("attributes", c.c_void_p)]


class _Process(c.Structure):
    _fields_ = [("process", w.HANDLE), ("thread", w.HANDLE),
                ("pid", w.DWORD), ("tid", w.DWORD)]


def _require(value):
    if not value:
        raise c.WinError(c.get_last_error())
    return value


class _Job:
    def __init__(self):
        self.api = c.WinDLL("kernel32", use_last_error=True)
        self.handles = []
        self.job = None
        self.process = _Process()
        self.attributes = None
        self.attributes_ready = False
        self.member = False
        self.resumed = False
        signatures = {
            "CreateJobObjectW": (w.HANDLE, [c.c_void_p, w.LPCWSTR]),
            "SetInformationJobObject": (w.BOOL, [w.HANDLE, c.c_int, c.c_void_p, w.DWORD]),
            "QueryInformationJobObject": (w.BOOL, [w.HANDLE, c.c_int, c.c_void_p, w.DWORD, c.c_void_p]),
            "TerminateJobObject": (w.BOOL, [w.HANDLE, w.UINT]),
            "GetCurrentProcess": (w.HANDLE, []),
            "DuplicateHandle": (w.BOOL, [w.HANDLE, w.HANDLE, w.HANDLE, c.POINTER(w.HANDLE), w.DWORD, w.BOOL, w.DWORD]),
            "CloseHandle": (w.BOOL, [w.HANDLE]),
            "InitializeProcThreadAttributeList": (w.BOOL, [c.c_void_p, w.DWORD, w.DWORD, c.POINTER(c.c_size_t)]),
            "UpdateProcThreadAttribute": (w.BOOL, [c.c_void_p, w.DWORD, c.c_size_t, c.c_void_p, c.c_size_t, c.c_void_p, c.c_void_p]),
            "DeleteProcThreadAttributeList": (None, [c.c_void_p]),
            "CreateProcessW": (w.BOOL, [w.LPCWSTR, w.LPWSTR, c.c_void_p, c.c_void_p, w.BOOL,
                                        w.DWORD, c.c_void_p, w.LPCWSTR, c.POINTER(_StartupEx), c.POINTER(_Process)]),
            "IsProcessInJob": (w.BOOL, [w.HANDLE, w.HANDLE, c.POINTER(w.BOOL)]),
            "ResumeThread": (w.DWORD, [w.HANDLE]),
            "WaitForSingleObject": (w.DWORD, [w.HANDLE, w.DWORD]),
            "GetExitCodeProcess": (w.BOOL, [w.HANDLE, c.POINTER(w.DWORD)]),
            "TerminateProcess": (w.BOOL, [w.HANDLE, w.UINT]),
        }
        for name, (result, arguments) in signatures.items():
            function = getattr(self.api, name)
            function.restype, function.argtypes = result, arguments

    def prepare(self, streams):
        import msvcrt
        self.job = _require(self.api.CreateJobObjectW(None, None))
        self.handles.append(self.job)
        limits = _Limits()
        limits.basic.flags = 0x2000  # KILL_ON_JOB_CLOSE, no breakaway permission.
        _require(self.api.SetInformationJobObject(self.job, 9, c.byref(limits), c.sizeof(limits)))
        inherited = []
        current = self.api.GetCurrentProcess()
        for stream in streams:
            duplicate = w.HANDLE()
            _require(self.api.DuplicateHandle(current, msvcrt.get_osfhandle(stream.fileno()),
                                             current, c.byref(duplicate), 0, True, 2))
            self.handles.append(duplicate.value)
            inherited.append(duplicate.value)
        self.streams = (w.HANDLE * 3)(*inherited)
        self.jobs = (w.HANDLE * 1)(self.job)
        needed = c.c_size_t()
        self.api.InitializeProcThreadAttributeList(None, 2, 0, c.byref(needed))
        if not needed.value:
            raise c.WinError(c.get_last_error())
        self.attributes = c.create_string_buffer(needed.value)
        _require(self.api.InitializeProcThreadAttributeList(self.attributes, 2, 0, c.byref(needed)))
        self.attributes_ready = True
        for key, values in ((0x20002, self.streams), (0x2000D, self.jobs)):
            _require(self.api.UpdateProcThreadAttribute(self.attributes, 0, key, values,
                                                       c.sizeof(values), None, None))

    def create(self, command, cwd, environment):
        startup = _StartupEx()
        startup.startup.cb = c.sizeof(startup)
        startup.startup.flags = 0x100  # STARTF_USESTDHANDLES.
        startup.startup.stdin, startup.startup.stdout, startup.startup.stderr = self.streams
        startup.attributes = c.cast(self.attributes, c.c_void_p)
        text = subprocess.list2cmdline(command)
        if len(text.encode("utf-16-le")) // 2 + 1 > 32767:
            raise ValueError("Windows command line exceeds 32767 UTF-16 units")
        block = None
        if environment is not None:
            if any(not isinstance(k, str) or not k or "=" in k or "\0" in k
                   or not isinstance(v, str) or "\0" in v for k, v in environment.items()):
                raise ValueError("Invalid explicit environment")
            block = c.create_unicode_buffer("\0".join(f"{k}={v}" for k, v in
                                            sorted(environment.items(), key=lambda x: x[0].upper())) + "\0\0")
        # Atomic job assignment plus suspended creation: no child instruction
        # executes before membership is checked. Nested-job failure is terminal.
        flags = 0x4 | 0x80000 | 0x400 | 0x08000000
        _require(self.api.CreateProcessW(command[0], c.create_unicode_buffer(text), None, None,
                                        True, flags, block, str(cwd), c.byref(startup), c.byref(self.process)))
        self.handles.extend([self.process.process, self.process.thread])

    def verify_and_resume(self):
        member = w.BOOL()
        _require(self.api.IsProcessInJob(self.process.process, self.job, c.byref(member)))
        if not member.value:
            raise RuntimeError("Suspended child is not in its private job")
        self.member = True
        if self.api.ResumeThread(self.process.thread) == 0xFFFFFFFF:
            raise c.WinError(c.get_last_error())
        self.resumed = True

    def accounting(self):
        value = _Accounting()
        _require(self.api.QueryInformationJobObject(self.job, 1, c.byref(value), c.sizeof(value), None))
        return {"active_processes": value.active, "total_processes": value.total,
                "user_ticks": value.user, "kernel_ticks": value.kernel}

    def root_exit(self):
        waited = self.api.WaitForSingleObject(self.process.process, 0)
        if waited == 258:
            return None
        if waited != 0:
            raise c.WinError(c.get_last_error())
        code = w.DWORD()
        _require(self.api.GetExitCodeProcess(self.process.process, c.byref(code)))
        return code.value

    def terminate_and_join(self):
        # Even an unconfirmed suspended launch has an exact owned process handle.
        if not self.member and self.root_exit() is None:
            _require(self.api.TerminateProcess(self.process.process, 125))
        _require(self.api.TerminateJobObject(self.job, 125))
        deadline = time.monotonic() + CLEANUP_SECONDS
        while True:
            counters = self.accounting()
            if self.root_exit() is not None and counters["active_processes"] == 0:
                return counters
            if time.monotonic() >= deadline:
                raise RuntimeError("Private tree did not become empty within cleanup deadline")
            time.sleep(POLL_SECONDS)

    def close(self):
        errors = []
        if self.attributes_ready:
            self.api.DeleteProcThreadAttributeList(self.attributes)
            self.attributes_ready = False
        for handle in reversed(self.handles):
            if not self.api.CloseHandle(handle):
                errors.append(c.get_last_error())
        self.handles.clear()
        if errors:
            raise RuntimeError(f"Owned handle close failed: {errors}")


def _bindings(expected):
    actual = {str(Path(path).resolve(strict=True)): file_sha256(Path(path)) for path in expected}
    if any(actual[str(Path(path).resolve())] != digest for path, digest in expected.items()):
        raise ValueError("Bound tool or source bytes changed")
    return actual


def _cancelled(interrupted, cancel):
    return bool(interrupted) or (cancel is not None and cancel.is_set())


def _validate_request(command, cwd, timeout_seconds, bindings):
    if (not isinstance(command, list) or not command or
            any(not isinstance(x, str) or "\0" in x for x in command)):
        raise ValueError("Command must be a nonempty string argv without NUL")
    executable = Path(command[0])
    if not executable.is_absolute() or executable.suffix.lower() != ".exe" or not executable.is_file():
        raise ValueError("Absolute existing .exe required")
    if not math.isfinite(timeout_seconds) or not 0 < timeout_seconds <= MAX_STAGE_SECONDS:
        raise ValueError("Stage timeout must be positive and at most 1200 seconds")
    if not Path(cwd).is_absolute() or not Path(cwd).is_dir():
        raise ValueError("Absolute existing working directory required")
    required = {SELF, executable.resolve()}
    if not required.issubset({Path(p).resolve() for p in bindings}):
        raise ValueError("Bindings must include helper and executable")


def _install_signal_handlers(handlers, interrupted):
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGBREAK):
        handlers[sig] = signal.signal(sig, lambda signum, _frame: interrupted.append(signum))


def _wait_for_quiescence(job, record, launched, timeout_seconds, cancel, interrupted):
    while True:
        if _cancelled(interrupted, cancel):
            record.update(status="cancelled", exit_code=130)
            return
        if time.monotonic() - launched >= timeout_seconds:
            record.update(status="timeout", exit_code=124)
            return
        root_code, counters = job.root_exit(), job.accounting()
        if root_code is not None:
            record["root_exit_code"] = root_code
        if root_code is not None and counters["active_processes"] == 0:
            record.update(status="exited", exit_code=root_code, quiescent=True, accounting=counters)
            return
        time.sleep(min(POLL_SECONDS, max(0, timeout_seconds - (time.monotonic() - launched))))


def _launch_and_wait(job, stack, out, record, request):
    command, cwd, environment, timeout_seconds, cancel, interrupted, bindings = request
    stdin = stack.enter_context(open("NUL", "rb"))
    stdout = stack.enter_context((out / "stdout.log").open("xb"))
    stderr = stack.enter_context((out / "stderr.log").open("xb"))
    job.prepare((stdin, stdout, stderr))
    _bindings(bindings)  # Reject drift during admission/setup, before launch.
    if _cancelled(interrupted, cancel):
        record.update(status="cancelled", exit_code=130, quiescent=True)
        return
    launched = time.monotonic()
    job.create(command, cwd, environment)
    record.update(child_created=True, child_pid=job.process.pid)
    if not _cancelled(interrupted, cancel):
        job.verify_and_resume()
    record["resumed"] = job.resumed
    _wait_for_quiescence(job, record, launched, timeout_seconds, cancel, interrupted)


def _stop_child(record, job):
    try:
        if job is not None and job.process.process:
            record.update(child_created=True, child_pid=job.process.pid, resumed=job.resumed)
            if not record["quiescent"]:
                record["termination_requested"] = True
                record["accounting"] = job.terminate_and_join()
                record["quiescent"] = True
            if record["root_exit_code"] is None:
                record["root_exit_code"] = job.root_exit()
        elif not record["child_created"]:
            record["quiescent"] = True
    except Exception as error:
        record.update(status="cleanup_error", exit_code=125, quiescent=False,
                      cleanup_error=f"{type(error).__name__}: {error}")


def _release(record, job, stack, handlers):
    # Each release runs even if the previous one fails; every failure is recorded.
    # Kill-on-close remains a fallback, never proof of quiescence.
    closers = ([job.close] if job is not None else []) + [stack.close]
    for close in closers:
        try:
            close()
        except Exception as error:
            record.update(status="cleanup_error", exit_code=125,
                          close_error=f"{type(error).__name__}: {error}")
    for sig, handler in handlers.items():
        signal.signal(sig, handler)


def _seal(record, out, bindings, interrupted, started):
    try:
        if "bindings" in record:
            record["bindings_unchanged"] = _bindings(bindings) == record["bindings"]
    except Exception as error:
        record.update(status="binding_error", exit_code=125, bindings_unchanged=False,
                      binding_error=str(error))
    record["seconds_elapsed"] = time.monotonic() - started
    record["signals"] = interrupted
    record["logs"] = {p.name: {"bytes": p.stat().st_size, "sha256": file_sha256(p)}
                      for p in (out / "stdout.log", out / "stderr.log") if p.is_file()}
    with (out / "receipt.json").open("x", encoding="utf-8") as stream:
        json.dump(record, stream, indent=2)


def run_owned(command, *, cwd: Path, output: Path, timeout_seconds: float,
              bindings: dict, cancel: threading.Event | None = None, environment: dict | None = None) -> dict:
    """Return a final receipt; original root exit is separate from supervisor status.

    75 = admission hold; 124 = timeout; 130 = cancellation; 125 = helper failure.
    A child returning these numbers retains them with status 'exited'. Quiescent
    means observed zero private-job active processes AND signalled root handle,
    not successful work, a sandbox, leak freedom or a filesystem/log-size quota.
    Caller bindings must cover this helper and the exact executable; pass scripts
    and other authored inputs too. No lookup via PATH and no shell/batch fallback.
    """
    if os.name != "nt" or c.sizeof(c.c_void_p) != 8 or threading.current_thread() is not threading.main_thread():
        raise ValueError("Requires main-thread Windows x64 Python")
    out = evidence_directory("windows-owned-job", Path(output))
    record = {"schema": 1, "status": "error", "exit_code": 125, "root_exit_code": None,
              "quiescent": False, "child_created": False, "resumed": False,
              "termination_requested": False,
              "timeout_seconds": timeout_seconds, "cleanup_seconds": CLEANUP_SECONDS,
              "command": command, "cwd": str(cwd), "supervisor_pid": os.getpid()}
    job = None
    handlers = {}
    interrupted = []
    started = time.monotonic()
    stack = ExitStack()
    try:
        _validate_request(command, cwd, timeout_seconds, bindings)
        record["bindings"] = _bindings(bindings)
        record["guard"] = native_guard(out)
        if not record["guard"]["passed"]:
            record.update(status="held", exit_code=75, quiescent=True)
        else:
            _install_signal_handlers(handlers, interrupted)
            job = _Job()
            _launch_and_wait(job, stack, out, record,
                             (command, cwd, environment, timeout_seconds, cancel, interrupted, bindings))
    except (Exception, KeyboardInterrupt) as error:
        record.update(error=f"{type(error).__name__}: {error}", status="error", exit_code=125)
    finally:
        _stop_child(record, job)
        _release(record, job, stack, handlers)
        _seal(record, out, bindings, interrupted, started)
    return record

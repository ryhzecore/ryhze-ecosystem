#!/usr/bin/env python3
"""Native Windows process-supervisor regressions; never builds or installs.

Run only after host resource admission, with a compiled helper and a fresh dated
Evidence attempt. All generated fixtures and per-case receipts are preserved.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time


FIXTURE = r'''
import ctypes, json, os, subprocess, sys, time
from pathlib import Path
mode = sys.argv[1]
if mode == "arguments":
    print(json.dumps(sys.argv[2:], ensure_ascii=True))
elif mode == "streams":
    data = sys.stdin.buffer.read()
    sys.stdout.buffer.write(data)
    sys.stderr.buffer.write(b"child-stderr\n")
    sys.exit(70)
elif mode == "sleep":
    time.sleep(60)
elif mode == "exit":
    ctypes.WinDLL("kernel32", use_last_error=True).ExitProcess(int(sys.argv[2]))
elif mode == "marker":
    Path(sys.argv[2]).write_bytes(b"child executed\n")
elif mode == "identity":
    print(json.dumps({"pid": os.getpid(), "parent_pid": os.getppid()}))
elif mode == "cpu-worker":
    while not Path(sys.argv[2]).exists():
        time.sleep(.005)
    value = 1
    while True:
        value = (value * 1103515245 + 12345) & 0x7fffffff
elif mode == "cpu-descendants":
    children = [subprocess.Popen([sys.executable, __file__, "cpu-worker", sys.argv[3]],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(2)]
    ready = Path(sys.argv[2])
    temporary = ready.with_suffix(".tmp")
    temporary.write_text(json.dumps([child.pid for child in children]), encoding="ascii")
    temporary.replace(ready)
    while True:
        time.sleep(.01)
elif mode == "descendant":
    child = subprocess.Popen([sys.executable, __file__, "sleep"],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    ready = Path(sys.argv[2])
    temporary = ready.with_suffix(".tmp")
    temporary.write_text(str(child.pid), encoding="ascii")
    temporary.replace(ready)
    while not Path(sys.argv[3]).exists():
        time.sleep(.01)
    sys.exit(0)
elif mode == "limits":
    from ctypes import wintypes as w
    class Basic(ctypes.Structure):
        _fields_ = [("process_time", ctypes.c_longlong), ("job_time", ctypes.c_longlong),
            ("flags", w.DWORD), ("min_ws", ctypes.c_size_t), ("max_ws", ctypes.c_size_t),
            ("active", w.DWORD), ("affinity", ctypes.c_size_t),
            ("priority", w.DWORD), ("scheduling", w.DWORD)]
    class IO(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in ("ro", "wo", "oo", "rb", "wb", "ob")]
    class Extended(ctypes.Structure):
        _fields_ = [("basic", Basic), ("io", IO), ("process_memory", ctypes.c_size_t),
            ("job_memory", ctypes.c_size_t), ("peak_process", ctypes.c_size_t),
            ("peak_job", ctypes.c_size_t)]
    info = Extended()
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.QueryInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD, ctypes.c_void_p]
    api.QueryInformationJobObject.restype = w.BOOL
    if not api.QueryInformationJobObject(None, 9, ctypes.byref(info), ctypes.sizeof(info), None):
        raise ctypes.WinError(ctypes.get_last_error())
    print(json.dumps({"flags": info.basic.flags, "process_time": info.basic.process_time,
        "job_time": info.basic.job_time, "active": info.basic.active, "memory": info.job_memory}))
else:
    raise ValueError(mode)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    if (os.name != "nt" or ctypes.sizeof(ctypes.c_void_p) != 8
            or platform.machine().lower() not in ("amd64", "x86_64")):
        parser.error("native Windows x64 Python required; no WSL execution")
    helper = args.helper.resolve(strict=True)
    root = args.output_dir.absolute()
    root.mkdir(parents=False, exist_ok=False)
    fixture = root / "process fixture 雪.py"
    fixture.write_text(FIXTURE, encoding="utf-8", newline="\n")
    checks = []
    receipt = {"schema_version": 1, "status": "running", "native_windows": True,
               "helper": str(helper), "helper_sha256": hashlib.sha256(helper.read_bytes()).hexdigest(),
               "python": sys.version, "checks": checks}

    def save():
        (root / "process-tests.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")

    def command(*child, seconds=5, cpu=None, accounting=None):
        argv = [str(helper), "--timeout-seconds", str(seconds)]
        if cpu is not None:
            argv += ["--cpu-seconds", str(cpu)]
        if accounting is not None:
            argv += ["--accounting", str(accounting)]
        return [*argv, "--", *map(str, child)]

    def run(label, argv, expected_code=0, expected_stdout=None, expected_stderr=b"", stdin=None,
            validate=None):
        started = time.monotonic()
        entry = {"name": label, "command": list(map(str, argv)), "passed": False}
        checks.append(entry)
        save()
        try:
            result = subprocess.run(argv, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=15, cwd=root, check=False)
        except BaseException as error:
            entry.update(error=f"{type(error).__name__}: {error}", seconds=time.monotonic() - started)
            save()
            raise
        (root / (label + ".stdout")).write_bytes(result.stdout)
        (root / (label + ".stderr")).write_bytes(result.stderr)
        entry.update(exit_code=result.returncode, seconds=time.monotonic() - started)
        save()
        if expected_code is not None:
            assert result.returncode == expected_code, (label, result.returncode, result.stderr)
        if expected_stdout is not None:
            assert result.stdout == expected_stdout, (label, result.stdout)
        if expected_stderr is not None:
            assert result.stderr == expected_stderr, (label, result.stderr)
        if validate is not None:
            validate(result)
        entry["passed"] = True
        save()
        return result

    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    api.OpenProcess.restype = wintypes.HANDLE
    api.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    api.WaitForSingleObject.restype = wintypes.DWORD
    api.CloseHandle.argtypes = [wintypes.HANDLE]
    api.CloseHandle.restype = wintypes.BOOL
    api.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    api.TerminateProcess.restype = wintypes.BOOL

    def account(path, *, code, cpu=180, wall=5, created=True, job=True):
        raw = path.read_bytes()
        assert 0 < len(raw) < 1024 and raw.endswith(b"\n") and b"\r" not in raw, raw
        value = json.loads(raw)
        assert value["schema_version"] == 1
        for name in ("user_cpu_ticks", "kernel_cpu_ticks", "supervisor_pid", "child_pid",
                     "total_processes", "active_processes", "limit_terminated_processes"):
            assert type(value[name]) is int and value[name] >= 0, (name, value[name])
        assert value["cpu_seconds"] == cpu and value["wall_seconds"] == wall
        assert value["memory_bytes"] == 2 << 30 and value["active_process_limit"] == 256
        assert value["exit_code"] == code and 0 <= value["exit_code"] <= 0xffffffff
        assert value["cleanup_succeeded"] is True and value["active_processes"] == 0
        assert value["child_created"] is created and value["supervisor_pid"] > 0
        assert value["accounting_valid"] is job and value["limits_applied"] is job
        if created:
            assert value["child_pid"] > 0 and value["total_processes"] >= 1
        else:
            assert value["child_pid"] == value["total_processes"] == 0
        checks[-1].setdefault("accounting", []).append({"path": str(path),
            "sha256": hashlib.sha256(raw).hexdigest(), "value": value})
        return value

    def cpu_descendants():
        label = "tight-cpu-descendants"
        ready, go, accounting = (root / (label + suffix) for suffix in (".pids", ".go", ".json"))
        argv = command(sys.executable, fixture, "cpu-descendants", ready, go,
                       seconds=10, cpu=1, accounting=accounting)
        process = None
        handles = []
        started = time.monotonic()
        entry = {"name": label, "command": argv, "passed": False}
        checks.append(entry)
        save()
        try:
            with (root / (label + ".stdout")).open("xb") as out, (root / (label + ".stderr")).open("xb") as err:
                process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=out, stderr=err, cwd=root)
                until = time.monotonic() + 5
                while not ready.exists():
                    assert process.poll() is None, "CPU fixture exited before readiness"
                    assert time.monotonic() < until, "CPU fixture readiness deadline"
                    time.sleep(.01)
                pids = json.loads(ready.read_text("ascii"))
                assert len(pids) == 2
                for pid in pids:
                    child = api.OpenProcess(0x00100000 | 1, False, pid)
                    assert child, ctypes.WinError(ctypes.get_last_error())
                    handles.append(child)
                go.write_bytes(b"start\n")
                code = process.wait(timeout=12)
                entry.update(exit_code=code, seconds=time.monotonic() - started)
                assert code not in (0, 125), code
                data = account(accounting, code=code, cpu=1, wall=10)
                assert data["supervisor_pid"] == process.pid
                assert data["total_processes"] == 3, data
                assert data["user_cpu_ticks"] + data["kernel_cpu_ticks"] >= 10_000_000, data
                assert data["status"] in ("cpu_limit", "exited"), data
                for child in handles:
                    assert api.WaitForSingleObject(child, 3000) == 0, "CPU-bound descendant survived"
                assert (root / (label + ".stdout")).read_bytes() == b""
                diagnostic = (root / (label + ".stderr")).read_bytes()
                assert diagnostic in (b"", b"axeon-process: job CPU limit exceeded\n",
                                      b"axeon-process: job CPU limit exceeded\r\n"), diagnostic
                entry["passed"] = True
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            for child in handles:
                if api.WaitForSingleObject(child, 3000) == 258:
                    api.TerminateProcess(child, 125)
                    api.WaitForSingleObject(child, 5000)
                api.CloseHandle(child)
            save()

    def descendants(label, disposition):
        ready = root / (label + ".pid")
        finish = root / (label + ".finish")
        argv = command(sys.executable, fixture, "descendant", ready, finish,
                       seconds=2 if disposition == "timeout" else 10)
        process = None
        child_handle = None
        sibling = None
        started = time.monotonic()
        entry = {"name": label, "command": argv, "passed": False}
        checks.append(entry)
        save()
        try:
            sibling = subprocess.Popen([sys.executable, str(fixture), "sleep"],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            with (root / (label + ".stdout")).open("xb") as out, (root / (label + ".stderr")).open("xb") as err:
                process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=out, stderr=err, cwd=root)
                until = time.monotonic() + 5
                while not ready.exists():
                    assert process.poll() is None, "supervisor exited before descendant readiness"
                    assert time.monotonic() < until, "descendant readiness deadline"
                    time.sleep(.01)
                # Capture the exact child object before any cleanup, preventing PID reuse errors.
                child_handle = api.OpenProcess(0x00100000 | 1, False, int(ready.read_text("ascii")))
                assert child_handle, ctypes.WinError(ctypes.get_last_error())
                if disposition == "normal":
                    finish.write_bytes(b"finish\n")
                elif disposition == "kill-helper":
                    process.kill()  # This handle belongs to this fixture only.
                code = process.wait(timeout=8)
                entry.update(exit_code=code, seconds=time.monotonic() - started)
                if disposition != "kill-helper":
                    assert code == (124 if disposition == "timeout" else 0), code
                assert api.WaitForSingleObject(child_handle, 3000) == 0, "owned descendant survived"
                assert sibling.poll() is None, "unrelated fixture sibling was terminated"
                assert (root / (label + ".stdout")).read_bytes() == b""
                assert (root / (label + ".stderr")).read_bytes() == b""
                entry["passed"] = True
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            if sibling is not None and sibling.poll() is None:
                sibling.terminate()
                sibling.wait(timeout=5)
            if child_handle:
                if api.WaitForSingleObject(child_handle, 0) == 258:
                    # A failed adapter must not strand this test's exact child.
                    api.TerminateProcess(child_handle, 125)
                    api.WaitForSingleObject(child_handle, 5000)
                api.CloseHandle(child_handle)
            save()

    save()
    try:
        values = ["", "plain", "space here", "tab\there", "雪 café", "trailing\\", "two\\\\",
                  'embedded"quote', '\\"', '\\\\"', "space and slash \\", "&|<>^%PATH%", "a\nb"]
        def check_arguments(result):
            assert json.loads(result.stdout) == values

        run("arguments", command(sys.executable, fixture, "arguments", *values), validate=check_arguments)
        run("streams-and-trap-status", command(sys.executable, fixture, "streams"),
            expected_code=70, stdin=b"exact\x00bytes\n", expected_stdout=b"exact\x00bytes\n",
            expected_stderr=b"child-stderr\n")
        run("exception-status", command(sys.executable, fixture, "exit", "3221225477"),
            expected_code=3221225477, expected_stdout=b"")
        def check_limits(result, cpu=180):
            limits = json.loads(result.stdout)
            assert limits["flags"] & (0x2000 | 0x200 | 8 | 4 | 2) == (0x2000 | 0x200 | 8 | 4 | 2)
            assert not limits["flags"] & (0x800 | 0x1000), "job breakaway is forbidden"
            assert limits["memory"] == 2 << 30 and limits["active"] == 256
            assert limits["process_time"] == limits["job_time"] == cpu * 10_000_000

        run("job-limits", command(sys.executable, fixture, "limits"), validate=check_limits)
        run("missing-program", command(root / "absent.exe"), expected_code=125,
            expected_stdout=b"", expected_stderr=None)
        for index, invalid in enumerate(("0", "-1", "3601", "999999999999999999", "1.5", "")):
            run(f"invalid-timeout-{index}", command(sys.executable, fixture, "exit", "0", seconds=invalid),
                expected_code=125, expected_stdout=b"", expected_stderr=None)
        descendants("normal-exit-cleanup", "normal")
        descendants("timeout-cleanup", "timeout")
        descendants("helper-death-cleanup", "kill-helper")
        # The original 14 cases above remain unchanged in scope and default limits.
        run("tight-job-limits", command(sys.executable, fixture, "limits", cpu=30),
            validate=lambda result: check_limits(result, cpu=30))
        marker = root / "must-not-execute.marker"
        def child_not_started(result):
            assert not marker.exists(), "invalid option or accounting destination executed the child"
            assert not (root / "unused.json").exists() and not (root / "unused2.json").exists()

        for index, invalid in enumerate(("0", "-1", "181", "999999999999999999", "1.5", "", " 1", "+1")):
            run(f"invalid-cpu-{index}", command(sys.executable, fixture, "marker", marker, cpu=invalid),
                expected_code=125, expected_stdout=b"", expected_stderr=None, validate=child_not_started)
        for label, options in (("duplicate-cpu", ["--cpu-seconds", "30", "--cpu-seconds", "1"]),
                               ("missing-cpu", ["--cpu-seconds"]),
                               ("missing-accounting", ["--accounting"]),
                               ("empty-accounting", ["--accounting", ""]),
                               ("duplicate-accounting", ["--accounting", str(root / "unused.json"),
                                                          "--accounting", str(root / "unused2.json")])):
            argv = [str(helper), "--timeout-seconds", "5", *options, "--", sys.executable,
                    str(fixture), "marker", str(marker)]
            run(label, argv, expected_code=125, expected_stdout=b"", expected_stderr=None, validate=child_not_started)
        identity_record = root / "accounting identity 雪.json"
        def check_identity(result):
            identity = json.loads(result.stdout)
            data = account(identity_record, code=0)
            assert data["child_pid"] == identity["pid"]
            assert data["supervisor_pid"] == identity["parent_pid"]
            assert data["total_processes"] == 1 and data["status"] == "exited"

        run("accounting-identity", command(sys.executable, fixture, "identity", accounting=identity_record),
            validate=check_identity)
        stream_record = root / "accounting-streams.json"
        run("accounting-raw-streams", command(sys.executable, fixture, "streams", cpu=30, accounting=stream_record),
            expected_code=70, stdin=b"exact\x00bytes\r\n\n", expected_stdout=b"exact\x00bytes\r\n\n",
            expected_stderr=b"child-stderr\n", validate=lambda result: account(stream_record, code=70, cpu=30))
        exception_record = root / "accounting-exception.json"
        run("accounting-exception", command(sys.executable, fixture, "exit", "3221225477", accounting=exception_record),
            expected_code=3221225477, expected_stdout=b"",
            validate=lambda result: account(exception_record, code=3221225477))
        wall_record = root / "accounting-wall-limit.json"
        def check_wall_accounting(result):
            data = account(wall_record, code=124, wall=1)
            assert data["status"] == "wall_limit"

        run("accounting-wall-limit", command(sys.executable, fixture, "sleep", seconds=1, accounting=wall_record),
            expected_code=124, expected_stdout=b"", validate=check_wall_accounting)
        missing_record = root / "accounting-missing-program.json"
        run("accounting-missing-program", command(root / "absent.exe", accounting=missing_record),
            expected_code=125, expected_stdout=b"", expected_stderr=None,
            validate=lambda result: account(missing_record, code=125, created=False))
        preserved = root / "existing-accounting.json"
        preserved.write_bytes(b"preserve this existing receipt exactly\n")
        original = preserved.read_bytes()
        def check_preserved(result):
            assert preserved.read_bytes() == original
            child_not_started(result)

        run("accounting-preserves-existing", command(sys.executable, fixture, "marker", marker, accounting=preserved),
            expected_code=125, expected_stdout=b"", expected_stderr=None, validate=check_preserved)
        run("accounting-missing-parent", command(sys.executable, fixture, "marker", marker,
            accounting=root / "absent directory" / "report.json"),
            expected_code=125, expected_stdout=b"", expected_stderr=None, validate=child_not_started)
        run("accounting-rejects-stream", command(sys.executable, fixture, "marker", marker,
            accounting=str(preserved) + ":sidecar"),
            expected_code=125, expected_stdout=b"", expected_stderr=None, validate=check_preserved)
        cpu_descendants()
        receipt.update(status="passed", passed=len(checks), failures=0)
        save()
        print(json.dumps({"status": "passed", "checks": len(checks), "receipt": str(root / "process-tests.json")}))
        return 0
    except BaseException as error:
        receipt.update(status="failed", error=f"{type(error).__name__}: {error}",
                       passed=sum(item["passed"] for item in checks))
        save()
        raise


if __name__ == "__main__":
    raise SystemExit(main())

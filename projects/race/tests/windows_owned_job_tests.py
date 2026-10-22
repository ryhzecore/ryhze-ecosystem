"""Inert Windows owned-job fixtures only; never invokes an engine tool or build."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import subprocess
import sys
import threading
import time
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import windows_owned_job as owned
from output_paths import evidence_directory
from source_manifest import file_sha256
from verify_desktop_surfaces import native_guard

FIXTURE = r'''
import ctypes, json, os, subprocess, sys, time
from pathlib import Path
mode = sys.argv[1]
if mode == 'exit':
    ctypes.WinDLL('kernel32').ExitProcess(int(sys.argv[2]))
elif mode == 'identity':
    print(json.dumps({'pid': os.getpid(), 'parent_pid': os.getppid(), 'executable': sys.executable}))
elif mode == 'echo':
    print(json.dumps(sys.argv[2:]))
    sys.stderr.buffer.write(b'fixture-stderr\n')
elif mode == 'marker':
    Path(sys.argv[2]).write_bytes(b'ran')
elif mode == 'leaf':
    Path(sys.argv[2]).write_text(str(os.getpid()))
    time.sleep(float(sys.argv[4]))
    Path(sys.argv[3]).write_bytes(b'done')
elif mode == 'branch':
    subprocess.Popen([sys.executable, '-B', __file__, 'leaf', *sys.argv[2:]],
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
elif mode == 'tree':
    child = subprocess.Popen([sys.executable, '-B', __file__, 'branch', *sys.argv[2:]],
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    child.wait(timeout=5)
elif mode == 'breakaway':
    try:
        subprocess.Popen([sys.executable, '-B', __file__, 'marker', sys.argv[2]],
                         creationflags=0x01000000).wait(timeout=5)
    except OSError:
        sys.exit(0)
    sys.exit(91)
else:
    raise ValueError(mode)
'''


class Harness:
    """Shared fixture state and assertions; check names and order are unchanged."""

    def __init__(self, out, receipt):
        self.out, self.receipt = out, receipt
        self.fixture = out / "inert fixture 雪.py"
        self.fixture.write_text(FIXTURE, encoding="utf-8")
        self.bindings = {owned.SELF: receipt["helper_sha256"],
                         Path(sys.executable): file_sha256(Path(sys.executable)),
                         self.fixture: file_sha256(self.fixture), Path(__file__): receipt["tests_sha256"]}
        api = c.WinDLL("kernel32", use_last_error=True)
        api.OpenProcess.argtypes, api.OpenProcess.restype = [w.DWORD, w.BOOL, w.DWORD], w.HANDLE
        api.WaitForSingleObject.argtypes, api.WaitForSingleObject.restype = [w.HANDLE, w.DWORD], w.DWORD
        api.CloseHandle.argtypes, api.CloseHandle.restype = [w.HANDLE], w.BOOL
        api.GetCurrentProcess.restype = w.HANDLE
        api.GetProcessHandleCount.argtypes = [w.HANDLE, c.POINTER(w.DWORD)]
        api.GetProcessHandleCount.restype = w.BOOL
        self.api = api

    def check(self, name, value):
        self.receipt["checks"].append({"check": name, "passed": bool(value)})
        if not value:
            raise AssertionError(name)

    def handles(self):
        result = w.DWORD()
        self.check("query fixture handle count",
                   self.api.GetProcessHandleCount(self.api.GetCurrentProcess(), c.byref(result)))
        return result.value

    def run(self, label, *child, seconds=5, cancel=None, supplied=None):
        out = self.out
        value = owned.run_owned([sys.executable, "-B", str(self.fixture), *map(str, child)], cwd=out,
                                output=out / label, timeout_seconds=seconds,
                                bindings=self.bindings if supplied is None else supplied, cancel=cancel)
        self.receipt["cases"].append({"label": label, "receipt": str(out / label / "receipt.json"),
                                      "sha256": file_sha256(out / label / "receipt.json"), "value": value})
        if value["status"] == "held":
            raise RuntimeError("fixture admission held; no retry")
        return value

    def drained(self, label, value, code=0, status="exited", total=None):
        self.check(label + " status/exit", value["status"] == status and value["exit_code"] == code)
        self.check(label + " private tree empty",
                   value["quiescent"] and value["accounting"]["active_processes"] == 0)
        self.check(label + " resumed in owned job", value["child_created"] and value["resumed"])
        if total is not None:
            # OS/launcher activity can add private-job processes. Require every
            # known fixture process, but do not invent a universal exact total.
            self.check(label + " minimum descendant accounting", value["accounting"]["total_processes"] >= total)

    def tracked_tree(self, label, *, cancel=False):
        ready, done = self.out / (label + ".pid"), self.out / (label + ".done")
        event, acquired, errors = threading.Event(), [], []

        def observe():
            try:
                until = time.monotonic() + 4
                while not ready.exists() and time.monotonic() < until:
                    time.sleep(.01)
                pid = int(ready.read_text())
                handle = self.api.OpenProcess(0x00100000, False, pid)
                if not handle:
                    raise c.WinError(c.get_last_error())
                acquired.append(handle)
                if cancel:
                    event.set()
            except Exception as error:
                errors.append(str(error))

        observer = threading.Thread(target=observe)
        observer.start()
        try:
            result = self.run(label, "tree", ready, done, 15, seconds=2, cancel=event if cancel else None)
            observer.join(timeout=5)
            self.check(label + " observation joined", not observer.is_alive() and not errors and len(acquired) == 1)
            self.drained(label, result, code=130 if cancel else 124,
                         status="cancelled" if cancel else "timeout", total=3)
            self.check(label + " exact descendant handle signalled",
                       self.api.WaitForSingleObject(acquired[0], 0) == 0)
            self.check(label + " late descendant side effect prevented", not done.exists())
        finally:
            observer.join(timeout=5)
            for handle in acquired:
                self.api.CloseHandle(handle)


def identity_and_exit_cases(h):
    h.check("Windows x64 ABI structures", [c.sizeof(t) for t in
            (owned._Limits, owned._Accounting, owned._Startup, owned._StartupEx, owned._Process)]
            == [144, 48, 104, 112, 24])
    identity = h.run("identity", "identity")
    h.drained("identity", identity, total=1)
    h.receipt["fixture_identity"] = json.loads((h.out / "identity/stdout.log").read_bytes())
    h.check("exact child PID identity", h.receipt["fixture_identity"]["pid"] == identity["child_pid"])
    for code in (0, 7, 75, 124, 125, 130, 0xC0000005):
        result = h.run("exit-" + str(code), "exit", code)
        h.drained("exit-" + str(code), result, code=code, total=1)
        h.check("full root exit " + str(code), result["root_exit_code"] == code)


def stream_cases(h):
    args_echo = ["", "space arg", 'quote"inside', "trailing\\", "雪"]
    h.drained("argv", h.run("argv", "echo", *args_echo), total=1)
    h.check("CRT argv fidelity", json.loads((h.out / "argv/stdout.log").read_bytes()) == args_echo)
    stderr = (h.out / "argv/stderr.log").read_bytes()
    h.check("raw stderr", stderr == b"fixture-stderr\r\n" or stderr == b"fixture-stderr\n")


def tree_cases(h):
    ready, done = h.out / "natural.pid", h.out / "natural.done"
    result = h.run("natural-tree", "tree", ready, done, .35)
    h.drained("natural-tree", result, total=3)
    h.check("parent exit waits for grandchild", done.read_bytes() == b"done")
    h.tracked_tree("timeout-tree")
    sibling_guard = native_guard(h.out)
    h.receipt["sibling_guard"] = sibling_guard
    if not sibling_guard["passed"]:
        raise RuntimeError("fixture admission held before sibling")
    # This inert fixture has no descendants; only the test owns its Popen
    # handle. It must survive another private job's cancellation.
    sibling = subprocess.Popen([sys.executable, "-B", str(h.fixture), "leaf",
                                str(h.out / "sibling.pid"), str(h.out / "sibling.done"), "15"],
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        h.tracked_tree("cancel-tree", cancel=True)
        h.check("unrelated sibling survives private cancellation", sibling.poll() is None)
    finally:
        if sibling.poll() is None:
            sibling.terminate()
        sibling.wait(timeout=5)


def failure_injection_cases(h, marker):
    event = threading.Event()
    event.set()
    result = h.run("pre-cancel", "marker", marker, cancel=event)
    h.check("prelaunch cancellation", result["status"] == "cancelled" and not result["child_created"]
            and not marker.exists())
    with patch.object(owned._Job, "prepare", side_effect=OSError("injected job attribute rejection")):
        result = h.run("early-ownership-failure", "marker", marker)
    h.check("early ownership failure closed", result["status"] == "error" and not result["child_created"]
            and not marker.exists())
    with patch.object(owned._Job, "verify_and_resume", side_effect=OSError("injected suspended membership failure")):
        result = h.run("suspended-ownership-failure", "marker", marker)
    h.check("suspended ownership failure closed", result["status"] == "error" and result["child_created"]
            and not result["resumed"] and result["quiescent"] and not marker.exists())
    original_join = owned._Job.terminate_and_join

    def join_then_report_error(job):
        original_join(job)
        raise OSError("injected cleanup reporting error after real join")

    with patch.object(owned._Job, "verify_and_resume", side_effect=OSError("injected resume failure")), \
            patch.object(owned._Job, "terminate_and_join", join_then_report_error):
        result = h.run("cleanup-error", "marker", marker)
    h.check("cleanup error never claims quiescence", result["status"] == "cleanup_error"
            and result["exit_code"] == 125 and not result["quiescent"] and not marker.exists())


def admission_cases(h, marker):
    bad = dict(h.bindings)
    bad[Path(sys.executable)] = "0" * 64
    result = h.run("binding-rejection", "marker", marker, supplied=bad)
    h.check("tool mismatch cannot launch", result["status"] == "error" and not result["child_created"]
            and not marker.exists())
    with patch.object(owned, "native_guard", return_value={"passed": False, "fixture": True}):
        value = owned.run_owned([sys.executable, "-B", str(h.fixture), "marker", str(marker)], cwd=h.out,
                                output=h.out / "guard-hold", timeout_seconds=5, bindings=h.bindings)
    h.check("guard75 no launch", value["status"] == "held" and value["exit_code"] == 75
            and not value["child_created"] and not marker.exists())
    try:
        h.run("exit-0", "marker", marker)
    except FileExistsError:
        h.check("used output refused", not marker.exists())
    else:
        h.check("used output refused", False)


def containment_cases(h, marker):
    h.drained("breakaway", h.run("breakaway", "breakaway", marker), total=1)
    h.check("breakaway forbidden", not marker.exists())
    before = h.handles()
    for index in range(6):
        h.drained("repeat", h.run("repeat-" + str(index), "exit", 0), total=1)
    h.check("repeated owned handle cleanup", h.handles() == before)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    out = evidence_directory("p04-owned-job-fixtures", args.output)
    receipt = {"status": "running", "checks": [], "cases": [], "pipeline_run": False,
               "helper_sha256": file_sha256(owned.SELF), "tests_sha256": file_sha256(Path(__file__))}
    h = Harness(out, receipt)
    try:
        receipt["guard"] = native_guard(out)
        if not receipt["guard"]["passed"]:
            raise RuntimeError("fixture admission held; no child launched")
        marker = out / "should-not-run"
        identity_and_exit_cases(h)
        stream_cases(h)
        tree_cases(h)
        failure_injection_cases(h, marker)
        admission_cases(h, marker)
        containment_cases(h, marker)
        receipt["status"] = "passed"
    except BaseException as error:
        receipt.update(status="held" if "admission held" in str(error) else "failed",
                       error=f"{type(error).__name__}: {error}")
    finally:
        receipt["helper_unchanged"] = file_sha256(owned.SELF) == receipt["helper_sha256"]
        receipt["tests_unchanged"] = file_sha256(Path(__file__)) == receipt["tests_sha256"]
        with (out / "tests.json").open("x", encoding="utf-8") as stream:
            json.dump(receipt, stream, indent=2)
    print(json.dumps({"status": receipt["status"], "checks": len(receipt["checks"]),
                      "error": receipt.get("error"), "receipt": str(out / "tests.json")}))
    return 0 if receipt["status"] == "passed" else 75 if receipt["status"] == "held" else 1


if __name__ == "__main__":
    raise SystemExit(main())

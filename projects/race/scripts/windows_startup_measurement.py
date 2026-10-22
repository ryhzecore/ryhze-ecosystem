"""Opt-in X-08 batch; requires a Main-reviewed, hash-bound plan after checkpoint.

Plan schema 1: commit, authored_sha256, writer_thread, expires_utc, context,
bindings {absolute-path: sha256}, build_receipt {path, sha256}, and lanes
{editor/runtime: {executable, shaders, viewport: [width,height], adapter}}.
Bindings must cover the complete deployed executable/DLL/shader trees, pinned
Python/Git/powercfg/tasklist, source inputs and this tool's imported helpers.
Plan acceptance is a review decision, not inferred from caller-supplied JSON.
No build, checkpoint, pin-generation, retry, or budget override is provided.
"""
from __future__ import annotations

import argparse
import ctypes as c
from ctypes import wintypes as w
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import time

from output_paths import evidence_directory, evidence_path
from source_manifest import ROOT, authored_inputs, file_sha256
import windows_owned_job as owned
from windows_ordinary import checkpoint, regular
from verify_desktop_surfaces import native_guard

SELF = Path(__file__).resolve()
GIT = Path(r"C:\Program Files\Git\cmd\git.exe")
SYSTEM = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
POWERSHELL = Path(r"C:\Users\YOUR_USER\.cache\codex-runtimes\codex-primary-runtime\dependencies\native\powershell\pwsh.exe")
LIMITS = {"editor": (0.5, 150_000_000), "runtime": (0.7, 110_000_000)}
EMPTY = b'{"schema":1,"origin":[0,0,0],"sections":[],"objects":[]}\n'
WRITER = "public-snapshot-owner"


class AdmissionHold(RuntimeError):
    def __init__(self, message, evidence):
        super().__init__(message)
        self.evidence = evidence


class InputInterrupted(RuntimeError):
    """Real user input changed the system input tick during a trial."""

    def __init__(self, start_tick, observed_tick):
        super().__init__(f"Real user input interrupted measurement (tick {start_tick} -> {observed_tick})")
        self.evidence = {"input_tick_start": start_tick, "input_tick_observed": observed_tick}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def write_new(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")


def read_json(path):
    require(path.stat().st_size <= 8 * 1024 * 1024, "JSON input exceeds 8 MiB")
    return json.loads(path.read_text(encoding="utf-8"))


class Power(c.Structure):
    _fields_ = [(name, w.BYTE) for name in ("ac", "flags", "percent", "saver")] + [
        (name, w.DWORD) for name in ("life", "full_life")]


class Memory(c.Structure):
    _fields_ = [("cb", w.DWORD), ("faults", w.DWORD)] + [
        (name, c.c_size_t) for name in
        ("peak_ws", "ws", "peak_paged", "paged", "peak_nonpaged", "nonpaged",
         "commit", "peak_commit", "private_commit")]


class LastInput(c.Structure):
    _fields_ = [("cb", w.UINT), ("tick", w.DWORD)]


class JobProcesses(c.Structure):
    _fields_ = [("assigned", w.DWORD), ("listed", w.DWORD), ("pids", c.c_size_t * 16)]


def apis():
    kernel = c.WinDLL("kernel32", use_last_error=True)
    user = c.WinDLL("user32", use_last_error=True)
    psapi = c.WinDLL("psapi", use_last_error=True)
    signatures = {
        "GetSystemPowerStatus": (kernel, w.BOOL, [c.POINTER(Power)]),
        "GetCurrentProcess": (kernel, w.HANDLE, []),
        "IsProcessInJob": (kernel, w.BOOL, [w.HANDLE, w.HANDLE, c.POINTER(w.BOOL)]),
        "QueryInformationJobObject": (kernel, w.BOOL,
                                      [w.HANDLE, c.c_int, c.c_void_p, w.DWORD, c.POINTER(w.DWORD)]),
        "OpenProcess": (kernel, w.HANDLE, [w.DWORD, w.BOOL, w.DWORD]),
        "CloseHandle": (kernel, w.BOOL, [w.HANDLE]),
        "GetSystemMetrics": (user, c.c_int, [c.c_int]),
        "GetDpiForSystem": (user, w.UINT, []),
        "GetLastInputInfo": (user, w.BOOL, [c.POINTER(LastInput)]),
        "GetProcessMemoryInfo": (psapi, w.BOOL, [w.HANDLE, c.POINTER(Memory), w.DWORD]),
        "QueryPerformanceCounter": (kernel, w.BOOL, [c.POINTER(c.c_int64)]),
        "QueryPerformanceFrequency": (kernel, w.BOOL, [c.POINTER(c.c_int64)]),
    }
    result = {}
    for name, (library, restype, argtypes) in signatures.items():
        function = getattr(library, name)
        function.restype, function.argtypes = restype, argtypes
        result[name] = function
    return result


def checked(result):
    if not result:
        raise c.WinError(c.get_last_error())
    return result


def counter(api, name):
    value = c.c_int64()
    checked(api[name](c.byref(value)))
    return value.value


def last_input(api):
    value = LastInput(c.sizeof(LastInput), 0)
    checked(api["GetLastInputInfo"](c.byref(value)))
    return value.tick


def child_membership(api, handle, pid):
    member = w.BOOL()
    checked(api["IsProcessInJob"](handle, None, c.byref(member)))
    require(member.value, "Measured app child is not in a job")
    # NULL queries this host's immediate Job, not an arbitrary ancestor/global Job.
    # Fixed bound; a failed/truncated query is terminal, never retried.
    processes = JobProcesses()
    checked(api["QueryInformationJobObject"](None, 3, c.byref(processes), c.sizeof(processes), None))
    require(processes.assigned == processes.listed <= 16, "Incomplete immediate-job PID list")
    pids = list(processes.pids[:processes.listed])
    require(os.getpid() in pids and pid in pids, "Host and app are not in the same immediate job")
    return {"child_in_job": True, "host_immediate_job_pids": pids, "same_job_observed": True}


def context(api):
    power = Power()
    checked(api["GetSystemPowerStatus"](c.byref(power)))
    require(power.ac == 1 and power.saver == 0, "AC/battery-saver admission failed")
    scheme = subprocess.run([str(SYSTEM / "powercfg.exe"), "/getactivescheme"],
                            capture_output=True, text=True, timeout=15, check=True)
    require(scheme.stdout.strip() and not scheme.stderr.strip(), "Unknown power scheme")
    command = ("$ErrorActionPreference='Stop'; [ordered]@{"
               "cpu=@(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors);"
               "video=@(Get-CimInstance Win32_VideoController | Sort-Object PNPDeviceID | "
               "Select-Object Name,PNPDeviceID,DriverVersion,CurrentHorizontalResolution,"
               "CurrentVerticalResolution,CurrentRefreshRate)} | ConvertTo-Json -Depth 5 -Compress")
    hardware = subprocess.run([str(POWERSHELL), "-NoProfile", "-NonInteractive", "-Command", command],
                              capture_output=True, text=True, timeout=15, check=True)
    require(not hardware.stderr.strip(), "Hardware/driver context query reported errors")
    inventory = json.loads(hardware.stdout)
    require(inventory["cpu"] and inventory["video"], "Hardware/driver inventory unavailable")
    return {"os": platform.version(), "machine": platform.machine(),
            "cpu": platform.processor(), "ac": power.ac, "saver": power.saver,
            "power_scheme": scheme.stdout.strip(),
            "screen": [api["GetSystemMetrics"](0), api["GetSystemMetrics"](1)],
            "system_dpi": api["GetDpiForSystem"](), "hardware": inventory}


def admission(plan, output, api):
    require(datetime.now(timezone.utc) < datetime.fromisoformat(plan["expires_utc"]),
            "Writer/run reservation expired")
    require(plan["writer_thread"] == WRITER, "Wrong cooperative writer reservation")
    require(sys.dont_write_bytecode and Path(sys.executable).resolve() ==
            Path(r"C:\Users\YOUR_USER\AppData\Local\Programs\Python\Python313\python.exe").resolve(),
            "Use pinned Python with -B")
    require(not any(k.startswith(("QT_", "SDL_", "PYTHONPATH", "PYTHONHOME")) and v
                    for k, v in os.environ.items()), "Unreviewed application/interpreter override")
    require(Path(shutil.which("tasklist.exe") or "").resolve() == (SYSTEM / "tasklist.exe").resolve(),
            "Guard tasklist resolution differs from pinned tool")
    source = checkpoint(ROOT, plan["commit"], plan["authored_sha256"])
    require(source["git"]["branch"] == "codex-workspace/race-6d04676d", "Wrong branch")
    for name, expected in plan["bindings"].items():
        require(file_sha256(regular(Path(name))) == expected, "Bound input changed: " + name)
    snapshot = native_guard(output)
    if not snapshot["passed"]:
        raise AdmissionHold("Resource/process admission held", snapshot)
    current = context(api)
    require(current == plan["context"], "Host/power/display context changed")
    return {"guard": snapshot, "context": current, "commit": plan["commit"],
            "authored_sha256": plan["authored_sha256"]}


def validate_plan(plan):
    require(plan["schema"] == 1 and set(plan["lanes"]) == set(LIMITS), "Invalid plan lanes")
    required = {SELF, Path(sys.executable), GIT, SYSTEM / "powercfg.exe", SYSTEM / "tasklist.exe"}
    required.add(POWERSHELL)
    for name in ("windows_owned_job.py", "windows_ordinary.py", "output_paths.py",
                 "source_manifest.py", "verify_desktop_surfaces.py"):
        required.add(SELF.parent / name)
    required.update(ROOT / item["path"] for item in authored_inputs(ROOT)["files"])
    receipt = regular(Path(plan["build_receipt"]["path"]))
    require(file_sha256(receipt) == plan["build_receipt"]["sha256"], "Build receipt changed")
    required.add(receipt)
    for lane in plan["lanes"].values():
        executable, shaders = regular(Path(lane["executable"])), Path(lane["shaders"])
        require(executable.suffix.lower() == ".exe" and shaders.is_dir(), "Missing build inputs")
        required.add(executable)
        require(len(lane["viewport"]) == 2 and all(type(x) is int and x > 0 for x in lane["viewport"]),
                "Exact positive viewport dimensions required")
        require(bool(lane["adapter"]), "Exact adapter identity required")
        required.update(regular(path) for path in executable.parent.rglob("*.dll"))
        required.update(regular(path) for path in shaders.rglob("*") if path.is_file())
        if (executable.parent / "qt.conf").exists():
            required.add(regular(executable.parent / "qt.conf"))
    pinned = {regular(Path(name)) for name in plan["bindings"]}
    require(required <= pinned, "Incomplete source/deployment/tool/build-receipt bindings")


def events(path):
    if not path.exists():
        return []
    require(path.stat().st_size < 65536, "Witness event stream exceeded bound")
    raw = path.read_bytes()
    return [json.loads(line) for line in raw[:raw.rfind(b"\n") + 1].splitlines()]


def validate_events(rows, plan, lane, pid, start, frequency):
    names = [row["event"] for row in rows]
    required = {"application_entry", "scene_adopted", "window_visible",
                "first_present_api_return", "responsive_event_turn", "geometry_settled", "ready"}
    require(len(names) == len(set(names)) and set(names) <= required, "Unexpected/duplicate event")
    require(all(type(row["qpc"]) is int and row["qpc"] >= start for row in rows), "Invalid clock")
    require([row["qpc"] for row in rows] == sorted(row["qpc"] for row in rows), "Event clock reversed")
    if not required <= set(names):
        return None
    by_name = {row["event"]: row for row in rows}
    identity = by_name["application_entry"]
    require(identity["pid"] == pid and identity["frequency"] == frequency and identity["lane"] == lane
            and identity["source_commit"] == plan["commit"]
            and identity["authored_input_sha256"] == plan["authored_sha256"], "App identity differs")
    scene, frame = by_name["scene_adopted"], by_name["first_present_api_return"]
    stage_keys = ("qapplication_qpc", "style_qpc", "arguments_qpc", "witness_ready_qpc")
    if any(key in identity for key in stage_keys):
        require(all(key in identity for key in stage_keys), "Incomplete editor entry timing stages")
        stage_qpcs = [identity["qpc"], *(identity[key] for key in stage_keys)]
        require(all(type(value) is int for value in stage_qpcs) and
                stage_qpcs == sorted(stage_qpcs) and stage_qpcs[-1] <= scene["qpc"],
                "Editor entry timing stages were invalid or out of order")
    require(scene["objects"] == 0 and frame["objects"] == 0, "Scene not empty")
    expected = plan["lanes"][lane]
    require([frame["width"], frame["height"]] == expected["viewport"]
            and frame["adapter"] == expected["adapter"], "Viewport/adapter differs")
    require(names.index("scene_adopted") < names.index("first_present_api_return")
            and names.index("first_present_api_return") < names.index("responsive_event_turn")
            and names[-1] == "ready", "Ready witness ordering invalid")
    settled = by_name["geometry_settled"]
    require(type(settled["stable_frames"]) is int and settled["stable_frames"] >= 2
            and all(settled[key] == frame[key] for key in ("width", "height", "epoch"))
            and names.index("first_present_api_return") < names.index("geometry_settled")
            and names.index("geometry_settled") < names.index("ready")
            and names.index("responsive_event_turn") < names.index("ready"),
            "Ready did not follow witnessed stable frames and responsive turn")
    return by_name


def memory_sample(api, handle):
    value = Memory()
    value.cb = c.sizeof(value)
    checked(api["GetProcessMemoryInfo"](handle, c.byref(value), value.cb))
    return {"qpc": counter(api, "QueryPerformanceCounter"),
            "private_commit_bytes": value.private_commit, "peak_commit_bytes": value.peak_commit,
            "working_set_bytes": value.ws, "peak_working_set_bytes": value.peak_ws}


def close_window(pid):
    user = c.WinDLL("user32", use_last_error=True)
    callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
    user.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
    user.EnumWindows.argtypes = [callback_type, w.LPARAM]
    user.IsWindowVisible.argtypes = [w.HWND]
    found = []
    def visit(window, _):
        owner = w.DWORD()
        user.GetWindowThreadProcessId(window, c.byref(owner))
        if owner.value == pid and user.IsWindowVisible(window):
            found.append(bool(user.PostMessageW(window, 0x10, 0, 0)))
        return True
    checked(user.EnumWindows(callback_type(visit), 0))
    require(found and all(found), "Could not request owned window close")


def observe(plan, lane, phase, output, scene):
    api = apis()
    record = {"schema": 1, "lane": lane, "phase": phase, "passed": False, "samples": []}
    child, handle = None, None
    try:
        member = w.BOOL()
        checked(api["IsProcessInJob"](api["GetCurrentProcess"](), None, c.byref(member)))
        require(member.value, "Measurement host must run under owned-job supervision")
        record["admission"] = admission(plan, output, api)
        input_tick = last_input(api)
        setting = plan["lanes"][lane]
        command = [setting["executable"], "--scene", str(scene), "--shaders", setting["shaders"],
                   "--startup-measurement", str(output)]
        record["frequency"] = frequency = counter(api, "QueryPerformanceFrequency")
        with (output / "app-stdout.log").open("xb") as stdout, (output / "app-stderr.log").open("xb") as stderr:
            record["start_qpc"] = start = counter(api, "QueryPerformanceCounter")
            child = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.DEVNULL,
                                     stdout=stdout, stderr=stderr, close_fds=True)
            record["created_qpc"] = counter(api, "QueryPerformanceCounter")
            record["pid"] = child.pid
            handle = checked(api["OpenProcess"](0x410, False, child.pid))
            record["membership"] = child_membership(api, handle, child.pid)
            observed = poll_ready(api, child, handle, plan, lane, phase, output, start, frequency, input_tick, record)
            record.update(observed)
            close_window(child.pid)
            record["app_exit"] = child.wait(timeout=5)
            require(record["app_exit"] == 0, "Application exit failed")
        record["post_context"] = context(api)
        require(record["post_context"] == plan["context"], "Post-run host context changed")
        checkpoint(ROOT, plan["commit"], plan["authored_sha256"])
        record["passed"] = True
    except (Exception, KeyboardInterrupt) as error:
        record["error"] = f"{type(error).__name__}: {error}"
        if isinstance(error, AdmissionHold):
            record["hold"] = error.evidence
        if isinstance(error, InputInterrupted):
            record["input_interruption"] = error.evidence
        raise
    finally:
        record["app_termination_requested"] = False
        try:
            if child is not None:
                record["app_exit_before_cleanup"] = child.poll()
                if record["app_exit_before_cleanup"] is None:
                    record["app_termination_requested"] = True
                    child.terminate()  # Exact owned child only; outer Job owns all descendants.
                    record["app_exit"] = child.wait(timeout=5)
                else:
                    record["app_exit"] = record["app_exit_before_cleanup"]
                record["app_exit_hex"] = f"0x{record['app_exit'] & 0xffffffff:08X}"
        except Exception as error:
            record["child_cleanup_pending_outer_job"] = f"{type(error).__name__}: {error}"
        if handle is not None:
            record["process_handle_closed"] = bool(api["CloseHandle"](handle))
        write_new(output / "measurement.json", record)
    return record


def poll_ready(api, child, handle, plan, lane, phase, output, start, frequency, input_tick, record):
    ready, ready_memory = None, None
    while True:
        now = counter(api, "QueryPerformanceCounter")
        require(child.poll() is None, "Application exited before observation ended")
        observed_tick = last_input(api)
        if observed_tick != input_tick:
            raise InputInterrupted(input_tick, observed_tick)
        rows = events(output / "ready-events.jsonl")
        record["events"] = rows
        witnesses = validate_events(rows, plan, lane, child.pid, start, frequency)
        sample = memory_sample(api, handle)
        record["samples"].append(sample)
        require(len(record["samples"]) <= 5000, "Memory sample bound exceeded")
        cap_seconds, cap_memory = LIMITS[lane]
        if phase == "measured":
            require(sample["peak_commit_bytes"] <= cap_memory, "Private-commit budget exceeded")
        if witnesses is not None and ready is None:
            ready = witnesses["ready"]["qpc"]
            ready_memory = sample
        if ready is None:
            require((now - start) / frequency < 30, "Ready timeout")
        else:
            require((ready - start) / frequency < 30, "Ready event beyond timeout")
            if phase == "measured":
                require((ready - start) / frequency <= cap_seconds, "Startup ready-time budget exceeded")
            if (now - ready) / frequency >= 5:
                latency = (ready - start) / frequency
                require((sample["qpc"] - ready) / frequency <= 5.1, "Memory endpoint overshot 100ms bound")
                return {"events": rows, "ready_seconds": latency, "ready_observed_memory": ready_memory,
                        "endpoint_memory": sample, "endpoint_lag_seconds": (sample["qpc"] - ready) / frequency - 5,
                        "peak_commit_bytes": sample["peak_commit_bytes"],
                        "budget_enforced": phase == "measured",
                        "observed_caps_met": latency <= cap_seconds and sample["peak_commit_bytes"] <= cap_memory,
                        "timing_scope": "pre-CreateProcess through app-ready; Present return, not scanout"}
        time.sleep(0.01)


def distribution(values):
    require(len(values) == 10, "Expected exactly ten measured trials")
    ordered = sorted(values)
    return {"n": 10, "min": ordered[0], "median": statistics.median(ordered),
            "nearest_rank_p95": ordered[math.ceil(0.95 * len(ordered)) - 1], "max": ordered[-1],
            "limitation": "n=10 p95=max; no population-tail guarantee"}


def schedule(lane):
    return (["natural-cache"] if lane == "editor" else []) + ["warmup"] * 2 + ["measured"] * 10


def case_name(lane, index, phase):
    return f"{lane}-{index:02d}-{phase}"


def run_case(context, lane, index, phase):
    plan, plan_path, expected_hash, output, scene, bindings, api = context
    admission(plan, output, api)
    case = output / case_name(lane, index, phase)
    case.mkdir()
    command = [sys.executable, "-B", str(SELF), "--host", lane, "--plan", str(plan_path),
               "--plan-sha256", expected_hash, "--output", str(case), "--scene", str(scene),
               "--phase", phase]
    result = owned.run_owned(command, cwd=ROOT, output=case / "supervisor",
                             timeout_seconds=50, bindings=bindings)
    return case, result


def run_phase(context, record, lane, index, phase):
    """Run one scheduled trial; any interruption or failure is terminal."""
    case, result = run_case(context, lane, index, phase)
    entry = {"lane": lane, "phase": phase, "output": str(case), "supervisor": result}
    record["cases"].append(entry)
    require(result["status"] == "exited" and result["root_exit_code"] == 0
            and result["exit_code"] == 0 and result["quiescent"]
            and result.get("bindings_unchanged")
            and result["accounting"]["total_processes"] >= 2,
            "Owned stage held/failed or application was not launched")
    trial = read_json(case / "measurement.json")
    require(trial["passed"] and trial["process_handle_closed"] and trial["lane"] == lane
            and trial["phase"] == phase and trial["budget_enforced"] == (phase == "measured"),
            "Incomplete or mislabeled trial")
    entry["outcome"] = "passed"
    return trial


def batch(plan_path, expected_hash, requested):
    plan_path = regular(plan_path)
    require(file_sha256(plan_path) == expected_hash, "Reviewed plan hash differs")
    plan = read_json(plan_path)
    output = evidence_directory("desktop-startup-budget", requested)
    record = {"schema": 1, "passed": False, "plan_sha256": expected_hash, "cases": []}
    try:
        validate_plan(plan)
        plan["bindings"][str(plan_path)] = expected_hash
        api = apis()
        record["admission"] = admission(plan, output, api)
        scene = output / "empty.scene.json"
        with scene.open("xb") as stream:
            stream.write(EMPTY)
        bindings = dict(plan["bindings"])
        bindings[str(scene)] = file_sha256(scene)
        record["empty_scene_sha256"] = bindings[str(scene)]
        context = (plan, plan_path, expected_hash, output, scene, bindings, api)
        for lane in ("editor", "runtime"):
            measurements = []
            for index, phase in enumerate(schedule(lane)):
                trial = run_phase(context, record, lane, index, phase)
                if phase == "measured":
                    measurements.append(trial)
            record[lane] = {"seconds": distribution([v["ready_seconds"] for v in measurements]),
                            "peak_private_commit_bytes": distribution([v["peak_commit_bytes"] for v in measurements])}
        record["passed"] = True
    except (Exception, KeyboardInterrupt) as error:
        record["error"] = f"{type(error).__name__}: {error}"
        if isinstance(error, AdmissionHold):
            record["hold"] = error.evidence
    finally:
        write_new(output / "receipt.json", record)
    return 0 if record["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--plan-sha256", required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--host", choices=tuple(LIMITS))
    parser.add_argument("--phase", choices=("natural-cache", "warmup", "measured"))
    parser.add_argument("--scene", type=Path)
    args = parser.parse_args()
    require(os.name == "nt" and c.sizeof(c.c_void_p) == 8, "Requires Windows x64")
    if args.host:
        require(args.output is not None and args.scene is not None and args.phase is not None,
                "Missing host paths/phase")
        output = evidence_path(args.output)
        require(any(output.name == case_name(args.host, i, phase) and args.phase == phase
                    for i, phase in enumerate(schedule(args.host))), "Host phase disagrees with bound case")
        plan_path = regular(args.plan)
        require(file_sha256(plan_path) == args.plan_sha256, "Host plan hash differs")
        require(args.scene.read_bytes() == EMPTY, "Empty Scene bytes differ")
        observe(read_json(plan_path), args.host, args.phase, output, regular(args.scene))
        return 0
    require(args.scene is None and args.phase is None, "Scene/phase are fixed by batch")
    return batch(args.plan, args.plan_sha256, args.output)


if __name__ == "__main__":
    raise SystemExit(main())

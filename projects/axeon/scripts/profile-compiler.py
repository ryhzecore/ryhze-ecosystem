#!/usr/bin/env python3
"""Profile fixed Axeon compiler workloads on Linux, validating every invocation.

Usage: python3 scripts/profile-compiler.py --compiler PATH --output-dir NEW_DIR
       [--trials 7] [--label DESCRIPTION] [--timeout-seconds 120]

Every sample is a new process. Sources and compiler bytes are hashed before each
run, warming their filesystem cache; these are not cold-cache/startup or hot
in-process measurements. The formatter workload is deliberately rejected, with
the same complete valid input and required E_FORMAT_LIMIT outcome every time.
Only the Python standard library is needed. No native compiler is invoked.
"""
import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import resource
import select
import signal
import statistics
import subprocess
import sys
import time


WORKLOAD_VERSION = "axeon-compiler-profile-1"
FUNCTIONS = 128
STEPS_PER_FUNCTION = 32
COMMENT_LINES = 300_000
NESTED_BLOCKS = 50
MAX_CHILD_FILE_BYTES = 64 * 1024 * 1024
EMPTY_SHA256 = hashlib.sha256(b"").hexdigest()
WORKLOADS = ("startup", "check", "emit-ir", "format-budget-rejection")


class ProfileError(RuntimeError):
    """A run failed its required outcome; measurements are not accepted."""


def digest(path):
    hashed = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            hashed.update(chunk)
    return hashed.hexdigest()


def generated_sources():
    arithmetic = []
    for function in range(FUNCTIONS):
        arithmetic.append(f"fn stage_{function:03}(value: i64) -> i64 {{\n")
        arithmetic.append("    let mut x: i64 = value;\n")
        arithmetic.extend(["    x = (x * 3 + 7) % 100003;\n"] * STEPS_PER_FUNCTION)
        arithmetic.append("    return x;\n}\n")
    arithmetic.append("fn main() -> i64 {\n    let mut x: i64 = 1;\n")
    for function in range(FUNCTIONS):
        arithmetic.append(f"    x = stage_{function:03}(x);\n")
    arithmetic.append("    print(x);\n    return x;\n}\n")
    expected = 1
    for _ in range(FUNCTIONS * STEPS_PER_FUNCTION):
        expected = (expected * 3 + 7) % 100003
    # Keep this exact source shared across all compiler versions being compared.
    rejection = ("fn main()->i64{" + "{" * NESTED_BLOCKS
                 + "//\n" * COMMENT_LINES + "}" * NESTED_BLOCKS + "return 0;}")
    return {
        "arithmetic.ax": "".join(arithmetic).encode("ascii"),
        "format-budget.ax": rejection.encode("ascii"),
    }, f"{expected}\nresult: {expected}\n".encode("ascii")


def summarize(values):
    """Inclusive linear-interpolation quartiles (R7), population variance."""
    if not values or any(not math.isfinite(value) or value < 0 for value in values):
        raise ValueError("summary requires finite, nonnegative measurements")
    ordered = sorted(values)

    def quantile(fraction):
        index = (len(ordered) - 1) * fraction
        lower = math.floor(index)
        upper = math.ceil(index)
        return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)

    mean = statistics.fmean(values)
    deviation = statistics.pstdev(values)
    return {
        "n": len(values), "minimum": ordered[0], "maximum": ordered[-1],
        "median": statistics.median(values), "mean": mean,
        "q1": quantile(0.25), "q3": quantile(0.75),
        "iqr": quantile(0.75) - quantile(0.25),
        "population_variance": statistics.pvariance(values),
        "population_standard_deviation": deviation,
        "coefficient_of_variation": deviation / mean if mean else None,
    }


def child_limits():
    # This small pre-exec callback runs in this single-threaded harness's child.
    # Its inherited Python memory can impose a floor on wait4 maximum RSS.
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (MAX_CHILD_FILE_BYTES, MAX_CHILD_FILE_BYTES))


def kill_owned_group(pid):
    # The child started a new session and is not reaped until after this call;
    # its PID cannot be recycled into an unrelated process while it is our child.
    try:
        os.killpg(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def wait_child(pid, deadline):
    """Return this child's wait4 status/usage, timeout flag and wait mechanism."""
    descriptor = None
    if hasattr(os, "pidfd_open"):
        try:
            descriptor = os.pidfd_open(pid)
        except OSError:
            pass
    mechanism = "pidfd/select" if descriptor is not None else "wait4/WNOHANG; 1ms poll"
    try:
        while True:
            waited, status, usage = os.wait4(pid, os.WNOHANG)
            if waited == pid:
                return status, usage, False, mechanism
            remaining = deadline - time.perf_counter()
            if remaining <= 0:
                kill_owned_group(pid)
                _, status, usage = os.wait4(pid, 0)
                return status, usage, True, mechanism
            if descriptor is not None:
                select.select([descriptor], [], [], remaining)
            else:
                time.sleep(min(0.001, remaining))
    finally:
        if descriptor is not None:
            os.close(descriptor)


def run_child(argv, cwd, stdout_path, stderr_path, timeout_seconds, env=None):
    """Capture regular files and per-child wait4 accounting; never use pipes."""
    if not math.isfinite(timeout_seconds) or timeout_seconds <= 0:
        raise ValueError("timeout must be finite and positive")
    stdout_path, stderr_path = Path(stdout_path), Path(stderr_path)
    parent_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
        started = time.perf_counter()
        process = subprocess.Popen(
            [str(arg) for arg in argv], cwd=cwd, env=env, stdin=subprocess.DEVNULL,
            stdout=stdout, stderr=stderr, start_new_session=True, preexec_fn=child_limits,
        )
        try:
            status, usage, timed_out, mechanism = wait_child(
                process.pid, started + timeout_seconds)
            finished = time.perf_counter()
            process.returncode = os.waitstatus_to_exitcode(status)
        except BaseException:
            # An interrupted harness must not leave its own active child behind.
            try:
                waited, status, _ = os.wait4(process.pid, os.WNOHANG)
                if waited == 0:
                    kill_owned_group(process.pid)
                    _, status, _ = os.wait4(process.pid, 0)
                process.returncode = os.waitstatus_to_exitcode(status)
            except ChildProcessError:
                # Already reaped: never signal a PID that could have been reused.
                pass
            raise
    return {
        "argv": [str(arg) for arg in argv], "cwd": str(cwd),
        "returncode": process.returncode, "timed_out": timed_out,
        "wall_seconds": finished - started,
        "user_cpu_seconds": usage.ru_utime, "system_cpu_seconds": usage.ru_stime,
        "max_rss_kib": usage.ru_maxrss,
        "parent_max_rss_kib_before_spawn": parent_rss,
        "minor_page_faults": usage.ru_minflt, "major_page_faults": usage.ru_majflt,
        "voluntary_context_switches": usage.ru_nvcsw,
        "involuntary_context_switches": usage.ru_nivcsw,
        "wait_mechanism": mechanism,
        "stdout": {"path": str(stdout_path), "bytes": stdout_path.stat().st_size,
                   "sha256": digest(stdout_path)},
        "stderr": {"path": str(stderr_path), "bytes": stderr_path.stat().st_size,
                   "sha256": digest(stderr_path)},
    }


def require_result(sample, expected_code, stdout_sha256=None, stderr_empty=False,
                   stderr_contains=None):
    if sample["timed_out"]:
        raise ProfileError("child exceeded its deadline; timed-out samples are invalid")
    if sample["returncode"] != expected_code:
        raise ProfileError(f"expected exit {expected_code}, got {sample['returncode']}")
    if stdout_sha256 is not None and sample["stdout"]["sha256"] != stdout_sha256:
        raise ProfileError("stdout checksum differs from the required result")
    if stderr_empty and sample["stderr"]["bytes"] != 0:
        raise ProfileError("successful workload unexpectedly wrote stderr")
    if stderr_contains is not None:
        # The child file limit bounds this read, including failure cases.
        stderr = Path(sample["stderr"]["path"]).read_bytes()
        if stderr_contains not in stderr:
            raise ProfileError(f"stderr lacks required diagnostic {stderr_contains!r}")


def output_signature(sample):
    signature = {stream: sample[stream]["sha256"] for stream in ("stdout", "stderr")}
    if "artifact" in sample:
        signature["artifact"] = sample["artifact"]["sha256"]
    return signature


def read_optional(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def host_configuration():
    cpuinfo = read_optional("/proc/cpuinfo") or ""
    model = next((line.split(":", 1)[1].strip() for line in cpuinfo.splitlines()
                  if line.startswith("model name")), None)
    return {
        "platform": platform.platform(), "machine": platform.machine(),
        "kernel": platform.release(), "python": sys.version,
        "cpu_model": model, "logical_cpus": os.cpu_count(),
        "affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
        "load_average": list(os.getloadavg()), "meminfo": read_optional("/proc/meminfo"),
        "cpu_governor_cpu0": read_optional("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
        "cgroup_memory_max": read_optional("/sys/fs/cgroup/memory.max"),
        "cgroup_cpu_max": read_optional("/sys/fs/cgroup/cpu.max"),
        "clock": vars(time.get_clock_info("perf_counter")),
    }


def save_report(directory, report):
    # Only replace this run's own report; output directory was created exclusively.
    temporary = directory / "profile.json.tmp"
    with temporary.open("x", encoding="utf-8") as output:
        json.dump(report, output, indent=2, allow_nan=False)
        output.write("\n")
    temporary.replace(directory / "profile.json")


def profile(compiler, directory, trials, label, timeout_seconds):
    sources, expected_stdout = generated_sources()
    for name, source in sources.items():
        (directory / name).write_bytes(source)
    samples_dir = directory / "samples"
    samples_dir.mkdir()
    compiler_hash = digest(compiler)
    inputs = {name: {"bytes": len(source), "sha256": hashlib.sha256(source).hexdigest()}
              for name, source in sources.items()}
    environment = os.environ.copy()
    environment.update({"LC_ALL": "C", "LANG": "C", "TZ": "UTC", "SOURCE_DATE_EPOCH": "0"})
    report = {
        "schema_version": 1, "workload_version": WORKLOAD_VERSION, "label": label,
        "status": "running", "captured_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "compiler": {"path": str(compiler), "sha256": compiler_hash},
        "harness": {"path": str(Path(__file__).resolve()), "sha256": digest(__file__)},
        "host_before": host_configuration(), "inputs": inputs,
        "configuration": {
            "measured_trials_per_workload": trials, "warmup_trials_per_workload": 1,
            "timeout_seconds_per_process": timeout_seconds,
            "max_child_file_bytes": MAX_CHILD_FILE_BYTES, "core_dumps": "disabled",
            "arithmetic_functions": FUNCTIONS, "steps_per_function": STEPS_PER_FUNCTION,
            "format_nested_blocks": NESTED_BLOCKS, "format_comment_lines": COMMENT_LINES,
            "workload_order": list(WORKLOADS), "trial_order": "one warmup round, then fixed interleaved rounds",
            "controlled_environment": {key: environment[key] for key in ("LC_ALL", "LANG", "TZ", "SOURCE_DATE_EPOCH")},
            "native_tools_invoked": False,
        },
        "scope": {
            "processes": "New process each sample, including warmup. No hot in-process execution or cold-cache claim.",
            "wall": "Popen launch through own wait4 completion, including executable startup, output publication and exit; hashing is outside timing.",
            "cpu": "Per-child wait4 user/system CPU; child accounting may include descendants it waited for, not harness hashing CPU.",
            "rss": "Linux wait4 ru_maxrss KiB, not allocation count or simultaneous tree memory. Fork/pre-exec Python memory can set a floor; parent peak is recorded, never subtracted.",
            "cache": "Compiler and both input files are hashed before and after each run; pre-run hashing warms filesystem pages. No cache flushing.",
            "format": "Valid fixed ~900 KiB input intentionally exceeds the 1 MiB formatted-output budget. Success is rejection with E_FORMAT_LIMIT, exit 1, and empty stdout. Earlier bounded rejection preserves the same accepted language/result contract; this is not a valid-program formatting throughput claim.",
            "validation": "Arithmetic interpreter output checked against an independent Python integer result. Every measured status/output/IR checksum checked against the verified per-compiler warmup. This does not independently validate LLVM execution.",
            "limits": "Single recorded host; no thermal/frequency/background-load control, allocation instrumentation, kernel/device profiling or universal performance claim.",
            "statistics": "Inclusive linear-interpolation quartiles (R7), population variance and standard deviation; warmups/preparation excluded from summaries.",
        },
        "preparation": [], "samples": [], "summaries": {},
    }

    def unchanged():
        if digest(compiler) != compiler_hash:
            raise ProfileError("compiler bytes changed during this run")
        for name, metadata in inputs.items():
            if digest(directory / name) != metadata["sha256"]:
                raise ProfileError(f"input bytes changed during this run: {name}")

    def invoke(name, phase, trial, arguments, artifact=None):
        unchanged()
        prefix = samples_dir / f"{name}-{phase}-{trial:03}"
        sample = run_child([compiler, *arguments], directory,
                           prefix.with_suffix(".stdout"), prefix.with_suffix(".stderr"),
                           timeout_seconds, environment)
        sample.update({"workload": name, "phase": phase, "trial": trial, "valid": False})
        target = report["preparation"] if phase == "preparation" else report["samples"]
        target.append(sample)
        if artifact is not None and artifact.is_file():
            sample["artifact"] = {"path": str(artifact), "bytes": artifact.stat().st_size,
                                  "sha256": digest(artifact)}
        unchanged()
        return sample

    try:
        save_report(directory, report)
        identity = invoke("identity", "preparation", 0, ["--version"])
        require_result(identity, 0, stderr_empty=True)
        version = Path(identity["stdout"]["path"]).read_text(encoding="utf-8")
        if not version.startswith("axeon ") or len(version.splitlines()) != 1:
            raise ProfileError("--version did not report a single Axeon compiler identity line")
        report["compiler"]["version_output"] = version
        identity["valid"] = True
        correctness = invoke("arithmetic-correctness", "preparation", 0,
                             ["interpret", "arithmetic.ax"])
        require_result(correctness, 0, hashlib.sha256(expected_stdout).hexdigest(), stderr_empty=True)
        correctness["valid"] = True
        report["expected_arithmetic_stdout"] = expected_stdout.decode("ascii")
        valid_rejection_source = invoke("format-input-check", "preparation", 0,
                                        ["check", "format-budget.ax"])
        require_result(valid_rejection_source, 0, EMPTY_SHA256, stderr_empty=True)
        valid_rejection_source["valid"] = True
        references = {}
        for round_number in range(trials + 1):
            phase = "warmup" if round_number == 0 else "measured"
            for workload in WORKLOADS:
                artifact = None
                if workload == "startup":
                    arguments = ["--version"]
                elif workload == "check":
                    arguments = ["check", "arithmetic.ax"]
                elif workload == "emit-ir":
                    artifact = samples_dir / f"emit-ir-{phase}-{round_number:03}.ll"
                    arguments = ["emit-ir", "arithmetic.ax", "-o", str(artifact)]
                else:
                    arguments = ["format", "format-budget.ax"]
                sample = invoke(workload, phase, round_number, arguments, artifact)
                if workload == "startup":
                    require_result(sample, 0, identity["stdout"]["sha256"], stderr_empty=True)
                elif workload == "format-budget-rejection":
                    require_result(sample, 1, EMPTY_SHA256, stderr_contains=b"error[E_FORMAT_LIMIT]")
                else:
                    require_result(sample, 0, EMPTY_SHA256, stderr_empty=True)
                    if workload == "emit-ir":
                        if "artifact" not in sample or sample["artifact"]["bytes"] == 0:
                            raise ProfileError("emit-ir did not produce a nonempty artifact")
                        with artifact.open("rb") as emitted:
                            if b"target triple = " not in emitted.read(4096):
                                raise ProfileError("emit-ir artifact lacks a target declaration")
                signature = output_signature(sample)
                if phase == "warmup":
                    references[workload] = signature
                elif signature != references[workload]:
                    raise ProfileError(f"{workload} output checksum changed after warmup")
                sample["valid"] = True
                save_report(directory, report)
        for workload in WORKLOADS:
            samples = [sample for sample in report["samples"]
                       if sample["phase"] == "measured" and sample["workload"] == workload]
            report["summaries"][workload] = {
                metric: summarize([sample[metric] for sample in samples])
                for metric in ("wall_seconds", "user_cpu_seconds", "system_cpu_seconds", "max_rss_kib")
            }
        report["status"] = "passed"
        returncode = 0
    except (OSError, ValueError, ProfileError) as error:
        report.update({"status": "failed", "failure": str(error)})
        returncode = 1
    except KeyboardInterrupt:
        report.update({"status": "interrupted", "failure": "harness interrupted; own active child terminated"})
        returncode = 130
    report["host_after"] = host_configuration()
    save_report(directory, report)
    print(json.dumps({"status": report["status"], "report": str(directory / "profile.json"),
                      "failure": report.get("failure"), "summaries": report["summaries"]}, indent=2))
    return returncode


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--trials", type=int, default=7)
    parser.add_argument("--label", default="unspecified")
    parser.add_argument("--timeout-seconds", type=float, default=120)
    args = parser.parse_args(argv)
    if sys.platform != "linux" or not hasattr(os, "wait4"):
        parser.error("this harness requires Linux os.wait4 resource accounting")
    if not 1 <= args.trials <= 100:
        parser.error("--trials must be in 1..100")
    if not math.isfinite(args.timeout_seconds) or args.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be finite and positive")
    try:
        compiler = args.compiler.expanduser().resolve(strict=True)
        if not compiler.is_file() or not os.access(compiler, os.X_OK):
            parser.error("--compiler must be an executable regular file")
        directory = args.output_dir.expanduser().absolute()
        directory.mkdir()  # Exclusive; never reuse or clean an existing evidence directory.
    except OSError as error:
        parser.error(str(error))
    return profile(compiler, directory, args.trials, args.label, args.timeout_seconds)


if __name__ == "__main__":
    raise SystemExit(main())

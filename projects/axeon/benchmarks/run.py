#!/usr/bin/env python3
"""Bounded Linux benchmark runner. Requires --execute after correctness review."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import re
import resource
import runpy
import signal
import statistics
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
INPUTS = (250_000, 1_000_000)
LANGUAGES = ("Axeon", "C++", "Python")
TRIALS = 3
BOOTSTRAPS = 1000


def percentile(values, fraction):
    values = sorted(values)
    position = (len(values) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(values) - 1)
    return values[lower] + (values[upper] - values[lower]) * (position - lower)


def summary(values):
    return {"count": len(values), "median": statistics.median(values),
            "q1": percentile(values, .25), "q3": percentile(values, .75),
            "iqr": percentile(values, .75) - percentile(values, .25),
            "sample_variance": statistics.variance(values) if len(values) > 1 else 0,
            "sample_stddev": statistics.stdev(values) if len(values) > 1 else 0,
            "min": min(values), "max": max(values)}


def cluster_bootstrap(groups, seed):
    rng = random.Random(seed)
    medians = []
    for _ in range(BOOTSTRAPS):
        values = [v for group in rng.choices(groups, k=len(groups)) for v in group]
        medians.append(statistics.median(values))
    return [percentile(medians, .025), percentile(medians, .975)]


def speedup_bootstrap(numerator_groups, denominator_groups, seed):
    rng = random.Random(seed)
    ratios = []
    for _ in range(BOOTSTRAPS):
        numerator = [v for group in rng.choices(numerator_groups, k=len(numerator_groups)) for v in group]
        denominator = [v for group in rng.choices(denominator_groups, k=len(denominator_groups)) for v in group]
        ratios.append(statistics.median(numerator) / statistics.median(denominator))
    return [percentile(ratios, .025), percentile(ratios, .975)]


def adapt_llvm(ir):
    """Export only kernel 0 and remove the hosted C main, without changing its body."""
    signature = "define internal i64 @ax_fn_0(i64 %arg0) {"
    if ir.count(signature) != 1:
        raise ValueError("unexpected Axeon kernel signature; refusing an unsafe IR adaptation")
    hosted_main = re.compile(r"^define i32 @main\(\) \{\n.*?^\}\n", re.M | re.S)
    ir, count = hosted_main.subn("", ir)
    if count != 1:
        raise ValueError("expected exactly one hosted main")
    ir = ir.replace(signature, "define i64 @ax_fn_0(i64 %arg0) {")
    ir = re.sub(r"@ax_fn_0\b", "@axeon_bench_kernel", ir)
    return ir


def parse_process_output(stdout, n, expected_checksum):
    data = json.loads(stdout)
    if (data.get("n") != n or not isinstance(data.get("warm_ns"), list)
            or len(data["warm_ns"]) != 7 or data.get("checksums") != [expected_checksum] * 9):
        raise ValueError(f"timing protocol or checksum mismatch for N={n}")
    durations = [data.get("cold_ns"), data.get("warmup_ns"), *data["warm_ns"]]
    if any(type(duration) is not int or duration < 0 for duration in durations):
        raise ValueError("all kernel durations must be nonnegative integer nanoseconds")
    image_rss = data.get("proc_image_peak_rss_kib")
    if type(image_rss) is not int or image_rss <= 0:
        raise ValueError("positive post-exec image VmHWM is required; no zero or missing fallback")
    return data


class Runner:
    def __init__(self, output, cpu):
        self.output = output
        self.cpu = cpu
        self.deadline = time.monotonic() + 180
        self.cpu_seconds = 0.0
        self.records = []

    def run(self, command, label, timeout=30):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0 or self.cpu_seconds >= 180:
            raise RuntimeError("overall 180-second wall/child-CPU budget exhausted")
        limit = min(timeout, remaining)

        def limits():
            os.setsid()
            os.sched_setaffinity(0, {self.cpu})
            resource.setrlimit(resource.RLIMIT_AS, (2 * 1024**3, 2 * 1024**3))
            resource.setrlimit(resource.RLIMIT_CPU, (30, 30))

        env = dict(os.environ, LC_ALL="C", LANG="C", PYTHONHASHSEED="0",
                   OPENBLAS_NUM_THREADS="1", OMP_NUM_THREADS="1", MPLBACKEND="Agg",
                   MPLCONFIGDIR=str(self.output / "matplotlib-cache"))
        command = [str(item) for item in command]
        with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
            begin = time.perf_counter_ns()
            proc = subprocess.Popen(command, cwd=ROOT, stdout=stdout, stderr=stderr,
                                    env=env, preexec_fn=limits)
            timed_out = False
            while True:
                pid, status, usage = os.wait4(proc.pid, os.WNOHANG)
                if pid:
                    break
                if (time.perf_counter_ns() - begin) / 1e9 > limit:
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    _, status, usage = os.wait4(proc.pid, 0)
                    timed_out = True
                    break
                time.sleep(.001)
            elapsed = time.perf_counter_ns() - begin
            proc.returncode = os.waitstatus_to_exitcode(status)
            stdout.seek(0)
            stderr.seek(0)
            out = stdout.read().decode("utf-8", "replace")
            err = stderr.read().decode("utf-8", "replace")
        self.cpu_seconds += usage.ru_utime + usage.ru_stime
        record = {"label": label, "command": command, "wall_ns": elapsed,
                  "peak_rss_kib": usage.ru_maxrss, "user_cpu_seconds": usage.ru_utime,
                  "system_cpu_seconds": usage.ru_stime, "exit_code": proc.returncode,
                  "stdout": out, "stderr": err}
        self.records.append(record)
        if timed_out or proc.returncode:
            raise RuntimeError(f"{label} failed (timeout={timed_out}, exit={proc.returncode}): {err[-2000:]}")
        return record


def file_hash(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def assembly_evidence(path, symbol):
    text = Path(path).read_text()
    match = re.search(rf"^{re.escape(symbol)}:.*?^\s*\.size\s+{re.escape(symbol)}\b", text, re.M | re.S)
    if not match:
        raise RuntimeError(f"assembly does not contain {symbol}")
    body = match.group()
    labels = {m.group(1): m.start() for m in re.finditer(r"^(\.L[^ :]+):", body, re.M)}
    backward = []
    for branch in re.finditer(r"^\s*(j[a-z]+)\s+(\.L[^\s,#]+)", body, re.M):
        if branch.group(2) in labels and labels[branch.group(2)] < branch.start():
            backward.append(branch.group().strip())
    if not backward:
        raise RuntimeError(f"no retained backward loop branch found in {symbol}")
    return {"file": str(path), "sha256": file_hash(path), "backward_branches": backward,
            "kernel_assembly_lines": len(body.splitlines())}


def write_reports(result, output):
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    with (output / "samples.csv").open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["language", "n", "trial", "phase", "sample", "nanoseconds", "checksum",
                         "proc_image_peak_rss_kib", "wait4_lifetime_peak_rss_kib", "process_wall_ns", "outside_kernel_ns"])
        for process in result["processes"]:
            phases = [("cold", 0, process["cold_ns"]), ("warmup", 0, process["warmup_ns"])]
            phases += [("warm", i, value) for i, value in enumerate(process["warm_ns"])]
            for call, (phase, sample, duration) in enumerate(phases):
                writer.writerow([process["language"], process["n"], process["trial"], phase, sample,
                                 duration, process["checksums"][call], process["proc_image_peak_rss_kib"], process["peak_rss_kib"],
                                 process["process_wall_ns"], process["outside_kernel_ns"]])
    with (output / "summary.csv").open("w", newline="") as f:
        columns = ["language", "n", "warm_median_ns", "warm_q1_ns", "warm_q3_ns", "warm_min_ns",
                   "warm_max_ns", "warm_ci95_low_ns", "warm_ci95_high_ns", "cold_median_ns",
                   "time_divided_by_axeon", "speedup_ci95_low", "speedup_ci95_high",
                   "proc_image_rss_median_kib", "wait4_lifetime_rss_median_kib", "outside_kernel_median_ns", "expected_checksum", "correct"]
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        for row in result["summaries"]:
            writer.writerow({"language": row["language"], "n": row["n"],
                "warm_median_ns": row["warm"]["median"], "warm_q1_ns": row["warm"]["q1"],
                "warm_q3_ns": row["warm"]["q3"], "warm_min_ns": row["warm"]["min"],
                "warm_max_ns": row["warm"]["max"], "warm_ci95_low_ns": row["warm_ci95_ns"][0],
                "warm_ci95_high_ns": row["warm_ci95_ns"][1], "cold_median_ns": row["cold"]["median"],
                "time_divided_by_axeon": row["time_divided_by_axeon"],
                "speedup_ci95_low": row["speedup_ci95"][0], "speedup_ci95_high": row["speedup_ci95"][1],
                "proc_image_rss_median_kib": row["proc_image_peak_rss_kib"]["median"],
                "wait4_lifetime_rss_median_kib": row["process_peak_rss_kib"]["median"],
                "outside_kernel_median_ns": row["outside_kernel_ns"]["median"],
                "expected_checksum": row["expected_checksum"], "correct": row["correct"]})


def execute(args):
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        raise RuntimeError(f"output directory must be absent or empty: {output}")
    output.mkdir(parents=True, exist_ok=True)
    affinity = sorted(os.sched_getaffinity(0))
    cpu = affinity[0] if args.cpu is None else args.cpu
    if cpu not in affinity:
        raise RuntimeError(f"CPU {cpu} is outside allowed affinity {affinity}")
    runner = Runner(output, cpu)
    compiler, clang = args.compiler.resolve(), args.clang.resolve()
    result = {"schema_version": 1, "status": "running", "inputs": list(INPUTS),
              "trials": TRIALS, "warmups_per_process": 1, "warm_samples_per_process": 7,
              "bootstrap_resamples": BOOTSTRAPS, "processes": [], "summaries": [],
              "plot_requested": not args.no_plot,
              "compiler_build_wall_seconds_supplied_by_caller": args.compiler_build_seconds,
              "host": {"platform": platform.platform(), "python": sys.version,
                       "python_executable": sys.executable, "allowed_cpus": affinity, "pinned_cpu": cpu,
                       "python_kernel_clock": vars(time.get_clock_info("perf_counter")),
                       "cpuinfo": Path("/proc/cpuinfo").read_text(),
                       "limits": {"subprocess_wall_seconds": 30, "subprocess_virtual_bytes": 2*1024**3,
                                  "overall_wall_seconds": 180, "overall_child_cpu_seconds": 180}},
              "notes": ["First call means a fresh-process kernel call, not flushed CPU or filesystem caches.",
                        "Bootstrap resamples whole processes; only three process clusters per series.",
                        "Primary memory is post-exec /proc/self/status VmHWM read after all kernels, before output formatting; it includes hosted process/runtime/driver, not incremental kernel allocation.",
                        "wait4 peak RSS is retained separately as a process-lifetime metric, including inherited pre-exec Python runner state.",
                        "Outside-kernel wall includes launch, imports, reporting, scheduling, and timer overhead.",
                        "Integer state recurrence; optimized tensor/numerical-library baseline and accelerator transfer are not applicable.",
                        "Axeon benchmark-only LLVM symbol export is not a supported language FFI feature."]}
    try:
        input_paths = [HERE / "workload.ax", HERE / "kernel.cpp", HERE / "driver.c",
                       HERE / "kernel_python.py", HERE / "run.py", HERE / "plot_results.py",
                       ROOT / "runtime/runtime.c", ROOT / "runtime/runtime.h"]
        input_hashes = {str(path.relative_to(ROOT)): file_hash(path) for path in input_paths}
        compiler_hash, clang_hash = file_hash(compiler), file_hash(clang)
        result["input_hashes_before_execution"] = input_hashes
        result["axeon_compiler_sha256"] = compiler_hash
        result["clang_sha256"] = clang_hash
        result["versions"] = {"axeon": runner.run([compiler, "--version"], "axeon_version")["stdout"],
                              "clang": runner.run([clang, "--version"], "clang_version")["stdout"]}
        if not re.search(r"\bclang version 19\.1\.7(?:\s|$)", result["versions"]["clang"].splitlines()[0]):
            raise RuntimeError("this protocol pins Clang 19.1.7")
        source = HERE / "workload.ax"
        original_ir, adapted_ir = output / "original.ll", output / "kernel.ll"
        phases = []

        def phase(command, label):
            record = runner.run(command, label)
            phases.append(record)
            return record

        phase([compiler, "emit-ir", source, "-o", original_ir], "axeon_frontend_emit_ir")
        adapted_ir.write_text(adapt_llvm(original_ir.read_text()))
        flags = ["-target", "x86_64-pc-linux-gnu", "-O3", "-Werror", "-Wno-override-module"]
        phase([clang, *flags, "-std=c11", "-c", ROOT / "runtime/runtime.c", "-o", output / "runtime.o"], "shared_runtime_compile")
        phase([clang, *flags, "-c", adapted_ir, "-o", output / "axeon.o"], "axeon_llvm_optimization_codegen")
        phase([clang, *flags, "-std=c++17", "-c", HERE / "kernel.cpp", "-o", output / "cpp.o"], "cpp_frontend_optimization_codegen")
        binaries = {}
        for language, symbol, object_name in [("Axeon", "axeon_bench_kernel", "axeon.o"),
                                               ("C++", "cpp_bench_kernel", "cpp.o")]:
            slug = "axeon" if language == "Axeon" else "cpp"
            driver = output / f"driver_{slug}.o"
            phase([clang, *flags, "-std=c11", f"-DBENCH_KERNEL={symbol}", "-c", HERE / "driver.c", "-o", driver], f"{slug}_driver_compile")
            binaries[language] = output / slug
            phase([clang, *flags, output / object_name, driver, output / "runtime.o", "-o", binaries[language]], f"{slug}_link")
        phase([sys.executable, "-c", "import py_compile,sys; py_compile.compile(sys.argv[1], cfile=sys.argv[2], doraise=True)",
               HERE / "kernel_python.py", output / "kernel_python.pyc"], "python_bytecode_prepare")
        result["python_preparation_note"] = "Bytecode preparation is measured separately; timed fresh processes launch source scripts and do not reuse this prepared pyc."
        result["compilation_phases"] = phases
        binaries["Python"] = HERE / "kernel_python.py"
        runner.run([clang, *flags, "-S", adapted_ir, "-o", output / "axeon.s"], "axeon_assembly_audit")
        runner.run([clang, *flags, "-std=c++17", "-S", HERE / "kernel.cpp", "-o", output / "cpp.s"], "cpp_assembly_audit")
        result["assembly"] = {"Axeon": assembly_evidence(output / "axeon.s", "axeon_bench_kernel"),
                              "C++": assembly_evidence(output / "cpp.s", "cpp_bench_kernel")}
        references = {}
        for n in [0, 1, 31, 32, 33, 257, *INPUTS]:
            references[n] = int(runner.run([sys.executable, HERE / "kernel_python.py", "--reference", n], f"reference_{n}")["stdout"])
        result["reference_checksums"] = references
        result["small_correctness_checks"] = []
        for n in [0, 1, 31, 32, 33, 257]:
            small_source = output / f"correctness_{n}.ax"
            small_source.write_text(source.read_text().replace("return kernel(1000000);", f"return kernel({n});"))
            interpreted = runner.run([compiler, "interpret", small_source, "--fuel", "1000000"], f"interpreter_correctness_{n}")
            if interpreted["stdout"].strip() != f"result: {references[n]}":
                raise RuntimeError(f"interpreter mismatch for N={n}")
            for language in LANGUAGES:
                command = [binaries[language], n] if language != "Python" else [sys.executable, binaries[language], n]
                parse_process_output(runner.run(command, f"{language}_correctness_{n}")["stdout"], n, references[n])
            result["small_correctness_checks"].append({"n": n, "checksum": references[n], "all_four_engines": True})
        for trial in range(TRIALS):
            for n in INPUTS if trial % 2 == 0 else reversed(INPUTS):
                offset = (trial + INPUTS.index(n)) % len(LANGUAGES)
                for language in LANGUAGES[offset:] + LANGUAGES[:offset]:
                    command = [binaries[language], n] if language != "Python" else [sys.executable, binaries[language], n]
                    record = runner.run(command, f"measure_{language}_{n}_{trial}")
                    data = parse_process_output(record["stdout"], n, references[n])
                    kernel_total = data["cold_ns"] + data["warmup_ns"] + sum(data["warm_ns"])
                    data.update(language=language, trial=trial, peak_rss_kib=record["peak_rss_kib"],
                                process_wall_ns=record["wall_ns"], outside_kernel_ns=record["wall_ns"]-kernel_total,
                                user_cpu_seconds=record["user_cpu_seconds"], system_cpu_seconds=record["system_cpu_seconds"])
                    result["processes"].append(data)
                    (output / "partial.json").write_text(json.dumps(result, indent=2) + "\n")
        for n in INPUTS:
            axeon_groups = [p["warm_ns"] for p in result["processes"] if p["n"] == n and p["language"] == "Axeon"]
            axeon_median = statistics.median(v for group in axeon_groups for v in group)
            for language_index, language in enumerate(LANGUAGES):
                processes = [p for p in result["processes"] if p["n"] == n and p["language"] == language]
                groups = [p["warm_ns"] for p in processes]
                warm = summary([v for group in groups for v in group])
                result["summaries"].append({"n": n, "language": language, "warm": warm,
                    "warm_ci95_ns": cluster_bootstrap(groups, n + language_index),
                    "cold": summary([p["cold_ns"] for p in processes]),
                    "process_peak_rss_kib": summary([p["peak_rss_kib"] for p in processes]),
                    "proc_image_peak_rss_kib": summary([p["proc_image_peak_rss_kib"] for p in processes]),
                    "outside_kernel_ns": summary([p["outside_kernel_ns"] for p in processes]),
                    "time_divided_by_axeon": warm["median"] / axeon_median,
                    "speedup_ci95": [1.0, 1.0] if language == "Axeon" else speedup_bootstrap(groups, axeon_groups, n + language_index),
                    "expected_checksum": references[n], "correct": True})
        result["input_scaling_ratios"] = {}
        for language in LANGUAGES:
            rows = [r for r in result["summaries"] if r["language"] == language]
            result["input_scaling_ratios"][language] = rows[1]["warm"]["median"] / rows[0]["warm"]["median"]
        after_hashes = {str(path.relative_to(ROOT)): file_hash(path) for path in input_paths}
        if after_hashes != input_hashes or file_hash(compiler) != compiler_hash or file_hash(clang) != clang_hash:
            raise RuntimeError("benchmark inputs or tool binaries changed during execution")
        result["source_hashes"] = input_hashes
        result["inputs_unchanged_after_execution"] = True
        result["status"] = "passed"
        result["total_child_cpu_seconds"] = runner.cpu_seconds
        result["total_wall_seconds"] = 180 - (runner.deadline - time.monotonic())
        result["commands"] = runner.records
        write_reports(result, output)
        if not args.no_plot:
            runner.run([sys.executable, HERE / "plot_results.py", output / "results.json", output / "benchmark.png"], "plot_results")
        result["plot_status"] = "skipped_by_request" if args.no_plot else "generated"
        result["commands"] = runner.records
        result["total_child_cpu_seconds"] = runner.cpu_seconds
        result["total_wall_seconds"] = 180 - (runner.deadline - time.monotonic())
        write_reports(result, output)
        print(json.dumps({"status": "passed", "output": str(output), "summaries": result["summaries"]}, indent=2))
    except Exception as error:
        result.update(status="failed", error=str(error), commands=runner.records)
        (output / "failure.json").write_text(json.dumps(result, indent=2) + "\n")
        if (output / "results.json").exists():
            (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--execute", action="store_true", help="run only after aggregate correctness checks pass")
    parser.add_argument("--compiler", type=Path, default=ROOT / "target/release/axeon")
    parser.add_argument("--clang", type=Path, default=Path("/workspace/tools/sysroot/usr/lib/llvm-19/bin/clang"))
    parser.add_argument("--output", type=Path,
                        help="fresh directory; defaults to a dated local Evidence/aggregate-benchmark attempt")
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--compiler-build-seconds", type=float)
    parser.add_argument("--no-plot", action="store_true", help="write JSON/CSV evidence without the optional Matplotlib chart")
    args = parser.parse_args()
    if not args.execute:
        parser.error("no workloads run without --execute; complete the aggregate correctness gate first")
    if args.compiler_build_seconds is not None and args.compiler_build_seconds < 0:
        parser.error("compiler build duration cannot be negative")
    if args.output is None:
        try:
            layout = runpy.run_path(str(ROOT / "scripts/project_layout.py"))
            args.output = layout["reserve_evidence_output"]("aggregate-benchmark")
        except (OSError, ValueError) as error:
            parser.error(str(error).replace("--output-dir", "--output"))
    execute(args)


if __name__ == "__main__":
    main()

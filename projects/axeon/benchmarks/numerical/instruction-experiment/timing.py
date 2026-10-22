#!/usr/bin/env python3
"""Authorized, single-run paired timing of previously checked frozen objects."""
import argparse
import csv
import importlib.util
import json
import os
from pathlib import Path
import platform
import random
import resource
import statistics
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


base = load("benchmark_support", ROOT / "benchmarks/run.py")
static = load("instruction_static", HERE / "static.py")
reference = load("numerical_reference", HERE.parent / "kernels.py")


def paired_interval(numerator, denominator, seed):
    rng = random.Random(seed)
    ratios = []
    for _ in range(1000):
        selected = rng.choices(range(3), k=3)
        a = [value for index in selected for value in numerator[index]]
        b = [value for index in selected for value in denominator[index]]
        ratios.append(statistics.median(a) / statistics.median(b))
    return [base.percentile(ratios, .025), base.percentile(ratios, .975)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--execute", action="store_true", required=True)
    args = parser.parse_args()
    if not args.execute:
        raise ValueError("timing requires --execute")
    destination = HERE / "timing.json"
    if destination.exists():
        raise ValueError("timing receipt already exists; refusing an unrecorded rerun")
    beginning = time.perf_counter_ns()
    own_before = resource.getrusage(resource.RUSAGE_SELF)
    thread_env = {name: "1" for name in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS",
                                         "BLIS_NUM_THREADS", "VECLIB_MAXIMUM_THREADS", "NUMEXPR_NUM_THREADS")}
    os.environ.update(thread_env)
    affinity = sorted(os.sched_getaffinity(0))
    cpu = 0
    if cpu not in affinity:
        raise ValueError("original benchmark CPU 0 is unavailable")
    runner = base.Runner(HERE / "artifacts", cpu)
    report = {"status": "running", "optimization": 2, "inputs": [1000, 4000],
              "trials": 3, "warm_samples_per_process": 7, "processes": [], "summaries": [],
              "comparison_intervals": [], "bootstrap_method": "1000 paired process-cluster resamples, seed 731 + comparison index; exploratory with only three clusters",
              "source_hashes_before": static.hashes(),
              "driver_sha256": base.file_hash(ROOT / "benchmarks/driver.c"),
              "static_receipt_sha256": base.file_hash(HERE / "static.json"),
              "host": {"platform": platform.platform(), "allowed_cpus": affinity, "pinned_cpu": cpu,
                       "cpuinfo": Path("/proc/cpuinfo").read_text(), "thread_environment": thread_env},
              "limits": {"process_virtual_memory_bytes": 2 * 1024**3, "process_cpu_seconds": 30,
                         "overall_wall_seconds": 180, "overall_child_cpu_seconds": 180},
              "kernel_object_hashes": {}, "executable_hashes": {}}
    try:
        capability = runner.run([HERE / "artifacts/capability"], "baseline_cpu_os_probe")
        report["capability_probe"] = json.loads(capability["stdout"])
        if not report["capability_probe"]["avx2"]:
            raise ValueError("CPU/OS AVX2 support required")
        existing = json.loads((HERE / "static.json").read_text())
        if existing["status"] != "passed" or report["source_hashes_before"] != existing["source_hashes_after"]:
            raise ValueError("static checks or frozen source hashes changed")
        binaries = {}
        for language, symbol in (("axeon", "axeon_bench_kernel"), ("cpp", "cpp_bench_kernel")):
            driver = HERE / "artifacts" / f"timing-driver-{language}.o"
            runner.run([static.CLANG, *static.FLAGS, "-O2", "-std=c11", f"-DBENCH_KERNEL={symbol}",
                        "-c", ROOT / "benchmarks/driver.c", "-o", driver], f"{language}_timing_driver_compile")
            for workload in ("scalar", "matrix"):
                for target in ("baseline", "avx2"):
                    stem = f"{workload}-{language}-O2-{target}"
                    obj = HERE / "artifacts" / f"{stem}.o"
                    binary = HERE / "artifacts" / f"{stem}-timing"
                    report["kernel_object_hashes"][stem] = base.file_hash(obj)
                    extra = ["-mavx2"] if target == "avx2" else []
                    runner.run([static.CLANG, *static.FLAGS, "-O2", *extra, obj, driver,
                                HERE / "artifacts/runtime.o", "-o", binary], f"{stem}_timing_link")
                    report["executable_hashes"][stem] = base.file_hash(binary)
                    binaries[workload, language, target] = binary
        measurement_start = time.perf_counter_ns()
        variants = [("axeon", "baseline"), ("axeon", "avx2"), ("cpp", "baseline"), ("cpp", "avx2")]
        for workload in ("scalar", "matrix"):
            for n in (1000, 4000):
                expected = reference.independent_reference(workload, n)["checksum"]
                for trial in range(3):
                    # Rotate deterministic orders to limit monotonic ordering bias.
                    order = variants[trial:] + variants[:trial]
                    for language, target in order:
                        record = runner.run([binaries[workload, language, target], n],
                                            f"{workload}_{n}_{language}_{target}_trial{trial}")
                        data = base.parse_process_output(record["stdout"], n, expected)
                        data.update(workload=workload, language=language, cpu_target=target, trial=trial,
                                    process_wall_ns=record["wall_ns"], wait4_peak_rss_kib=record["peak_rss_kib"],
                                    user_cpu_seconds=record["user_cpu_seconds"], system_cpu_seconds=record["system_cpu_seconds"])
                        report["processes"].append(data)
        report["measurement_window_wall_ns"] = time.perf_counter_ns() - measurement_start
        grouped = {}
        for workload in ("scalar", "matrix"):
            for n in (1000, 4000):
                for language, target in variants:
                    processes = [p for p in report["processes"] if
                                 (p["workload"], p["n"], p["language"], p["cpu_target"]) == (workload, n, language, target)]
                    groups = [p["warm_ns"] for p in processes]
                    grouped[workload, n, language, target] = groups
                    report["summaries"].append({"workload": workload, "n": n, "language": language,
                                                "cpu_target": target, "warm_ns": base.summary([v for g in groups for v in g]),
                                                "cluster_median_ci95_ns": base.cluster_bootstrap(groups, 731)})
                comparisons = [(f"{language}_baseline_over_avx2", (language, "baseline"), (language, "avx2"))
                               for language in ("axeon", "cpp")]
                comparisons += [(f"{target}_cpp_over_axeon", ("cpp", target), ("axeon", target))
                                for target in ("baseline", "avx2")]
                for name, numerator, denominator in comparisons:
                    a, b = grouped[workload, n, *numerator], grouped[workload, n, *denominator]
                    report["comparison_intervals"].append({"workload": workload, "n": n, "comparison": name,
                        "median_ratio": statistics.median(v for g in a for v in g) / statistics.median(v for g in b for v in g),
                        "paired_process_ci95": paired_interval(a, b, 731 + len(report["comparison_intervals"]))})
        report["source_hashes_after"] = static.hashes()
        if report["source_hashes_before"] != report["source_hashes_after"]:
            raise ValueError("source changed during timing")
        report["status"] = "passed"
    finally:
        own_after = resource.getrusage(resource.RUSAGE_SELF)
        report["total_wall_ns"] = time.perf_counter_ns() - beginning
        report["runner_user_cpu_seconds"] = own_after.ru_utime - own_before.ru_utime
        report["runner_system_cpu_seconds"] = own_after.ru_stime - own_before.ru_stime
        report["total_child_cpu_seconds"] = runner.cpu_seconds
        report["commands"] = runner.records
        destination.write_text(json.dumps(report, indent=2) + "\n")
        with (HERE / "timing-samples.csv").open("w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(["workload", "n", "language", "cpu_target", "trial", "phase", "sample", "nanoseconds", "checksum"])
            for p in report["processes"]:
                phases = [("cold", 0, p["cold_ns"]), ("warmup", 0, p["warmup_ns"])] + [("warm", i, v) for i, v in enumerate(p["warm_ns"])]
                for index, (phase, sample, duration) in enumerate(phases):
                    writer.writerow([p["workload"], p["n"], p["language"], p["cpu_target"], p["trial"], phase, sample, duration, p["checksums"][index]])
    print(json.dumps({"status": report["status"], "processes": len(report["processes"]),
                      "whole_wall_seconds": report["total_wall_ns"] / 1e9,
                      "child_cpu_seconds": report["total_child_cpu_seconds"],
                      "comparisons": report["comparison_intervals"]}, indent=2))


if __name__ == "__main__":
    main()

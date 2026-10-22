#!/usr/bin/env python3
"""Compare ordinary compiler timings or separately validated allocation counts.

timing --compiler-a BASE --compiler-b CANDIDATE --output-dir NEW [--trials 7]
allocations --report-a BASE/allocations.json --report-b CAND/allocations.json --output-dir NEW

No builds, installations or workload substitutions. Linux/Python standard library.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import statistics
import sys


ALLOCATION_PATH = Path(__file__).with_name("profile-allocations.py")
_spec = importlib.util.spec_from_file_location("axeon_comparison_allocations", ALLOCATION_PATH)
allocation = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(allocation)
shared = allocation.shared
demand = allocation.demand
METRICS = ("wall_seconds", "user_cpu_seconds", "system_cpu_seconds", "max_rss_kib")
HOST_KEYS = ("platform", "machine", "kernel", "cpu_model", "affinity")


def require_stable_hosts(snapshots, context):
    baseline = snapshots[0]
    for snapshot in snapshots[1:]:
        changed = [key for key in HOST_KEYS if snapshot[key] != baseline[key]]
        demand(not changed, f"{context} recorded host keys changed: {', '.join(changed)}")


class Frozen:
    def __init__(self):
        self.files = {}

    def add(self, path, expected=None):
        path = Path(path).absolute()
        observed = allocation.fingerprint(path)
        wanted = expected if expected is not None else self.files.get(path, observed)
        demand(observed == {k: wanted[k] for k in ("bytes", "sha256")}, f"file changed: {path}")
        self.files[path] = observed
        return {"path": str(path), **observed}

    def check(self):
        allocation.unchanged(self.files)


class Receipt:
    """Atomic updates confined to the freshly created comparison directory."""
    def __init__(self, directory, report):
        self.directory, self.report, self.previous = directory, report, None

    def save(self):
        target = self.directory / "comparison.json"
        if self.previous is not None:
            demand(allocation.fingerprint(target) == self.previous, "comparison receipt changed externally")
        else:
            demand(not target.exists(), "comparison receipt already exists")
        temporary = self.directory / "comparison.json.tmp"
        with temporary.open("x", encoding="utf-8") as output:
            json.dump(self.report, output, indent=2, allow_nan=False)
            output.write("\n")
        temporary.replace(target)
        self.previous = allocation.fingerprint(target)


def timing_order(round_number):
    # Warmup AB; measured rounds AB, BA, AB, BA, ... . Odd totals are disclosed.
    return ("a", "b") if round_number == 0 or round_number % 2 else ("b", "a")


def timing_summaries(samples, trials):
    result = {}
    for workload in shared.WORKLOADS:
        chosen = [s for s in samples if s["workload"] == workload and s["phase"] == "measured"]
        demand(len(chosen) == 2 * trials and all(s["valid"] for s in chosen), "incomplete measured pairs")
        pairs = []
        for trial in range(1, trials + 1):
            members = [s for s in chosen if s["trial"] == trial]
            demand([s["side"] for s in members] == list(timing_order(trial)), "missing, duplicate or reordered pair")
            pair = {s["side"]: s for s in members}
            for sample in members:
                demand(all(type(sample[k]) in (int, float) and math.isfinite(sample[k]) and sample[k] >= 0
                           for k in METRICS), "invalid process measurement")
                demand(sample["wall_seconds"] > 0, "nonpositive wall time")
            pairs.append({"trial": trial, "order": "".join(timing_order(trial)).upper(),
                          "wall_ratio_b_over_a": pair["b"]["wall_seconds"] / pair["a"]["wall_seconds"],
                          "wall_difference_b_minus_a_seconds": pair["b"]["wall_seconds"] - pair["a"]["wall_seconds"]})
        result[workload] = {
            side: {metric: shared.summarize([s[metric] for s in chosen if s["side"] == side]) for metric in METRICS}
            for side in ("a", "b")}
        result[workload].update({
            "pairs": pairs,
            "paired_wall_ratio_b_over_a": shared.summarize([p["wall_ratio_b_over_a"] for p in pairs]),
            "paired_wall_difference_median_seconds": statistics.median([p["wall_difference_b_minus_a_seconds"] for p in pairs]),
            "wall_ratio_by_order": {order: shared.summarize([p["wall_ratio_b_over_a"] for p in pairs if p["order"] == order])
                                    for order in ("AB", "BA") if any(p["order"] == order for p in pairs)},
        })
    return result


def compare_timing(args, report, receipt):
    directory = args.output_dir
    sources, expected_stdout = shared.generated_sources()
    immutable, outputs = Frozen(), Frozen()
    compilers = {"a": args.compiler_a, "b": args.compiler_b}
    report["compilers"] = {side: immutable.add(path) for side, path in compilers.items()}
    for path in (Path(__file__), ALLOCATION_PATH, allocation.SHARED_PATH):
        immutable.add(path)
    report["harnesses"] = [{"path": str(p), **v} for p, v in immutable.files.items() if p not in compilers.values()]
    report["inputs"] = {}
    for name, source in sources.items():
        path = directory / name
        with path.open("xb") as stream:
            stream.write(source)
        report["inputs"][name] = immutable.add(path)
    samples_dir = directory / "samples"
    samples_dir.mkdir()
    environment = os.environ.copy()
    controls = {"LC_ALL": "C", "LANG": "C", "TZ": "UTC", "SOURCE_DATE_EPOCH": "0"}
    environment.update(controls)
    report.update({"workload_version": shared.WORKLOAD_VERSION,
                   "configuration": {"trials": args.trials, "warmups": 1, "timeout_seconds": args.timeout_seconds,
                                     "workload_order": list(shared.WORKLOADS), "order": "adjacent pairs; warmup AB; measured AB, BA alternating",
                                     "controlled_environment": controls, "max_child_file_bytes": shared.MAX_CHILD_FILE_BYTES,
                                     "stable_recorded_host_keys": list(HOST_KEYS)},
                   "scope": {"wall": "Ordinary compiler Popen through wait4, including startup/output/exit. Hashing excluded.",
                             "cache": "New process every sample. Both binaries and identical inputs hashed before/after each invocation; warmed filesystem pages, no cold-cache claim.",
                             "rss": "wait4 peak KiB, including possible Python pre-exec floor; no allocation-count or simultaneous-tree interpretation.",
                             "validation": "Exact shared version/stdout/stderr/LLVM signatures and independent arithmetic interpreter result; LLVM native execution is outside this harness.",
                             "statistics": "All measured pairs retained. R7 quartiles and population variance; B/A wall ratio below 1 favors B. No confidence interval or significance claim; inspect both pair orders. Odd trial counts have one extra AB pair.",
                             "rejection": "Complete unchanged valid formatter source intentionally returns E_FORMAT_LIMIT/exit 1. Not accepted-program formatting throughput.",
                             "limits": "One host, no thermal/background-load control or universal advantage claim. No instrumented timing is used."}})

    def invoke(side, workload, phase, trial, arguments, artifact=None):
        immutable.check()
        report["active_invocation"] = {"side": side, "workload": workload, "phase": phase, "trial": trial}
        receipt.save()
        prefix = samples_dir / f"{workload}-{phase}-{trial:03}-{side}"
        sample = shared.run_child([compilers[side], *arguments], directory,
                                  prefix.with_suffix(".stdout"), prefix.with_suffix(".stderr"),
                                  args.timeout_seconds, environment)
        sample.update({"side": side, "workload": workload, "phase": phase, "trial": trial, "valid": False})
        report["preparation" if phase == "preparation" else "samples"].append(sample)
        # Preserve the failed process even if its output files violate a limit.
        receipt.save()
        for stream in ("stdout", "stderr"):
            outputs.add(sample[stream]["path"], sample[stream])
        if artifact is not None:
            sample["artifact"] = outputs.add(artifact)
        immutable.check()
        return sample

    identities = []
    for side in ("a", "b"):
        identity = invoke(side, "identity", "preparation", 0, ["--version"])
        shared.require_result(identity, 0, stderr_empty=True)
        version = Path(identity["stdout"]["path"]).read_text(encoding="utf-8")
        demand(version.startswith("axeon ") and len(version.splitlines()) == 1, "invalid compiler identity")
        identities.append(version)
        report["compilers"][side]["version_output"] = version
        identity["valid"] = True
    demand(identities[0] == identities[1], "compiler identities differ, including version/subset/LLVM configuration")
    version_hash = hashlib.sha256(identities[0].encode()).hexdigest()
    for side in ("a", "b"):
        correct = invoke(side, "arithmetic-correctness", "preparation", 0, ["interpret", "arithmetic.ax"])
        shared.require_result(correct, 0, hashlib.sha256(expected_stdout).hexdigest(), stderr_empty=True)
        correct["valid"] = True
        valid = invoke(side, "format-input-check", "preparation", 0, ["check", "format-budget.ax"])
        shared.require_result(valid, 0, shared.EMPTY_SHA256, stderr_empty=True)
        valid["valid"] = True
    report["expected_arithmetic_stdout"] = expected_stdout.decode("ascii")
    references = {}
    for trial in range(args.trials + 1):
        phase = "warmup" if trial == 0 else "measured"
        for workload in shared.WORKLOADS:
            for side in timing_order(trial):
                artifact = None
                if workload == "startup":
                    command = ["--version"]
                elif workload == "check":
                    command = ["check", "arithmetic.ax"]
                elif workload == "emit-ir":
                    artifact = samples_dir / f"{workload}-{phase}-{trial:03}-{side}.ll"
                    command = ["emit-ir", "arithmetic.ax", "-o", artifact]
                else:
                    command = ["format", "format-budget.ax"]
                sample = invoke(side, workload, phase, trial, command, artifact)
                if workload == "format-budget-rejection":
                    shared.require_result(sample, 1, shared.EMPTY_SHA256, stderr_contains=b"error[E_FORMAT_LIMIT]")
                else:
                    shared.require_result(sample, 0, version_hash if workload == "startup" else shared.EMPTY_SHA256, stderr_empty=True)
                if artifact is not None:
                    demand(sample["artifact"]["bytes"] > 0, "empty LLVM output")
                    with artifact.open("rb") as stream:
                        demand(b"target triple = " in stream.read(4096), "LLVM output lacks target declaration")
                signature = shared.output_signature(sample)
                if workload not in references:
                    references[workload] = signature
                demand(signature == references[workload], f"{workload}: outputs differ across compilers/trials")
                sample["valid"] = True
                report["active_invocation"] = None
                receipt.save()
            # Recheck accepted outputs outside process timings after each pair.
            outputs.check()
    immutable.check()
    outputs.check()
    report["common_output_signatures"] = references
    report["summaries"] = timing_summaries(report["samples"], args.trials)


def read_large_json(path):
    allocation.regular(path, 16 * 1024 * 1024)

    def unique(pairs):
        result = {}
        for key, value in pairs:
            demand(key not in result, "duplicate JSON key")
            result[key] = value
        return result

    return json.loads(Path(path).read_text(encoding="utf-8"), object_pairs_hook=unique,
                      parse_constant=lambda value: (_ for _ in ()).throw(allocation.AllocationError(f"nonfinite JSON {value}")))


def validate_allocation_run(path, frozen):
    frozen.add(path)
    report = read_large_json(path)
    demand(report.get("schema_version") == 1 and report.get("status") == "passed"
           and report.get("workload_version") == allocation.VERSION, "unsupported or failed allocation run")
    for name in ("compiler", "profiler", "harness", "shared_harness"):
        frozen.add(report[name]["path"], report[name])
    demand(report["harness"]["sha256"] == shared.digest(ALLOCATION_PATH)
           and report["shared_harness"]["sha256"] == shared.digest(allocation.SHARED_PATH), "allocation validator/harness revision differs")
    sources, arithmetic = shared.generated_sources()
    sources.update(allocation.MODULE_SOURCES)
    wanted_inputs = {name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()} for name, data in sources.items()}
    demand(report["inputs"] == wanted_inputs, "allocation inputs differ from fixed shared workloads")
    for name, expected in wanted_inputs.items():
        frozen.add(path.parent / "inputs" / name, expected)
    config = report["configuration"]
    trials = config["trials"]
    demand(type(trials) is int and 1 <= trials <= 100 and config["warmups"] == 1
           and config["fuel"] == allocation.FUEL and config["workload_order"] == list(allocation.WORKLOADS), "invalid allocation configuration")
    preparations = report["preparation"]
    expected_names = ["compiler-version", "arithmetic-project-check", "arithmetic-project-interpret", "arithmetic-project-emit-ir",
                      "aggregate-modules-project-check", "aggregate-modules-project-interpret", "aggregate-modules-project-emit-ir",
                      "format-success", "format-budget-valid-source", "format-budget-rejection"]
    demand([p["name"] for p in preparations] == expected_names, "missing or reordered ordinary-compiler preparation")
    prep = {p["name"]: p for p in preparations}
    for p in preparations:
        demand(p["valid"] is True, "invalid preparation")
        shared.require_result(p, 1 if p["name"] == "format-budget-rejection" else 0,
                              stderr_empty=p["name"] != "format-budget-rejection",
                              stderr_contains=b"error[E_FORMAT_LIMIT]" if p["name"] == "format-budget-rejection" else None)
        for item in ("stdout", "stderr", "artifact"):
            if item in p:
                frozen.add(p[item]["path"], p[item])
        if p["name"].endswith(("-check", "-emit-ir")) or p["name"] in ("format-budget-valid-source", "format-budget-rejection"):
            demand(p["stdout"]["sha256"] == shared.EMPTY_SHA256, "ordinary preparation unexpectedly printed output")
    identity = Path(prep["compiler-version"]["stdout"]["path"]).read_text()
    demand(identity == report["compiler"]["version_output"] and identity.startswith("axeon ")
           and len(identity.splitlines()) == 1, "allocation compiler identity mismatch")
    expectations = {}
    for name, expected in (("arithmetic-project", arithmetic), ("aggregate-modules-project", b"f64:0x8000000000000000\n42\nresult: 42\n")):
        demand(Path(prep[name + "-interpret"]["stdout"]["path"]).read_bytes() == expected, "independent arithmetic/module result differs")
        with Path(prep[name + "-emit-ir"]["artifact"]["path"]).open("rb") as stream:
            demand(b"target triple = " in stream.read(4096), "allocation LLVM oracle lacks target declaration")
        parts = expected.rsplit(b"result: ", 1)
        wanted_sources = {"arithmetic.ax": len(sources["arithmetic.ax"])} if name == "arithmetic-project" else {
            Path(p).name: len(data) for p, data in allocation.MODULE_SOURCES.items()}
        expectations[name] = {"mode": "project", "outcome": "success", "exit": 0, "codes": [],
                              "result": int(parts[1]), "sources": wanted_sources,
                              "artifacts": {"emitted.ll": {"sha256": prep[name + "-emit-ir"]["artifact"]["sha256"]},
                                            "stdout.txt": {"sha256": hashlib.sha256(parts[0]).hexdigest()}}}
    expectations["format-success"] = {"mode": "format", "outcome": "success", "exit": 0, "codes": [],
        "result": None, "sources": {"arithmetic.ax": len(sources["arithmetic.ax"])},
        "artifacts": {"formatted.ax": {"sha256": prep["format-success"]["stdout"]["sha256"]}}}
    expectations["format-budget-rejection"] = {"mode": "format", "outcome": "format_error", "exit": 1,
        "codes": ["E_FORMAT_LIMIT"], "result": None, "sources": {"format-budget.ax": len(sources["format-budget.ax"])},
        "artifacts": {"diagnostics.txt": {"contains": "error[E_FORMAT_LIMIT]"}}}
    demand(report["expected_results"] == expectations, "allocation output oracle mismatch")
    diagnostics = Path(prep["format-budget-rejection"]["stderr"]["path"]).read_bytes()
    prefix = str(path.parent / "inputs/format-budget.ax").encode()
    demand(diagnostics.startswith(prefix + b":"), "unexpected diagnostic source prefix")
    seen = set()
    for sample in report["samples"]:
        demand(type(sample["trial"]) is int, "allocation trial must be an integer")
        key = (sample["workload"], sample["round_kind"], sample["trial"])
        demand(key not in seen and sample["valid"] is True, "duplicate or invalid allocation sample")
        seen.add(key)
        expected = report["expected_results"][sample["workload"]]
        actual, files = allocation.validate_result(sample["directory"], sample["process"], expected)
        demand(actual == sample["allocation_report"], "inline allocation report differs from raw report")
        demand({p.name: v for p, v in files.items()} == sample["outputs"], "allocation output inventory differs")
        for p, metadata in files.items():
            frozen.add(p, metadata)
        for stream in ("stdout", "stderr"):
            frozen.add(sample["process"][stream]["path"], sample["process"][stream])
        if sample["workload"] == "format-budget-rejection":
            demand(Path(sample["directory"], "diagnostics.txt").read_bytes() == diagnostics, "rejection differs from ordinary compiler")
    demand(seen == {(w, "warmup" if i == 0 else "measured", i) for w in allocation.WORKLOADS for i in range(trials + 1)}, "missing/extra allocation trials")
    demand(allocation.summarize_samples(report["samples"]) == report["summaries"], "allocation distributions differ from raw counters")
    return report, b"format-budget.ax" + diagnostics[len(prefix):]


def allocation_differences(a, b):
    result = {}
    for workload in allocation.WORKLOADS:
        result[workload] = {}
        demand(a[workload].keys() == b[workload].keys(), "allocation phase sets differ")
        for phase, left in a[workload].items():
            right = b[workload][phase]
            changes = {}
            for metric in (*allocation.STATE, *allocation.EVENTS):
                x, y = (left[metric], right[metric]) if metric in allocation.STATE else (left["events"][metric], right["events"][metric])
                changes[metric] = {"a": x, "b": y, "median_b_minus_a": y["median"] - x["median"],
                                   "median_ratio_b_over_a": y["median"] / x["median"] if x["median"] else None}
            changes["net_live_requested_bytes_raw"] = {"a": left["net_live_requested_bytes_raw"], "b": right["net_live_requested_bytes_raw"]}
            result[workload][phase] = changes
    return result


def compare_allocations(args, report, receipt):
    frozen = Frozen()
    for p in (Path(__file__), ALLOCATION_PATH, allocation.SHARED_PATH):
        frozen.add(p)
    report["runs"] = {}
    normalized = {}
    for side, path in (("a", args.report_a), ("b", args.report_b)):
        report["active_validation"] = {"side": side, "report": str(path)}
        receipt.save()
        run, diagnostics = validate_allocation_run(path, frozen)
        normalized[side] = diagnostics
        report["runs"][side] = {"origin": frozen.add(path), "report": run}
        receipt.save()
    a, b = (report["runs"][side]["report"] for side in ("a", "b"))
    for key in ("inputs", "configuration", "expected_results", "scope"):
        demand(a[key] == b[key], f"allocation {key} differ")
    demand(a["compiler"]["version_output"] == b["compiler"]["version_output"], "allocation compiler versions differ")
    for key in ("harness", "shared_harness"):
        demand(a[key]["sha256"] == b[key]["sha256"], "allocation harness hashes differ")
    require_stable_hosts([a["host_before"], a["host_after"], b["host_before"], b["host_after"]], "allocation")
    demand(normalized["a"] == normalized["b"], "allocation diagnostic code/span/message differs")
    report["scope"] = {"measurement": "Only requested-size allocation count/byte distributions; no instrumented time or RSS comparison.",
                       "pairing": "Independent allocation runs with identical workloads; not randomized timing pairs.",
                       "diagnostics": "Original diagnostic hashes retained; only exact declared absolute source prefix replaced with format-budget.ax for cross-directory equality.",
                       "limits": "Not physical heap, source allocation expressions, allocation sites, native/device memory, leak verdict or proof of a speedup. Peaks include entry baselines."}
    report["differences"] = allocation_differences(a["summaries"], b["summaries"])
    frozen.check()
    report["active_validation"] = None


def execute(args):
    report = {"schema_version": 1, "mode": args.mode, "label": args.label, "status": "running",
              "captured_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "host_before": shared.host_configuration(), "preparation": [], "samples": [], "summaries": {}}
    receipt = Receipt(args.output_dir, report)
    try:
        receipt.save()
        (compare_timing if args.mode == "timing" else compare_allocations)(args, report, receipt)
        report["host_after"] = shared.host_configuration()
        if args.mode == "timing":
            require_stable_hosts([report["host_before"], report["host_after"]], "timing")
        report["status"], code = "passed", 0
    except (allocation.AllocationError, shared.ProfileError, OSError, ValueError, KeyError, TypeError) as error:
        report.update(status="failed", failure=str(error))
        code = 1
    except KeyboardInterrupt:
        report.update(status="interrupted", failure="interrupted; shared runner terminates only its owned active child")
        code = 130
    if "host_after" not in report:
        report["host_after"] = shared.host_configuration()
    try:
        receipt.save()
    except (allocation.AllocationError, OSError, ValueError) as error:
        print(json.dumps({"status": "failed", "report": str(args.output_dir / "comparison.json"),
                          "failure": f"could not publish final receipt; existing evidence was not replaced: {error}"}))
        return 1
    print(json.dumps({"status": report["status"], "report": str(args.output_dir / "comparison.json"), "failure": report.get("failure")}))
    return code


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="mode", required=True)
    timing = subparsers.add_parser("timing")
    timing.add_argument("--compiler-a", type=Path, required=True)
    timing.add_argument("--compiler-b", type=Path, required=True)
    timing.add_argument("--trials", type=int, default=7)
    timing.add_argument("--timeout-seconds", type=float, default=120)
    allocations = subparsers.add_parser("allocations")
    allocations.add_argument("--report-a", type=Path, required=True)
    allocations.add_argument("--report-b", type=Path, required=True)
    for subparser in (timing, allocations):
        subparser.add_argument("--output-dir", type=Path, required=True)
        subparser.add_argument("--label", default="unspecified")
    args = parser.parse_args(argv)
    if sys.platform != "linux" or not hasattr(os, "wait4"):
        parser.error("Linux os.wait4 is required")
    if args.mode == "timing" and (not 1 <= args.trials <= 100 or not math.isfinite(args.timeout_seconds) or args.timeout_seconds <= 0):
        parser.error("trials must be 1..100 and timeout finite/positive")
    try:
        for name in (("compiler_a", "compiler_b") if args.mode == "timing" else ("report_a", "report_b")):
            path = getattr(args, name).expanduser().absolute()
            allocation.regular(path, shared.MAX_CHILD_FILE_BYTES)
            if args.mode == "timing" and not os.access(path, os.X_OK):
                parser.error(f"not executable: {path}")
            setattr(args, name, path)
        args.output_dir = args.output_dir.expanduser().absolute()
        args.output_dir.mkdir()  # Exclusive: no cleanup or reuse of old evidence.
    except (OSError, allocation.AllocationError) as error:
        parser.error(str(error))
    return execute(args)


if __name__ == "__main__":
    raise SystemExit(main())

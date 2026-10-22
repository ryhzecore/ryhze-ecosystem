#!/usr/bin/env python3
"""Explicit subset-11 to subset-12 timing/allocation comparisons without builds.

Timing uses the accepted checkpoint-0.12 package. Allocation comparison uses
the accepted symbolic-body-05 prerequisite. Exact side identities replace only
the historical same-version requirement; workload/host/output checks remain.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

sys.dont_write_bytecode = True
HELPER_PATH = Path(__file__).with_name("compare-generic-transition.py")
_spec = importlib.util.spec_from_file_location("generic_transition_common", HELPER_PATH)
common = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(common)
need = common.need
FORMAT_ERROR = (b"format-budget.ax:300001:1: error[E_FORMAT_LIMIT]: formatted source "
                b"exceeds the 1 MiB source limit (bytes 900065..900066)\n")


def inputs(args, report):
    frozen = common.Frozen()
    frozen.add(Path(__file__))
    frozen.add(HELPER_PATH)
    manifest, index = common.verify_candidate(args.snapshot_root, args.candidate_sha256, frozen)
    report["candidate"] = {"snapshot": str(args.snapshot_root), "capture_commit": manifest["source_commit"],
                           "capture_status": manifest["status"], "compiler_sha256": args.candidate_sha256}
    return frozen, index


def timing(args, report, receipt, comparison):
    frozen, _ = inputs(args, report)
    package = common.verify_package(args.accepted_package, frozen)
    outputs = common.Frozen()
    shared = comparison.shared
    need(tuple(shared.WORKLOADS) == ("startup", "check", "emit-ir", "format-budget-rejection"),
         "fixed timing workloads changed")
    compilers = {"a": args.accepted_package / "bin/axeon", "b": args.snapshot_root / "target/release/axeon"}
    report["compilers"] = {side: frozen.add(path) for side, path in compilers.items()}
    report["accepted"] = {"source_commit": package["source_commit"], "package": str(args.accepted_package)}
    sources, expected = shared.generated_sources()
    report["inputs"] = {}
    for name, content in sources.items():
        path = args.output_dir / name
        with path.open("xb") as target:
            target.write(content)
        report["inputs"][name] = frozen.add(path)
    samples_dir = args.output_dir / "samples"
    samples_dir.mkdir()
    environment = os.environ.copy()
    controls = {"LC_ALL": "C", "LANG": "C", "TZ": "UTC", "SOURCE_DATE_EPOCH": "0"}
    environment.update(controls)
    report.update(workload_version=shared.WORKLOAD_VERSION,
                  configuration={"trials": args.trials, "warmups": 1, "timeout_seconds": args.timeout_seconds,
                                 "workload_order": list(shared.WORKLOADS),
                                 "order": "adjacent pairs; warmup AB; measured AB, BA alternating",
                                 "controlled_environment": controls,
                                 "max_child_file_bytes": shared.MAX_CHILD_FILE_BYTES,
                                 "stable_recorded_host_keys": list(comparison.HOST_KEYS)},
                  expected_arithmetic_stdout=expected.decode("ascii"),
                  expected_format_stderr=FORMAT_ERROR.decode("ascii"),
                  scope={"comparison": "Whole accepted-package to bounded-generic candidate transition. Versions, features, source and build paths differ; no generic-only or verifier-only causal claim.",
                         "wall": "Ordinary compiler Popen through wait4 including startup/output/exit. Hashing and report writes excluded from timed intervals.",
                         "cache": "New process per sample. Both binaries and identical inputs hashed around each invocation; warmed filesystem pages, not cold-cache performance.",
                         "rss": "wait4 peak KiB includes possible inherited Python memory floor; not simultaneous process-tree memory or allocation counts.",
                         "validation": "Exact designated version per side; common arithmetic answers, diagnostics and LLVM across both sides and trials. Native execution is a separate gate.",
                         "statistics": "All pairs retained; R7 quartiles, population variance and order-split ratios. No significance, universal speedup or causal optimization claim. Odd trials have an extra AB pair.",
                         "rejection": "Valid unchanged formatter source exceeds its output budget; not accepted-program formatting throughput.",
                         "limits": "One host; affinity recorded equally, not pinned; no thermal/frequency/background-load control. Invoke under bounded outer worker timeout."})

    def invoke(side, workload, phase, trial, arguments, artifact=None):
        frozen.check()
        report["active_invocation"] = dict(side=side, workload=workload, phase=phase, trial=trial)
        receipt.save()
        prefix = samples_dir / f"{workload}-{phase}-{trial:03}-{side}"
        sample = shared.run_child([compilers[side], *arguments], args.output_dir,
                                  prefix.with_suffix(".stdout"), prefix.with_suffix(".stderr"),
                                  args.timeout_seconds, environment)
        sample.update(side=side, workload=workload, phase=phase, trial=trial, valid=False)
        report["preparation" if phase == "preparation" else "samples"].append(sample)
        receipt.save()
        for stream in ("stdout", "stderr"):
            outputs.add(sample[stream]["path"], sample[stream])
        if artifact is not None:
            sample["artifact"] = outputs.add(artifact)
        frozen.check()
        return sample

    version_hashes = {side: hashlib.sha256(value.encode()).hexdigest() for side, value in common.VERSIONS.items()}
    for side in ("a", "b"):
        identity = invoke(side, "identity", "preparation", 0, ["--version"])
        shared.require_result(identity, 0, version_hashes[side], stderr_empty=True)
        report["compilers"][side]["version_output"] = common.VERSIONS[side]
        identity["valid"] = True
        correct = invoke(side, "arithmetic-correctness", "preparation", 0, ["interpret", "arithmetic.ax"])
        shared.require_result(correct, 0, hashlib.sha256(expected).hexdigest(), stderr_empty=True)
        correct["valid"] = True
        valid = invoke(side, "format-input-check", "preparation", 0, ["check", "format-budget.ax"])
        shared.require_result(valid, 0, shared.EMPTY_SHA256, stderr_empty=True)
        valid["valid"] = True
    references = {}
    for trial in range(args.trials + 1):
        phase = "warmup" if trial == 0 else "measured"
        for workload in shared.WORKLOADS:
            for side in comparison.timing_order(trial):
                artifact = None
                if workload == "startup":
                    arguments = ["--version"]
                elif workload == "check":
                    arguments = ["check", "arithmetic.ax"]
                elif workload == "emit-ir":
                    artifact = samples_dir / f"{workload}-{phase}-{trial:03}-{side}.ll"
                    arguments = ["emit-ir", "arithmetic.ax", "-o", artifact]
                else:
                    arguments = ["format", "format-budget.ax"]
                sample = invoke(side, workload, phase, trial, arguments, artifact)
                if workload == "format-budget-rejection":
                    shared.require_result(sample, 1, shared.EMPTY_SHA256, stderr_contains=b"error[E_FORMAT_LIMIT]")
                    need(sample["stderr"]["sha256"] == hashlib.sha256(FORMAT_ERROR).hexdigest(),
                         "formatter diagnostic code/span/message changed")
                else:
                    wanted = version_hashes[side] if workload == "startup" else shared.EMPTY_SHA256
                    shared.require_result(sample, 0, wanted, stderr_empty=True)
                if artifact is not None:
                    need(sample["artifact"]["bytes"] > 0, "empty LLVM output")
                    with artifact.open("rb") as source:
                        need(b'target triple = "x86_64-pc-linux-gnu"' in source.read(4096),
                             "LLVM target changed")
                signature = shared.output_signature(sample)
                # Only version output differs. Every other signature remains common.
                key = f"startup-{side}" if workload == "startup" else workload
                if key not in references:
                    references[key] = signature
                need(signature == references[key], f"outputs differ across compilers/trials: {key}")
                sample["valid"] = True
                report["active_invocation"] = None
                receipt.save()
            outputs.check()
    report["output_signatures"] = references
    report["cross_version_ir_equal"] = True
    report["summaries"] = comparison.timing_summaries(report["samples"], args.trials)
    frozen.check()
    outputs.check()
    report.update(frozen_inputs_and_producers=frozen.records(), raw_files=outputs.records(),
                  all_recorded_hashes_unchanged=True)


def allocations(args, report, receipt, comparison):
    frozen, index = inputs(args, report)
    acceptance_path = args.snapshot_root / "evidence/symbolic-body-20261011/acceptance.json"
    frozen.add(acceptance_path, index[acceptance_path.relative_to(args.snapshot_root).as_posix()])
    accepted = common.read_json(acceptance_path, frozen)
    need(accepted["status"] == "passed" and accepted["source_commit"] == common.SYMBOLIC_COMMIT
         and accepted["snapshot"] == "symbolic-body-05"
         and accepted["compiler_sha256"] == common.SYMBOLIC_SHA256
         and accepted["compiler_version"] == "0.11.0" and accepted["language_subset"] == 11,
         "symbolic-body prerequisite identity differs")
    copied = {item["path"]: item["sha256"] for item in accepted["copied_files"]}
    frozen.add(args.baseline_report, copied["allocations/allocations.json"])
    symbolic_manifest = args.symbolic_snapshot / ".snapshot-manifest.json"
    frozen.add(symbolic_manifest, copied["snapshot-manifest.json"])
    symbolic = common.read_json(symbolic_manifest, frozen)
    need(symbolic["name"] == "symbolic-body-05"
         and symbolic["source_commit"] == accepted["snapshot_base_commit"],
         "symbolic-body captured source differs")
    report["runs"], normalized = {}, {}
    for side, path in (("a", args.baseline_report), ("b", args.candidate_report)):
        report["active_validation"] = {"side": side, "report": str(path)}
        receipt.save()
        # This validates every preparation, raw output, allocation counter,
        # expected answer, harness hash, and recomputed sample distribution.
        run, diagnostics = comparison.validate_allocation_run(path, frozen)
        normalized[side] = diagnostics
        report["runs"][side] = {"origin": frozen.add(path), "report": run}
        receipt.save()
    a, b = (report["runs"][side]["report"] for side in ("a", "b"))
    need(a["compiler"]["sha256"] == common.SYMBOLIC_SHA256
         and Path(a["compiler"]["path"]) == args.symbolic_snapshot / "target/release/axeon"
         and a["compiler"]["version_output"] == common.VERSIONS["a"],
         "allocation baseline is not the accepted symbolic-body compiler")
    need(b["compiler"]["sha256"] == args.candidate_sha256
         and Path(b["compiler"]["path"]) == args.snapshot_root / "target/release/axeon"
         and b["compiler"]["version_output"] == common.VERSIONS["b"],
         "candidate allocation compiler identity differs")
    for side, run, root in (("a", a, args.symbolic_snapshot), ("b", b, args.snapshot_root)):
        need(Path(run["profiler"]["path"]) == root / "tools/allocation-profiler/target/release/axeon-allocation-profiler",
             f"allocation profiler does not belong to designated snapshot: {side}")
    for key in ("inputs", "configuration", "expected_results", "scope"):
        need(a[key] == b[key], f"allocation {key} differ")
    for key in ("harness", "shared_harness"):
        need(a[key]["sha256"] == b[key]["sha256"], "allocation harness hashes differ")
    comparison.require_stable_hosts([a["host_before"], a["host_after"], b["host_before"], b["host_after"]], "allocation")
    need(normalized["a"] == normalized["b"], "allocation diagnostic code/span/message differs")
    report["scope"] = {
        "comparison": "Accepted symbolic-body-05 prerequisite to exact subset-12 candidate; declared compiler versions differ. Identical workloads, configuration, expected artifacts and harnesses are mandatory.",
        "measurement": "Requested-size allocation counts/bytes only; no instrumented timing or RSS comparison.",
        "pairing": "Independent allocation runs with identical workloads; not randomized timing pairs.",
        "diagnostics": "Raw hashes retained; only exact declared absolute source prefix replaced with format-budget.ax for comparison.",
        "limits": "Not physical heap, allocation sites, native/device memory, a leak verdict or proof of speedup. Peaks include entry baselines.",
    }
    report["differences"] = comparison.allocation_differences(a["summaries"], b["summaries"])
    report["baseline_acceptance"] = {"source_commit": accepted["source_commit"],
                                     "snapshot": accepted["snapshot"], "compiler_sha256": common.SYMBOLIC_SHA256}
    frozen.check()
    report.update(active_validation=None, frozen_inputs_and_producers=frozen.records(),
                  all_recorded_hashes_unchanged=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="mode", required=True)
    timing_parser = subparsers.add_parser("timing")
    timing_parser.add_argument("--trials", type=int, default=7)
    allocation_parser = subparsers.add_parser("allocations")
    allocation_parser.add_argument("--baseline-report", type=Path,
                                   default=Path("/home/Leo/.local/state/axeon/module-validation/symbolic-body-05/allocations/allocations.json"))
    allocation_parser.add_argument("--symbolic-snapshot", type=Path,
                                   default=Path("/home/Leo/.cache/axeon/build-snapshots/symbolic-body-05"))
    allocation_parser.add_argument("--candidate-report", required=True, type=Path)
    for subparser in (timing_parser, allocation_parser):
        common.common_arguments(subparser)
    args = parser.parse_args()
    if args.mode == "timing":
        need(1 <= args.trials <= 100, "timing trials must be in 1..100")
    else:
        for key in ("baseline_report", "candidate_report", "symbolic_snapshot"):
            setattr(args, key, getattr(args, key).expanduser().absolute())
    common.prepare(args)
    comparison = common.load("generic_profile_comparison", args.snapshot_root / "scripts/compare-compiler-profiles.py")
    report = {"schema_version": 1, "mode": "explicit-subset11-to-subset12-" + args.mode,
              "status": "running", "captured_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "host_before": comparison.shared.host_configuration(), "preparation": [], "samples": [], "summaries": {}}
    receipt = comparison.Receipt(args.output_dir, report)
    receipt.save()
    try:
        (timing if args.mode == "timing" else allocations)(args, report, receipt, comparison)
        report["host_after"] = comparison.shared.host_configuration()
        comparison.require_stable_hosts([report["host_before"], report["host_after"]], "transition helper")
        report["status"] = "passed"
        code = 0
    except BaseException as error:
        report.update(status="interrupted" if isinstance(error, KeyboardInterrupt) else "failed",
                      failure=f"{type(error).__name__}: {error}")
        code = 130 if isinstance(error, KeyboardInterrupt) else 1
    receipt.save()
    print(json.dumps({"status": report["status"], "report": str(args.output_dir / "comparison.json"),
                      "failure": report.get("failure")}))
    return code


if __name__ == "__main__":
    raise SystemExit(main())

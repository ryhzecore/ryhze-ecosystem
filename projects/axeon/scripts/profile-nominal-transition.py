#!/usr/bin/env python3
"""Bounded subset-12 to subset-13 measurements; never builds or installs tools.

Timing delegates unchanged workloads to profile-generic-transition.py, adapting
only designated identities. Allocations compare the qualified identity
prerequisite, not the delivered package. The new nominal example is a separate
candidate characterization, with no unsupported baseline or speedup ratio.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import types

sys.dont_write_bytecode = True
HERE = Path(__file__).absolute().parent
HELPERS = {
    "compare-generic-transition.py": "5f9049a09cabbe0a5b881fc6959d16f497f69c77528821f4edbedd24aa36fd37",
    "profile-generic-transition.py": "c6aea024a038a9bc856bc7df149678128d594df2c60f11cf1b2eb0c1e72fbde7",
    "compare-compiler-profiles.py": "2ba7455377b7c7d266525560fc123319ec7d5b448a1d9d79c20240c2d88a8d74",
    "profile-compiler.py": "7a06ea5082f4399e05f4cce53973e486d7ffa380bda0edf42868302151fb2aad",
    "profile-allocations.py": "37312aa92f17e0d03d4d2912c0583491b54dece0eedd416a5bc8a9cb17c61cfa",
}
PACKAGE_COMMIT = "0b4b1eacc701b769fa6e7fb2a052e9ae918e36fe"
PACKAGE_COMPILER = "7e9f30b5133fcf965a2460f1f9b44aa132cbf1a87b7d2dd820a5965c9af2bd14"
IDENTITY_COMPILER = "8b412903ece70f08e0617b7de584d7e463daf52f908dbe03f3668b6e5c6a6870"
IDENTITY_PROFILER = "02a48e247930df6e4df1dcc1d64b13d2d6459f4b4fb9bea978f5c1d5d4d0a32e"
IDENTITY_ACCEPTANCE = "bbad8c5cea4771f88439e10005e74d8094887ae871653ab41e57e8b34baf6580"
IDENTITY_REPORT = "63202fc190bab61da6431391d36cd2ac12f7f8128034a6370098d372ad95d380"
VERSIONS = {side: f"axeon {version}; subset {subset}; LLVM/Clang 19.1.7; hosted ABI 1 (single-file), 2 (modules)\n"
            for side, version, subset in (("a", "0.12.0", 12), ("b", "0.13.0", 13))}
EXAMPLE_STDOUT = b"42\n-9223372036854775808\nresult: 0\n"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# Check the utility before importing it; its own dependency imports are stdlib.
if hashlib.sha256((HERE / "compare-generic-transition.py").read_bytes()).hexdigest() != HELPERS["compare-generic-transition.py"]:
    raise RuntimeError("frozen transition utility hash differs")
common = load("nominal_profile_common", HERE / "compare-generic-transition.py")
need = common.need


def inputs(args, report):
    frozen = common.Frozen()
    manifest, index = common.verify_candidate(args.snapshot_root, args.candidate_sha256, frozen)
    need(Path(__file__).absolute() == args.snapshot_root / "scripts/profile-nominal-transition.py",
         "run the wrapper from its designated immutable candidate snapshot")
    frozen.add(Path(__file__), index["scripts/profile-nominal-transition.py"])
    for name, expected in HELPERS.items():
        need(index["scripts/" + name] == expected, f"frozen helper manifest differs: {name}")
        frozen.add(args.snapshot_root / "scripts" / name, expected)
    # Hash every declared source/build input, excluding retained historical evidence.
    for name, expected in index.items():
        if name.startswith(("src/", "tools/allocation-profiler/")) or name in (
                "Cargo.toml", "Cargo.lock", "build.rs", "rust-toolchain.toml", "toolchain-lock.json"):
            frozen.add(args.snapshot_root / name, expected)
    report["candidate"] = {"snapshot": str(args.snapshot_root), "capture_commit": manifest["source_commit"],
                           "capture_status": manifest["status"], "compiler_sha256": args.candidate_sha256}
    report["frozen_inputs_and_producers"] = frozen.records()
    return frozen, index


def verify_package(package, frozen):
    accepted = common.read_json(package / "package-receipt.json", frozen)
    need(accepted["status"] == "passed" and accepted["source_commit"] == PACKAGE_COMMIT
         and accepted["compiler_sha256"] == PACKAGE_COMPILER
         and accepted["compiler_version"] == "0.12.0" and accepted["language_subset"] == 12
         and accepted["version_output"] == VERSIONS["a"]
         and accepted["matches_both_independent_builds"] is True, "accepted package identity differs")
    reproduction = common.read_json(package / "reproduction.json", frozen)
    need(common.fingerprint(package / "reproduction.json")["sha256"] == accepted["reproduction_sha256"]
         and reproduction["source_commit"] == PACKAGE_COMMIT and reproduction["exit_code"] == 0
         and reproduction["independent_builds"] == 2 and reproduction["shared_target_directory"] is False
         and [v["sha256"]["target/release/axeon"] for v in reproduction["builds"]] == [PACKAGE_COMPILER] * 2,
         "accepted independent reproduction differs")
    frozen.add(package / "bin/axeon", PACKAGE_COMPILER)
    need(os.access(package / "bin/axeon", os.X_OK), "accepted compiler is not executable")
    return accepted


def timing(args, report, receipt, comparison):
    predecessor = common.load("frozen_nominal_timing_delegate", HERE / "profile-generic-transition.py")
    # Private module namespace adaptation; no accepted helper file is changed.
    predecessor.common = types.SimpleNamespace(**{**vars(common), "VERSIONS": VERSIONS,
                                                  "verify_package": verify_package})
    predecessor.inputs = inputs
    report["delegation"] = {"helper": "profile-generic-transition.py", "changes":
                            "Identity validation only; identical four workloads, preparation, alternating order, output equality and statistics."}
    predecessor.timing(args, report, receipt, comparison)
    report["scope"]["comparison"] = (
        "Whole delivered checkpoint0.13 compiler0.12.0/subset12 to compiler0.13.0/subset13 candidate transition. "
        "Features, sources and build paths differ; no nominal-only or causal optimization claim.")
    report["scope"]["compile"] = "Check and LLVM emission only; excludes native linking, native execution and device work."


def allocations(args, report, receipt, comparison):
    frozen, index = inputs(args, report)
    accepted_path = args.snapshot_root / "evidence/nominal-identities/validation.json"
    need(index[accepted_path.relative_to(args.snapshot_root).as_posix()] == IDENTITY_ACCEPTANCE,
         "identity acceptance manifest differs")
    frozen.add(accepted_path, IDENTITY_ACCEPTANCE)
    accepted = common.read_json(accepted_path, frozen)
    need(accepted["status"] == "accepted_internal_prerequisite"
         and accepted["source_commit"] == "f22a2a2617bc53a2b060df8e4144a29de6459ae6"
         and accepted["snapshot"] == "nominal-identities-03"
         and accepted["compiler_sha256"] == IDENTITY_COMPILER
         and accepted["profiler_sha256"] == IDENTITY_PROFILER
         and accepted["compiler_version"] == "0.12.0" and accepted["language_subset"] == 12,
         "qualified allocation prerequisite identity differs")
    frozen.add(args.identity_snapshot / ".snapshot-manifest.json", accepted["snapshot_manifest_sha256"])
    frozen.add(args.baseline_report, IDENTITY_REPORT)
    report["runs"], normalized = {}, {}
    for side, path in (("a", args.baseline_report), ("b", args.candidate_report)):
        report["active_validation"] = {"side": side, "report": str(path)}
        receipt.save()
        run, diagnostics = comparison.validate_allocation_run(path, frozen)
        normalized[side] = diagnostics
        report["runs"][side] = {"origin": frozen.add(path), "report": run}
        receipt.save()
    a, b = (report["runs"][side]["report"] for side in ("a", "b"))
    for side, run, root, compiler, profiler in (
            ("a", a, args.identity_snapshot, IDENTITY_COMPILER, IDENTITY_PROFILER),
            ("b", b, args.snapshot_root, args.candidate_sha256, args.candidate_profiler_sha256)):
        need(run["compiler"]["sha256"] == compiler
             and Path(run["compiler"]["path"]) == root / "target/release/axeon"
             and run["compiler"]["version_output"] == VERSIONS[side], f"allocation compiler differs: {side}")
        need(run["profiler"]["sha256"] == profiler and Path(run["profiler"]["path"]) == root /
             "tools/allocation-profiler/target/release/axeon-allocation-profiler", f"allocation profiler differs: {side}")
        need(run["configuration"]["trials"] == 7 and run["configuration"]["warmups"] == 1,
             "allocation requires one warmup and seven measured samples")
    for key in ("inputs", "configuration", "expected_results", "scope"):
        need(a[key] == b[key], f"allocation {key} differ")
    for key in ("harness", "shared_harness"):
        need(a[key]["sha256"] == b[key]["sha256"], "allocation harness hashes differ")
    comparison.require_stable_hosts([a["host_before"], a["host_after"], b["host_before"], b["host_after"]], "allocation")
    need(normalized["a"] == normalized["b"], "allocation diagnostic code/span/message differs")
    report["baseline_acceptance"] = {"source_commit": accepted["source_commit"], "snapshot": accepted["snapshot"],
                                     "compiler_sha256": IDENTITY_COMPILER, "profiler_sha256": IDENTITY_PROFILER}
    report["scope"] = {
        "comparison": "Qualified nominal-identities-03 internal prerequisite to subset13 candidate; NOT the delivered checkpoint0.13 binary used in timing. Identical old workloads/settings/artifacts/harnesses required.",
        "measurement": "Requested-size allocation count/byte distributions, including interpreter execution phases; no instrumented timing or RSS comparison.",
        "pairing": "Independent runs; not randomized timing pairs. Raw samples, R7 quartiles and population variance retained.",
        "diagnostics": "Original hashes retained; only exact absolute source prefix normalized to format-budget.ax.",
        "limits": "Not physical heap, allocation sites, native/device memory, leak verdict or proof of speedup. Peaks include entry baselines; verify-ir repeats verification and phases must not be summed.",
    }
    report["differences"] = comparison.allocation_differences(a["summaries"], b["summaries"])
    frozen.check()
    report.update(active_validation=None, frozen_inputs_and_producers=frozen.records(), all_recorded_hashes_unchanged=True)


def example(args, report, receipt, comparison):
    frozen, index = inputs(args, report)
    shared, alloc = comparison.shared, comparison.allocation
    outputs = common.Frozen()
    compiler = args.snapshot_root / "target/release/axeon"
    profiler = args.snapshot_root / "tools/allocation-profiler/target/release/axeon-allocation-profiler"
    report["profiler"] = frozen.add(profiler, args.candidate_profiler_sha256)
    source = args.snapshot_root / "examples/generic-nominals/main.ax"
    sources = [source.with_name(name + ".ax") for name in ("main", "packets", "tests")]
    need(set(source.parent.glob("*.ax")) == set(sources), "nominal example source set differs")
    for path in sources:
        frozen.add(path, index[path.relative_to(args.snapshot_root).as_posix()])
    controls = {"LC_ALL": "C", "LANG": "C", "TZ": "UTC", "SOURCE_DATE_EPOCH": "0"}
    environment = {**os.environ, **controls}
    report.update(configuration={"warmups": 1, "trials": 7, "fuel": alloc.FUEL,
                                 "timeout_seconds": args.timeout_seconds, "controlled_environment": controls,
                                 "workload_order": ["check", "emit-ir", "interpret", "allocations"]},
                  scope="Separate candidate generic nominal module example only. Ordinary process check/LLVM-emission/reference-interpreter timing and instrumented requested allocations are separate. No baseline ratio, C++/Python comparison, native execution, native linking, device claim or full-language claim. One warmup and seven measured rounds; warm filesystem, process startup included, affinity recorded not pinned; RSS may include inherited Python floor; no thermal/load control. Allocation verify-ir repeats verification and phases must not be summed.",
                  expected_stdout=EXAMPLE_STDOUT.decode(), allocation_samples=[])

    def invoke(label, command, expected, phase, trial, workload, artifact=None):
        frozen.check()
        report["active_invocation"] = {"label": label, "phase": phase, "trial": trial}
        receipt.save()
        sample = shared.run_child(command, args.snapshot_root, args.output_dir / (label + ".stdout"),
                                  args.output_dir / (label + ".stderr"), args.timeout_seconds, environment)
        sample.update(phase=phase, trial=trial, workload=workload, valid=False)
        report["preparation" if phase == "preparation" else "samples"].append(sample)
        receipt.save()  # Preserve the raw attempt before validating exit or output.
        for stream in ("stdout", "stderr"):
            outputs.add(sample[stream]["path"], sample[stream])
        shared.require_result(sample, 0, hashlib.sha256(expected).hexdigest(), stderr_empty=True)
        if artifact is not None:
            sample["artifact"] = outputs.add(artifact)
            need(sample["artifact"]["bytes"] > 0, "empty example LLVM output")
        frozen.check()
        sample["valid"] = True
        report["active_invocation"] = None
        receipt.save()
        return sample

    invoke("version", [compiler, "--version"], VERSIONS["b"].encode(), "preparation", 0, "identity")
    invoke("oracle", [compiler, "interpret", source, "--fuel", str(alloc.FUEL)], EXAMPLE_STDOUT,
           "preparation", 0, "correctness")
    reference = args.output_dir / "reference.ll"
    emitted = invoke("emit-reference", [compiler, "emit-ir", source, "-o", reference], b"",
                     "preparation", 0, "emit-ir", reference)
    expectation = {"mode": "project", "outcome": "success", "exit": 0, "codes": [], "result": 0,
                   "sources": {p.name: p.stat().st_size for p in sources if p.name != "tests.ax"},
                   "artifacts": {"emitted.ll": {"sha256": emitted["artifact"]["sha256"]},
                                 "stdout.txt": {"sha256": hashlib.sha256(EXAMPLE_STDOUT.rsplit(b"result: ", 1)[0]).hexdigest()}}}
    report["allocation_expectation"] = expectation
    for trial in range(8):
        phase = "warmup" if trial == 0 else "measured"
        for workload in ("check", "emit-ir", "interpret"):
            artifact = args.output_dir / f"emit-{trial}.ll" if workload == "emit-ir" else None
            command = [compiler, workload, source]
            if artifact is not None:
                command += ["-o", artifact]
            if workload == "interpret":
                command += ["--fuel", str(alloc.FUEL)]
            sample = invoke(f"{workload}-{trial}", command, EXAMPLE_STDOUT if workload == "interpret" else b"",
                            phase, trial, workload, artifact)
            if artifact is not None:
                need(sample["artifact"]["sha256"] == emitted["artifact"]["sha256"], "example LLVM changed between trials")
        directory = args.output_dir / f"alloc-{trial}"
        process = invoke(f"alloc-{trial}", [profiler, "project", source, "--output-dir", directory,
                                          "--fuel", str(alloc.FUEL)], b"", phase, trial, "allocations")
        sample = {"trial": trial, "round_kind": phase, "directory": str(directory), "process": process, "valid": False}
        report["allocation_samples"].append(sample)
        receipt.save()
        observed, files = alloc.validate_result(directory, process, expectation)
        sample.update(allocation_report=observed, valid=True)
        for path, metadata in files.items():
            outputs.add(path, metadata)
        outputs.check()
        receipt.save()
    report["summaries"] = {workload: {metric: shared.summarize([
        s[metric] for s in report["samples"] if s["phase"] == "measured" and s["workload"] == workload])
        for metric in comparison.METRICS} for workload in ("check", "emit-ir", "interpret")}
    measured = [s["allocation_report"] for s in report["allocation_samples"] if s["round_kind"] == "measured"]
    report["allocation_summaries"] = {}
    for phase in measured[0]["phases"]:
        phases = [next(p for p in sample["phases"] if p["name"] == phase["name"]) for sample in measured]
        report["allocation_summaries"][phase["name"]] = {
            **{metric: shared.summarize([p[metric] for p in phases]) for metric in alloc.STATE},
            "events": {metric: shared.summarize([p["events"][metric] for p in phases]) for metric in alloc.EVENTS},
            "net_live_requested_bytes_raw": [p["net_live_requested_bytes"] for p in phases]}
    frozen.check()
    outputs.check()
    report.update(frozen_inputs_and_producers=frozen.records(), raw_files=outputs.records(), all_recorded_hashes_unchanged=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    for name in ("timing", "allocations", "example"):
        subparser = modes.add_parser(name)
        common.common_arguments(subparser)
        subparser.set_defaults(accepted_package=Path("/home/Leo/.local/share/axeon/artifacts/axeon-cpu-0.13"), trials=7)
        if name in ("allocations", "example"):
            subparser.add_argument("--candidate-profiler-sha256", required=True)
        if name == "allocations":
            subparser.add_argument("--identity-snapshot", type=Path,
                                   default=Path("/home/Leo/.cache/axeon/build-snapshots/nominal-identities-03"))
            subparser.add_argument("--baseline-report", type=Path, default=Path(
                "/home/Leo/.local/state/axeon/module-validation/nominal-identities-03/allocations/allocations.json"))
            subparser.add_argument("--candidate-report", required=True, type=Path)
    args = parser.parse_args()
    if args.mode in ("allocations", "example"):
        need(len(args.candidate_profiler_sha256) == 64 and all(c in "0123456789abcdef" for c in args.candidate_profiler_sha256),
             "candidate profiler hash must be lowercase SHA256")
    if args.mode == "allocations":
        for key in ("baseline_report", "candidate_report", "identity_snapshot"):
            setattr(args, key, getattr(args, key).expanduser().absolute())
    common.prepare(args)
    # Validate all frozen executable Python dependencies before importing them.
    for name, expected in HELPERS.items():
        need(common.fingerprint(args.snapshot_root / "scripts" / name)["sha256"] == expected,
             f"frozen measurement helper differs: {name}")
    comparison = load("nominal_profile_comparison", args.snapshot_root / "scripts/compare-compiler-profiles.py")
    report = {"schema_version": 1, "mode": "explicit-subset12-to-subset13-" + args.mode,
              "status": "running", "captured_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "host_before": comparison.shared.host_configuration(), "preparation": [], "samples": [], "summaries": {}}
    receipt = comparison.Receipt(args.output_dir, report)
    receipt.save()
    try:
        {"timing": timing, "allocations": allocations, "example": example}[args.mode](args, report, receipt, comparison)
        report["host_after"] = comparison.shared.host_configuration()
        comparison.require_stable_hosts([report["host_before"], report["host_after"]], "transition wrapper")
        report["status"], code = "passed", 0
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

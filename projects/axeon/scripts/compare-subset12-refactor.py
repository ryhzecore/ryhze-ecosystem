#!/usr/bin/env python3
"""Exact subset-12 behavior/LLVM comparison against accepted local delivery 0.13.

Uses all accepted example sources, including generic modules, without building.
The older transition helper remains frozen and keeps its subset-11 contract.
"""
import argparse
import datetime
import importlib.util
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
HELPER = Path(__file__).with_name("compare-generic-transition.py")
spec = importlib.util.spec_from_file_location("transition_utilities", HELPER)
util = importlib.util.module_from_spec(spec)
spec.loader.exec_module(util)
need = util.need
COMMIT = "0b4b1eacc701b769fa6e7fb2a052e9ae918e36fe"
COMPILER = "7e9f30b5133fcf965a2460f1f9b44aa132cbf1a87b7d2dd820a5965c9af2bd14"
VERSION = util.VERSIONS["b"]
NAMES = util.OLD_NAMES | {"generic-modules/main"}


def compare(args, report, receipt, runner):
    root, out = args.snapshot_root, args.output_dir
    frozen, outputs = util.Frozen(), util.Frozen()
    frozen.add(Path(__file__))
    frozen.add(HELPER, "5f9049a09cabbe0a5b881fc6959d16f497f69c77528821f4edbedd24aa36fd37")
    manifest, index = util.verify_candidate(root, args.candidate_sha256, frozen)
    baseline = args.accepted_snapshot.absolute()
    old_manifest = util.read_json(baseline / ".snapshot-manifest.json", frozen)
    need(old_manifest["source_commit"] == COMMIT and old_manifest["status"] == [],
         "baseline is not the accepted committed delivery snapshot")
    old_index = {v["path"]: v["sha256"] for v in old_manifest["files"]}
    need(len(old_index) == len(old_manifest["files"]), "duplicate accepted source paths")
    package = args.accepted_package
    accepted = util.read_json(package / "package-receipt.json", frozen)
    need(accepted["status"] == "passed" and accepted["source_commit"] == COMMIT
         and accepted["compiler_sha256"] == COMPILER
         and accepted["compiler_version"] == "0.12.0" and accepted["language_subset"] == 12
         and accepted["version_output"] == VERSION
         and accepted["matches_both_independent_builds"] is True,
         "accepted package identity differs")
    reproduction = util.read_json(package / "reproduction.json", frozen)
    need(util.fingerprint(package / "reproduction.json")["sha256"] == accepted["reproduction_sha256"]
         and reproduction["source_commit"] == COMMIT and reproduction["exit_code"] == 0
         and reproduction["independent_builds"] == 2
         and reproduction["shared_target_directory"] is False
         and [v["sha256"]["target/release/axeon"] for v in reproduction["builds"]] == [COMPILER] * 2,
         "accepted independent reproduction differs")
    example_runner = "scripts/record-native-examples.py"
    frozen.add(baseline / example_runner, old_index[example_runner])
    need(index[example_runner] == old_index[example_runner], "accepted example runner changed")
    old = util.load("accepted_subset12_examples", baseline / example_runner)
    candidate = util.load("candidate_subset12_examples", root / example_runner)
    need(set(old.EXPECTED) == NAMES and len(NAMES) == 18
         and candidate.EXPECTED == old.EXPECTED
         and candidate.EXPECTED_REJECTION == old.EXPECTED_REJECTION,
         "accepted example set, expectations or diagnostic changed")
    sources = {p for p in old_index if p.startswith("examples/") and p.endswith(".ax")}
    actual = {p.relative_to(root).as_posix() for p in (root / "examples").rglob("*.ax")}
    need(actual == sources, "accepted example source set changed")
    for path in sorted(sources):
        frozen.add(root / path, old_index[path])
        need(index[path] == old_index[path], "candidate source manifest differs")
    compilers = {"a": package / "bin/axeon", "b": root / "target/release/axeon"}
    frozen.add(compilers["a"], COMPILER)
    report.update(
        provenance={"accepted_source_commit": COMMIT, "accepted_manifest": str(baseline / ".snapshot-manifest.json"),
                    "candidate_capture_commit": manifest["source_commit"], "candidate_capture_status": manifest["status"]},
        compilers={side: frozen.add(path) for side, path in compilers.items()},
        accepted_example_sources=sorted(sources))

    def invoke(side, label, arguments, expected):
        frozen.check()
        sample = runner.shared.run_child(
            [compilers[side], *arguments], root, out / f"{label}-{side}.stdout",
            out / f"{label}-{side}.stderr", args.timeout_seconds)
        sample.update(side=side, label=label, valid=False)
        report["samples"].append(sample)
        receipt.save()
        need(not sample["timed_out"] and sample["returncode"] == expected[0], f"unexpected exit: {label}/{side}")
        for stream, wanted in zip(("stdout", "stderr"), expected[1:]):
            outputs.add(sample[stream]["path"], sample[stream])
            if wanted is not None:
                need(Path(sample[stream]["path"]).read_bytes() == wanted.encode(), f"exact {stream} differs: {label}/{side}")
        sample["valid"] = True
        return sample

    for side in compilers:
        invoke(side, "version", ["--version"], (0, VERSION, ""))
        report["compilers"][side]["version_output"] = VERSION
    for name, expected in old.EXPECTED.items():
        label, hashes = name.replace("/", "-"), []
        for side in compilers:
            invoke(side, label + "-interpret", ["interpret", f"examples/{name}.ax"], expected)
            target = out / f"{label}-{side}.ll"
            invoke(side, label + "-emit", ["emit-ir", f"examples/{name}.ax", "-o", str(target)], (0, "", ""))
            hashes.append(outputs.add(target)["sha256"])
        need(hashes[0] == hashes[1], f"accepted LLVM differs: {name}")
        report["comparisons"].append({"source": f"examples/{name}.ax", "llvm_sha256": hashes[0],
                                      "exact_execution_and_ir_match": True})
        receipt.save()
    diagnostics = []
    for side in compilers:
        sample = invoke(side, "ownership-error", ["check", "examples/ownership-error.ax", "--diagnostic-format", "json"], (1, "", None))
        raw = Path(sample["stderr"]["path"]).read_bytes()
        need(json.loads(raw) == old.EXPECTED_REJECTION, "accepted ownership diagnostic changed")
        diagnostics.append(raw)
    need(diagnostics[0] == diagnostics[1], "ownership diagnostic bytes differ")
    frozen.check()
    outputs.check()
    report.update(example_count=18, negative_diagnostics_equal=True,
                  frozen_inputs=frozen.records(), artifacts_and_outputs=outputs.records(),
                  all_input_and_output_hashes_rechecked=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    util.common_arguments(parser)
    parser.set_defaults(accepted_package=Path("/home/Leo/.local/share/axeon/artifacts/axeon-cpu-0.13"))
    parser.add_argument("--accepted-snapshot", type=Path,
                        default=Path("/home/Leo/.cache/axeon/build-snapshots/workspace-generics-delivery-03"))
    args = parser.parse_args()
    util.prepare(args)
    runner = util.load("subset12_behavior_runner", args.snapshot_root / "scripts/compare-compiler-profiles.py")
    report = {"schema_version": 1, "mode": "same-subset12-refactor", "status": "running",
              "captured_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "samples": [], "comparisons": [],
              "scope": "All 18 accepted examples: exact source, interpreter output, diagnostics and LLVM. No new language feature or performance claim."}
    receipt = runner.Receipt(args.output_dir, report)
    receipt.save()
    try:
        compare(args, report, receipt, runner)
        report["status"], code = "passed", 0
    except BaseException as error:
        report.update(status="interrupted" if isinstance(error, KeyboardInterrupt) else "failed", failure=f"{type(error).__name__}: {error}")
        code = 130 if isinstance(error, KeyboardInterrupt) else 1
    receipt.save()
    print(json.dumps({"status": report["status"], "report": str(args.output_dir / "comparison.json"), "failure": report.get("failure")}))
    return code


if __name__ == "__main__":
    raise SystemExit(main())

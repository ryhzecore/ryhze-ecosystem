#!/usr/bin/env python3
"""Exact accepted subset-11 compatibility plus separate subset-12 example checks.

Run on Linux against a frozen ext4 snapshot; this helper never builds anything.
The accepted workload comes from its accepted manifest, not a filtered new list.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import stat
import sys

sys.dont_write_bytecode = True
ACCEPTED_COMMIT = "4060b01bbaa81cab246f0379fe5df9b4a353f3f0"
ACCEPTED_SHA256 = "6ddf11d0c52ae770342d5fd77d7cdfc8953ff597f82349dca217eda74a9fcee9"
SYMBOLIC_COMMIT = "c6b2ce90931e9f2f60dfda2c82adddb5f2b6f2e7"
SYMBOLIC_SHA256 = "0ddf0d2cb8e9db9e8fb06de1fffa55d83decda368a2cd29ed9121babfc68ba1f"
VERSIONS = {
    side: f"axeon {version}; subset {subset}; LLVM/Clang 19.1.7; hosted ABI 1 (single-file), 2 (modules)\n"
    for side, version, subset in (("a", "0.11.0", 11), ("b", "0.12.0", 12))
}
OLD_NAMES = {
    "fibonacci", "effects", "control", "overflow", "arrays", "numerics", "bounds",
    "tensors", "tensors-recovery", "tensors-trap", "modules/main", "records", "unions",
    "aggregate-modules/main", "trait-modules/main", "unit-modules/main", "nested-modules/main",
}
NEW_FILES = {f"examples/generic-modules/{name}.ax" for name in
             ("contracts", "main", "pipeline", "tests", "types")}
GENERIC_OUTPUT = (0, "42\n7\n9\nresult: 0\n", "")


def need(condition, message):
    if not condition:
        raise RuntimeError(message)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fingerprint(path):
    path = Path(path)
    metadata = path.lstat()
    need(stat.S_ISREG(metadata.st_mode) and metadata.st_size <= 128 * 1024**2,
         f"bounded regular non-symlink file required: {path}")
    with path.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    return {"bytes": metadata.st_size, "sha256": digest}


class Frozen:
    def __init__(self):
        self.files = {}

    def add(self, path, expected=None):
        path = Path(path).absolute()
        actual = fingerprint(path)
        wanted = self.files.get(path, actual) if expected is None else expected
        if isinstance(wanted, str):
            need(actual["sha256"] == wanted, f"file hash differs: {path}")
        else:
            need(actual == {key: wanted[key] for key in ("bytes", "sha256")},
                 f"file changed: {path}")
        if path in self.files:
            need(actual == self.files[path], f"frozen file changed: {path}")
        self.files[path] = actual
        return {"path": str(path), **actual}

    def check(self):
        for path, expected in self.files.items():
            need(fingerprint(path) == expected, f"frozen file changed: {path}")

    def records(self):
        return [{"path": str(path), **value} for path, value in sorted(self.files.items())]


def read_json(path, frozen):
    frozen.add(path)
    need(Path(path).stat().st_size <= 16 * 1024**2, f"JSON exceeds bound: {path}")

    def unique(pairs):
        result = {}
        for key, value in pairs:
            need(key not in result, f"duplicate JSON key in {path}: {key}")
            result[key] = value
        return result

    return json.loads(Path(path).read_text(encoding="utf-8"), object_pairs_hook=unique,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))


def verify_candidate(root, expected_hash, frozen):
    need(root.is_dir() and not root.is_symlink() and not root.is_relative_to(Path("/mnt")),
         "candidate must be an existing ext4 snapshot outside /mnt")
    manifest = read_json(root / ".snapshot-manifest.json", frozen)
    need(manifest["name"] == root.name, "snapshot name differs from its directory")
    index = {entry["path"]: entry["sha256"] for entry in manifest["files"]}
    need(len(index) == len(manifest["files"]), "duplicate source manifest paths")
    for relative in ("Cargo.toml", "Cargo.lock", "rust-toolchain.toml", "toolchain-lock.json",
                     "scripts/profile-compiler.py", "scripts/profile-allocations.py",
                     "scripts/compare-compiler-profiles.py", "scripts/record-native-examples.py"):
        frozen.add(root / relative, index[relative])
    compiler = root / "target/release/axeon"
    frozen.add(compiler, expected_hash)
    need(os.access(compiler, os.X_OK), "candidate compiler is not executable")
    return manifest, index


def verify_package(package, frozen):
    receipt = read_json(package / "package-receipt.json", frozen)
    need(receipt["status"] == "passed" and receipt["source_commit"] == ACCEPTED_COMMIT,
         "baseline package is not the designated accepted source")
    need(receipt["compiler_sha256"] == ACCEPTED_SHA256
         and receipt["compiler_version"] == "0.11.0" and receipt["language_subset"] == 11
         and receipt["version_output"] == VERSIONS["a"]
         and receipt["matches_both_independent_builds"] is True,
         "baseline package identity differs")
    compiler = package / "bin/axeon"
    frozen.add(compiler, ACCEPTED_SHA256)
    need(os.access(compiler, os.X_OK), "accepted compiler is not executable")
    reproduction = read_json(package / "reproduction.json", frozen)
    need(fingerprint(package / "reproduction.json")["sha256"] == receipt["reproduction_sha256"]
         and reproduction["source_commit"] == ACCEPTED_COMMIT and reproduction["exit_code"] == 0
         and reproduction["independent_builds"] == 2
         and reproduction["shared_target_directory"] is False
         and [build["sha256"]["target/release/axeon"] for build in reproduction["builds"]]
         == [ACCEPTED_SHA256] * 2, "accepted reproduction differs")
    return receipt


def common_arguments(parser):
    parser.add_argument("--snapshot-root", required=True, type=Path)
    parser.add_argument("--candidate-sha256", required=True)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--accepted-package", type=Path,
                        default=Path("/home/Leo/.local/share/axeon/artifacts/axeon-cpu-0.12"))
    parser.add_argument("--timeout-seconds", type=float, default=30)


def prepare(args):
    need(sys.platform == "linux" and hasattr(os, "wait4"), "Linux wait4 is required")
    need(re.fullmatch(r"[0-9a-f]{64}", args.candidate_sha256) is not None,
         "candidate SHA-256 must be explicitly designated")
    need(math.isfinite(args.timeout_seconds) and 0 < args.timeout_seconds <= 120,
         "child timeout must be finite and in (0, 120]")
    for key in ("snapshot_root", "output_dir", "accepted_package"):
        path = getattr(args, key).expanduser().absolute()
        need(not path.is_symlink(), f"symlink path is not allowed: {path}")
        setattr(args, key, path)
    need(not args.output_dir.is_relative_to(args.snapshot_root),
         "keep output receipts outside immutable candidate snapshot")
    args.output_dir.mkdir()  # Exclusive: preserve every earlier run and partial run.


def compare(args, report, receipt, comparison):
    root, out = args.snapshot_root, args.output_dir
    frozen, outputs = Frozen(), Frozen()
    frozen.add(Path(__file__))
    manifest, index = verify_candidate(root, args.candidate_sha256, frozen)
    accepted = verify_package(args.accepted_package, frozen)
    baseline = args.accepted_snapshot.expanduser().absolute()
    baseline_manifest = read_json(baseline / ".snapshot-manifest.json", frozen)
    need(baseline_manifest["source_commit"] == ACCEPTED_COMMIT and baseline_manifest["status"] == [],
         "old workload manifest is not the accepted clean source")
    old_index = {entry["path"]: entry["sha256"] for entry in baseline_manifest["files"]}
    need(len(old_index) == len(baseline_manifest["files"]), "duplicate accepted source paths")
    runner = "scripts/record-native-examples.py"
    frozen.add(baseline / runner, old_index[runner])
    old = load("accepted_generic_transition_examples", baseline / runner)
    candidate = load("candidate_generic_transition_examples", root / runner)
    need(set(old.EXPECTED) == OLD_NAMES and len(old.EXPECTED) == 17,
         "accepted workload must contain all 17 original examples")
    need(set(candidate.EXPECTED) == OLD_NAMES | {"generic-modules/main"},
         "candidate workload changed beyond its designated generic addition")
    need({name: candidate.EXPECTED[name] for name in OLD_NAMES} == old.EXPECTED
         and candidate.EXPECTED_REJECTION == old.EXPECTED_REJECTION,
         "accepted expected outputs or rejection were changed")
    need(candidate.EXPECTED["generic-modules/main"] == GENERIC_OUTPUT,
         "generic example expected output differs")
    old_sources = {path for path in old_index if path.startswith("examples/") and path.endswith(".ax")}
    actual_sources = {path.relative_to(root).as_posix() for path in (root / "examples").rglob("*.ax")}
    need(actual_sources == old_sources | NEW_FILES and not old_sources & NEW_FILES,
         "old example removed or unexpected source addition")
    for relative in sorted(old_sources):
        frozen.add(root / relative, old_index[relative])
        need(index[relative] == old_index[relative], "candidate manifest disagrees on old example bytes")
    for relative in sorted(NEW_FILES):
        frozen.add(root / relative, index[relative])
    compilers = {"a": args.accepted_package / "bin/axeon", "b": root / "target/release/axeon"}
    report.update(provenance={"accepted_source_commit": accepted["source_commit"],
                             "candidate_capture_commit": manifest["source_commit"],
                             "candidate_capture_status": manifest["status"],
                             "accepted_manifest": str(baseline / ".snapshot-manifest.json")},
                  compilers={side: frozen.add(path) for side, path in compilers.items()},
                  accepted_example_sources=sorted(old_sources), generic_sources=sorted(NEW_FILES))

    def invoke(side, label, arguments, expected):
        frozen.check()
        sample = comparison.shared.run_child(
            [compilers[side], *arguments], root, out / f"{label}-{side}.stdout",
            out / f"{label}-{side}.stderr", args.timeout_seconds)
        sample.update(side=side, label=label, valid=False)
        report["samples"].append(sample)
        receipt.save()
        need(not sample["timed_out"] and sample["returncode"] == expected[0],
             f"unexpected exit: {label}/{side}")
        for stream, wanted in zip(("stdout", "stderr"), expected[1:]):
            outputs.add(sample[stream]["path"], sample[stream])
            if wanted is not None:
                need(Path(sample[stream]["path"]).read_bytes() == wanted.encode(),
                     f"exact {stream} differs: {label}/{side}")
        sample["valid"] = True
        return sample

    for side in compilers:
        invoke(side, "version", ["--version"], (0, VERSIONS[side], ""))
        report["compilers"][side]["version_output"] = VERSIONS[side]
    for name, expected in old.EXPECTED.items():
        label, ir_hashes = name.replace("/", "-"), []
        for side in compilers:
            invoke(side, label + "-interpret", ["interpret", f"examples/{name}.ax"], expected)
            target = out / f"{label}-{side}.ll"
            invoke(side, label + "-emit", ["emit-ir", f"examples/{name}.ax", "-o", str(target)], (0, "", ""))
            ir_hashes.append(outputs.add(target)["sha256"])
        need(ir_hashes[0] == ir_hashes[1], f"LLVM changed for accepted example: {name}")
        report["comparisons"].append({"source": f"examples/{name}.ax", "llvm_sha256": ir_hashes[0],
                                      "exact_execution_and_ir_match": True})
        receipt.save()
    errors = []
    for side in compilers:
        sample = invoke(side, "ownership-error", ["check", "examples/ownership-error.ax",
                         "--diagnostic-format", "json"], (1, "", None))
        raw = Path(sample["stderr"]["path"]).read_bytes()
        need(json.loads(raw) == old.EXPECTED_REJECTION, "accepted ownership diagnostic changed")
        errors.append(raw)
    need(errors[0] == errors[1], "ownership diagnostic raw bytes differ")
    invoke("b", "generic-interpret", ["interpret", "examples/generic-modules/main.ax"], GENERIC_OUTPUT)
    generic_ir = out / "generic-candidate.ll"
    invoke("b", "generic-emit", ["emit-ir", "examples/generic-modules/main.ax", "-o", str(generic_ir)], (0, "", ""))
    generic_ir_record = outputs.add(generic_ir)
    need(generic_ir_record["bytes"] > 0, "generic example emitted empty LLVM")
    frozen.check()
    outputs.check()
    report.update(example_count=17, negative_diagnostics_equal=True,
                  generic_example={"expected": GENERIC_OUTPUT, "llvm": generic_ir_record,
                                   "scope": "Separate candidate interpreter and IR checks; native execution/wrappers are separate gates."},
                  frozen_inputs=frozen.records(), artifacts_and_outputs=outputs.records(),
                  all_input_and_output_hashes_rechecked=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    common_arguments(parser)
    parser.add_argument("--accepted-snapshot", type=Path,
                        default=Path("/home/Leo/.cache/axeon/build-snapshots/traits-delivery-01"))
    args = parser.parse_args()
    prepare(args)
    comparison = load("generic_behavior_comparison", args.snapshot_root / "scripts/compare-compiler-profiles.py")
    report = {"schema_version": 1, "mode": "explicit-subset11-to-subset12-compatibility",
              "status": "running", "captured_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "samples": [], "comparisons": [],
              "scope": "All 17 accepted examples retain their exact source hashes, outputs, diagnostics and LLVM. Generic example checked separately. No performance or full-generics acceptance."}
    receipt = comparison.Receipt(args.output_dir, report)
    receipt.save()
    try:
        compare(args, report, receipt, comparison)
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

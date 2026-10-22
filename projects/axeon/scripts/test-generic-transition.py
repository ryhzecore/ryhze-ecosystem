#!/usr/bin/env python3
"""Adversarial local fixtures; never execute a compiler, profiler, or subprocess."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


behavior = load("test_generic_behavior", HERE / "compare-generic-transition.py")
profile = load("test_generic_profile", HERE / "profile-generic-transition.py")
comparison = load("test_generic_comparison", HERE / "compare-compiler-profiles.py")
native = load("test_generic_native", HERE / "record-native-examples.py")
fixtures = load("test_generic_existing_fixtures", HERE / "test-compare-compiler-profiles.py")
IR = b'target triple = "x86_64-pc-linux-gnu"\ndefine i64 @main() { ret i64 0 }\n'
INPUT_FILES = ("Cargo.toml", "Cargo.lock", "rust-toolchain.toml", "toolchain-lock.json",
               "scripts/profile-compiler.py", "scripts/profile-allocations.py",
               "scripts/compare-compiler-profiles.py")


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return {"path": str(path), **behavior.fingerprint(path)}


def write_json(path, data):
    return write(path, (json.dumps(data, indent=2) + "\n").encode())


def example_module(path, expected, rejection):
    write(path, (f"EXPECTED = {expected!r}\nEXPECTED_REJECTION = {rejection!r}\n").encode())


class TransitionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="axeon-generic-transition-selftest-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.candidate = self.root / "candidate"
        self.baseline = self.root / "traits-delivery-01"
        self.package = self.root / "package"
        self.candidate.mkdir()
        self.baseline.mkdir()
        # Fail immediately if any path accidentally escapes the mocked child boundary.
        popen = mock.patch("subprocess.Popen", side_effect=AssertionError("selftests must not launch subprocesses"))
        popen.start()
        self.addCleanup(popen.stop)
        for relative in INPUT_FILES:
            target = self.candidate / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(HERE.parent / relative, target)
        # Keep the historical subset-11 -> subset-12 workload fixed when later
        # examples are added to the current runner.
        scope = behavior.OLD_NAMES | {"generic-modules/main"}
        self.expected = {name: copy.deepcopy(value) for name, value in native.EXPECTED.items()
                         if name in scope}
        self.assertEqual(set(self.expected), scope)
        self.old_expected = {name: value for name, value in self.expected.items()
                             if name != "generic-modules/main"}
        self.rejection = copy.deepcopy(native.EXPECTED_REJECTION)
        self.assertEqual(set(self.old_expected), behavior.OLD_NAMES)
        for source in (HERE.parent / "examples").rglob("*.ax"):
            relative = source.relative_to(HERE.parent)
            if relative.parts[:2] == ("examples", "generic-nominals"):
                continue
            write(self.candidate / relative, source.read_bytes())
            if relative.as_posix() not in behavior.NEW_FILES:
                write(self.baseline / relative, source.read_bytes())
        example_module(self.baseline / "scripts/record-native-examples.py", self.old_expected, self.rejection)
        self.write_candidate_examples()
        for path, content in ((self.candidate / "target/release/axeon", b"candidate fixture; never executed"),
                              (self.package / "bin/axeon", b"accepted fixture; never executed")):
            write(path, content)
            path.chmod(0o755)
        self.accepted_hash = behavior.fingerprint(self.package / "bin/axeon")["sha256"]
        self.candidate_hash = behavior.fingerprint(self.candidate / "target/release/axeon")["sha256"]
        for module in (behavior, profile.common):
            patch = mock.patch.object(module, "ACCEPTED_SHA256", self.accepted_hash)
            patch.start()
            self.addCleanup(patch.stop)
        reproduction = {"source_commit": behavior.ACCEPTED_COMMIT, "exit_code": 0,
                        "independent_builds": 2, "shared_target_directory": False,
                        "builds": [{"sha256": {"target/release/axeon": self.accepted_hash}} for _ in range(2)]}
        reproduced = write_json(self.package / "reproduction.json", reproduction)
        self.package_receipt = {"status": "passed", "source_commit": behavior.ACCEPTED_COMMIT,
                                "compiler_sha256": self.accepted_hash, "compiler_version": "0.11.0",
                                "language_subset": 11, "version_output": behavior.VERSIONS["a"],
                                "matches_both_independent_builds": True,
                                "reproduction_sha256": reproduced["sha256"]}
        write_json(self.package / "package-receipt.json", self.package_receipt)
        self.manifest(self.baseline, behavior.ACCEPTED_COMMIT)
        self.refresh_candidate()
        self.sequence = 0

    def manifest(self, root, commit):
        files = [{"path": path.relative_to(root).as_posix(), "sha256": behavior.fingerprint(path)["sha256"]}
                 for path in sorted(root.rglob("*")) if path.is_file()
                 and path.name != ".snapshot-manifest.json" and "target" not in path.relative_to(root).parts]
        write_json(root / ".snapshot-manifest.json",
                   {"name": root.name, "source_commit": commit, "status": [], "files": files})

    def refresh_candidate(self):
        self.manifest(self.candidate, "e" * 40)

    def write_candidate_examples(self):
        example_module(self.candidate / "scripts/record-native-examples.py", self.expected, self.rejection)

    def arguments(self):
        self.sequence += 1
        output = self.root / f"run-{self.sequence}"
        return argparse.Namespace(snapshot_root=self.candidate, candidate_sha256=self.candidate_hash,
                                  accepted_package=self.package, accepted_snapshot=self.baseline,
                                  output_dir=output, timeout_seconds=1, trials=3)

    def simulated_child(self, fault=None):
        def run(argv, cwd, stdout_path, stderr_path, timeout, environment=None):
            side = "b" if Path(argv[0]) == self.candidate / "target/release/axeon" else "a"
            operation = argv[1]
            stdout, stderr, code = b"", b"", 0
            if operation == "--version":
                version_side = "a" if fault == "identity" and side == "b" else side
                stdout = behavior.VERSIONS[version_side].encode()
            elif operation == "interpret":
                if argv[2] == "arithmetic.ax":
                    _, stdout = comparison.shared.generated_sources()
                else:
                    name = argv[2][len("examples/"):-len(".ax")]
                    code, out, err = native.EXPECTED[name]
                    stdout, stderr = out.encode(), err.encode()
            elif operation == "emit-ir":
                content = IR + (b"; changed fixture\n" if fault == "ir" and side == "b" else b"")
                write(Path(argv[argv.index("-o") + 1]), content)
            elif operation == "check" and "ownership-error.ax" in argv[2]:
                rejection = copy.deepcopy(native.EXPECTED_REJECTION)
                if fault == "diagnostic" and side == "b":
                    rejection["diagnostics"][0]["message"] += " altered"
                stderr, code = json.dumps(rejection).encode(), 1
            elif operation == "format":
                stderr, code = profile.FORMAT_ERROR, 1
                if fault == "format":
                    stderr = stderr.replace(b"300001:1", b"300002:1")
            return {"timed_out": False, "returncode": code,
                    "stdout": write(stdout_path, stdout), "stderr": write(stderr_path, stderr),
                    "wall_seconds": 0.002 if side == "b" else 0.003,
                    "user_cpu_seconds": 0.001, "system_cpu_seconds": 0.0, "max_rss_kib": 64}
        return run

    def run_behavior(self, fault=None):
        args = self.arguments()
        args.output_dir.mkdir()
        report = {"samples": [], "comparisons": []}
        receipt = comparison.Receipt(args.output_dir, report)
        with mock.patch.object(comparison.shared, "run_child", side_effect=self.simulated_child(fault)) as child:
            behavior.compare(args, report, receipt, comparison)
        return report, child.call_count

    def test_all_17_old_examples_are_compared_and_generic_is_separate(self):
        report, calls = self.run_behavior()
        self.assertEqual(calls, 74)
        self.assertEqual(report["example_count"], 17)
        self.assertEqual({entry["source"] for entry in report["comparisons"]},
                         {f"examples/{name}.ax" for name in self.old_expected})
        self.assertEqual(set(report["generic_sources"]), behavior.NEW_FILES)
        self.assertTrue(report["negative_diagnostics_equal"])
        self.assertTrue(report["all_input_and_output_hashes_rechecked"])

    def test_wrong_designated_candidate_hash_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "file hash differs"):
            behavior.verify_candidate(self.candidate, "0" * 64, behavior.Frozen())

    def test_wrong_accepted_package_version_is_rejected(self):
        self.package_receipt["compiler_version"] = "0.12.0"
        write_json(self.package / "package-receipt.json", self.package_receipt)
        with self.assertRaisesRegex(RuntimeError, "baseline package identity differs"):
            behavior.verify_package(self.package, behavior.Frozen())

    def test_missing_old_source_cannot_be_hidden_by_new_manifest(self):
        (self.candidate / "examples/fibonacci.ax").unlink()
        self.refresh_candidate()
        with self.assertRaisesRegex(RuntimeError, "old example removed"):
            self.run_behavior()

    def test_missing_old_expected_example_is_rejected(self):
        del self.expected["fibonacci"]
        self.write_candidate_examples()
        self.refresh_candidate()
        with self.assertRaisesRegex(RuntimeError, "candidate workload changed"):
            self.run_behavior()

    def test_changed_old_expected_output_cannot_redefine_the_oracle(self):
        self.expected["fibonacci"] = (0, "999\nresult: 999\n", "")
        self.write_candidate_examples()
        self.refresh_candidate()
        with self.assertRaisesRegex(RuntimeError, "accepted expected outputs"):
            self.run_behavior()

    def test_changed_old_source_rehashed_in_candidate_still_fails_accepted_hash(self):
        source = self.candidate / "examples/fibonacci.ax"
        source.write_bytes(source.read_bytes() + b"\n// edited fixture\n")
        self.refresh_candidate()
        with self.assertRaisesRegex(RuntimeError, "file hash differs"):
            self.run_behavior()

    def test_substituted_versions_ir_and_diagnostics_are_rejected(self):
        for fault, error in (("identity", "exact stdout differs"), ("ir", "LLVM changed"),
                             ("diagnostic", "ownership diagnostic changed")):
            with self.subTest(fault=fault), self.assertRaisesRegex(RuntimeError, error):
                self.run_behavior(fault)

    def test_existing_output_directory_and_receipt_are_preserved(self):
        args = self.arguments()
        args.output_dir.mkdir()
        marker = args.output_dir / "comparison.json"
        marker.write_bytes(b"preserve this earlier receipt\n")
        with self.assertRaises(FileExistsError):
            behavior.prepare(args)
        self.assertEqual(marker.read_bytes(), b"preserve this earlier receipt\n")

    def run_timing(self, fault=None):
        args = self.arguments()
        args.output_dir.mkdir()
        report = {"preparation": [], "samples": [], "summaries": {}}
        receipt = comparison.Receipt(args.output_dir, report)
        with mock.patch.object(comparison.shared, "run_child", side_effect=self.simulated_child(fault)):
            profile.timing(args, report, receipt, comparison)
        return report

    def test_timing_keeps_all_alternating_pairs_and_exact_shared_ir(self):
        report = self.run_timing()
        self.assertEqual(len(report["samples"]), 32)  # Four workloads, two sides, warmup + three rounds.
        self.assertEqual([pair["order"] for pair in report["summaries"]["check"]["pairs"]],
                         ["AB", "BA", "AB"])
        self.assertTrue(report["cross_version_ir_equal"])
        self.assertNotEqual(report["compilers"]["a"]["version_output"], report["compilers"]["b"]["version_output"])

    def test_timing_transition_rejects_wrong_version_ir_and_diagnostic_span(self):
        for fault in ("identity", "ir", "format"):
            with self.subTest(fault=fault), self.assertRaises((RuntimeError, comparison.shared.ProfileError)):
                self.run_timing(fault)

    def allocation_pair(self):
        args = self.arguments()
        args.output_dir.mkdir()
        args.symbolic_snapshot = self.root / "symbolic-body-05"
        args.symbolic_snapshot.mkdir()
        symbolic_compiler = args.symbolic_snapshot / "target/release/axeon"
        write(symbolic_compiler, b"symbolic fixture; never executed")
        symbolic_hash = behavior.fingerprint(symbolic_compiler)["sha256"]
        patch = mock.patch.object(profile.common, "SYMBOLIC_SHA256", symbolic_hash)
        patch.start()
        self.addCleanup(patch.stop)
        pairs = []
        for side, root in (("a", args.symbolic_snapshot), ("b", self.candidate)):
            path, report = fixtures.allocation_fixture(self.root / f"alloc-{self.sequence}-{side}", calls=4 if side == "a" else 5)
            compiler = root / "target/release/axeon"
            report["compiler"] = {"path": str(compiler), **behavior.fingerprint(compiler),
                                  "version_output": behavior.VERSIONS[side]}
            profiler = root / "tools/allocation-profiler/target/release/axeon-allocation-profiler"
            report["profiler"] = write(profiler, f"profiler {side}; never executed".encode())
            version_path = Path(report["preparation"][0]["stdout"]["path"])
            report["preparation"][0]["stdout"] = write(version_path, behavior.VERSIONS[side].encode())
            write_json(path, report)
            pairs.append((path, report))
        args.baseline_report, baseline_report = pairs[0]
        args.candidate_report, candidate_report = pairs[1]
        self.manifest(args.symbolic_snapshot, "4" * 40)
        accepted = {"status": "passed", "source_commit": profile.common.SYMBOLIC_COMMIT,
                    "snapshot": "symbolic-body-05", "snapshot_base_commit": "4" * 40,
                    "compiler_sha256": symbolic_hash, "compiler_version": "0.11.0", "language_subset": 11,
                    "copied_files": [{"path": "allocations/allocations.json",
                                      "sha256": behavior.fingerprint(args.baseline_report)["sha256"]},
                                     {"path": "snapshot-manifest.json",
                                      "sha256": behavior.fingerprint(args.symbolic_snapshot / ".snapshot-manifest.json")["sha256"]}]}
        write_json(self.candidate / "evidence/symbolic-body-20261011/acceptance.json", accepted)
        self.refresh_candidate()
        return args, baseline_report, candidate_report

    def run_allocations(self, args):
        report = {}
        receipt = comparison.Receipt(args.output_dir, report)
        profile.allocations(args, report, receipt, comparison)
        return report

    def test_allocation_transition_uses_full_existing_validator_and_count_summaries(self):
        args, _, _ = self.allocation_pair()
        report = self.run_allocations(args)
        delta = report["differences"]["arithmetic-project"]["load"]["alloc_successes"]
        self.assertEqual(delta["median_b_minus_a"], 1)
        self.assertNotIn("wall_seconds", json.dumps(report["differences"]))
        self.assertTrue(report["all_recorded_hashes_unchanged"])

    def test_allocation_transition_rejects_raw_artifact_corruption(self):
        args, _, candidate = self.allocation_pair()
        artifact = Path(candidate["samples"][0]["directory"]) / "emitted.ll"
        artifact.write_bytes(artifact.read_bytes() + b"; corrupted\n")
        with self.assertRaises(comparison.allocation.AllocationError):
            self.run_allocations(args)

    def test_allocation_transition_rejects_missing_trial(self):
        args, _, candidate = self.allocation_pair()
        candidate["samples"].pop()
        write_json(args.candidate_report, candidate)
        with self.assertRaisesRegex(comparison.allocation.AllocationError, "missing/extra allocation trials"):
            self.run_allocations(args)

    def test_allocation_transition_rejects_changed_host_affinity(self):
        args, _, candidate = self.allocation_pair()
        candidate["host_after"] = {**candidate["host_after"], "affinity": [999]}
        write_json(args.candidate_report, candidate)
        with self.assertRaisesRegex(comparison.allocation.AllocationError, "affinity"):
            self.run_allocations(args)


if __name__ == "__main__":
    unittest.main()

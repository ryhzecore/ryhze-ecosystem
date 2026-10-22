#!/usr/bin/env python3
"""Adversarial transition fixtures; never execute compilers or subprocesses."""
import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


historical = load("nominal_historical_fixtures", HERE / "test-generic-transition.py")
behavior = load("nominal_behavior_under_test", HERE / "compare-nominal-transition.py")
profile = load("nominal_profile_under_test", HERE / "profile-nominal-transition.py")
comparison = historical.comparison
write, write_json = historical.write, historical.write_json


class NominalTransitionTests(unittest.TestCase):
    def setUp(self):
        # Reuse real input generation, source manifests, receipts and the mock
        # child boundary, without inheriting/repeating the historical test suite.
        self.fixture = historical.TransitionTests(methodName="runTest")
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        f = self.fixture
        for name in profile.HELPERS:
            write(f.candidate / "scripts" / name, (HERE / name).read_bytes())
        write(f.candidate / "scripts/profile-nominal-transition.py", (HERE / "profile-nominal-transition.py").read_bytes())
        for relative in historical.behavior.NEW_FILES:
            write(f.baseline / relative, (f.candidate / relative).read_bytes())
        for relative in behavior.NEW_FILES:
            write(f.candidate / relative, (HERE.parent / relative).read_bytes())
        self.old_expected = copy.deepcopy(f.expected)
        f.expected = copy.deepcopy(historical.native.EXPECTED)
        historical.example_module(f.baseline / "scripts/record-native-examples.py", self.old_expected, f.rejection)
        f.write_candidate_examples()
        f.manifest(f.baseline, behavior.COMMIT)
        for module, key, value in ((behavior, "COMPILER", f.accepted_hash),
                                   (profile, "PACKAGE_COMPILER", f.accepted_hash),
                                   (profile, "__file__", str(f.candidate / "scripts/profile-nominal-transition.py"))):
            patch = mock.patch.object(module, key, value)
            patch.start()
            self.addCleanup(patch.stop)
        reproduction = {"source_commit": behavior.COMMIT, "exit_code": 0,
                        "independent_builds": 2, "shared_target_directory": False,
                        "builds": [{"sha256": {"target/release/axeon": f.accepted_hash}} for _ in range(2)]}
        reproduced = write_json(f.package / "reproduction.json", reproduction)
        f.package_receipt.update(source_commit=behavior.COMMIT, compiler_version="0.12.0", language_subset=12,
                                 version_output=behavior.VERSIONS["a"], reproduction_sha256=reproduced["sha256"])
        write_json(f.package / "package-receipt.json", f.package_receipt)
        profiler = f.candidate / "tools/allocation-profiler/target/release/axeon-allocation-profiler"
        self.profiler_hash = write(profiler, b"candidate profiler fixture; never executed")["sha256"]
        f.refresh_candidate()

    def arguments(self):
        args = self.fixture.arguments()
        args.trials = 7
        args.candidate_profiler_sha256 = self.profiler_hash
        args.output_dir.mkdir()
        return args

    def child(self, fault=None):
        old_child = self.fixture.simulated_child()

        def run(argv, cwd, stdout_path, stderr_path, timeout, environment=None):
            side = "b" if Path(argv[0]) == self.fixture.candidate / "target/release/axeon" else "a"
            operation = argv[1]
            # The historical dispatcher handles all frozen old workloads. Its
            # version is deliberately replaced by each designated side here.
            result = old_child(argv, cwd, stdout_path, stderr_path, timeout, environment)
            if operation == "--version":
                value = behavior.VERSIONS["a" if fault == "identity" and side == "b" else side].encode()
                result["stdout"] = write(stdout_path, value)
            if operation == "test":
                value = behavior.TEST_OUTPUT
                if fault == "test-output":
                    value = value.replace("3 passed; 0 failed", "2 passed; 1 failed")
                result["stdout"] = write(stdout_path, value.encode())
            if operation == "interpret" and "generic-nominals" in str(argv[2]) and fault == "nominal-output":
                result["stdout"] = write(stdout_path, b"42\n0\nresult: 0\n")
            if operation == "emit-ir" and side == "b" and fault == "ir":
                write(Path(argv[argv.index("-o") + 1]), historical.IR + b"; altered\n")
            if operation == "check" and "ownership-error.ax" in str(argv[2]) and side == "b" and fault == "diagnostic":
                rejection = copy.deepcopy(historical.native.EXPECTED_REJECTION)
                rejection["diagnostics"][0]["message"] += " altered"
                result["stderr"] = write(stderr_path, json.dumps(rejection).encode())
            if operation == "format" and fault == "format-span":
                result["stderr"] = write(stderr_path, historical.profile.FORMAT_ERROR.replace(b"300001:1", b"300002:1"))
            return result
        return run

    def run_behavior(self, fault=None):
        args = self.arguments()
        report = {"samples": [], "comparisons": []}
        receipt = comparison.Receipt(args.output_dir, report)
        with mock.patch.object(comparison.shared, "run_child", side_effect=self.child(fault)) as child:
            behavior.compare(args, report, receipt, comparison)
        return report, child.call_count

    def run_timing(self, fault=None):
        args = self.arguments()
        report = {"preparation": [], "samples": [], "summaries": {}}
        receipt = comparison.Receipt(args.output_dir, report)
        with mock.patch.object(comparison.shared, "run_child", side_effect=self.child(fault)):
            profile.timing(args, report, receipt, comparison)
        return report

    def test_all_18_previous_examples_remain_and_nominal_tests_are_separate(self):
        report, calls = self.run_behavior()
        self.assertEqual(calls, 79)
        self.assertEqual(report["example_count"], 18)
        self.assertEqual({v["source"] for v in report["comparisons"]},
                         {f"examples/{name}.ax" for name in self.old_expected})
        self.assertEqual(set(report["added_example_sources"]), behavior.NEW_FILES)
        self.assertEqual(report["added_reference_tests"], 3)
        self.assertTrue(report["all_input_and_output_hashes_rechecked"])

    def test_rehashed_old_generic_source_still_fails_accepted_bytes(self):
        source = self.fixture.candidate / "examples/generic-modules/main.ax"
        source.write_bytes(source.read_bytes() + b"\n// changed accepted workload\n")
        self.fixture.refresh_candidate()
        with self.assertRaisesRegex(RuntimeError, "file hash differs"):
            self.run_behavior()

    def test_old_or_nominal_expected_answer_cannot_redefine_oracle(self):
        for name in ("generic-modules/main", "generic-nominals/main"):
            with self.subTest(name=name):
                saved = self.fixture.expected[name]
                self.fixture.expected[name] = (0, "999\nresult: 0\n", "")
                self.fixture.write_candidate_examples()
                self.fixture.refresh_candidate()
                with self.assertRaisesRegex(RuntimeError, "example set, expectations or diagnostic changed"):
                    self.run_behavior()
                self.fixture.expected[name] = saved

    def test_nominal_source_omission_is_not_hidden_by_rehashed_manifest(self):
        (self.fixture.candidate / "examples/generic-nominals/packets.ax").unlink()
        self.fixture.refresh_candidate()
        with self.assertRaisesRegex(RuntimeError, "accepted source removed or unexpected source added"):
            self.run_behavior()

    def test_identity_ir_diagnostic_and_nominal_results_reject_real_drift(self):
        for fault, message in (("identity", "exact stdout differs"), ("ir", "accepted LLVM differs"),
                               ("diagnostic", "accepted ownership diagnostic changed"),
                               ("nominal-output", "exact stdout differs"), ("test-output", "exact stdout differs")):
            with self.subTest(fault=fault), self.assertRaisesRegex(RuntimeError, message):
                self.run_behavior(fault)

    def test_package_version_and_reproduction_cannot_be_substituted(self):
        f = self.fixture
        f.package_receipt["compiler_version"] = "0.13.0"
        write_json(f.package / "package-receipt.json", f.package_receipt)
        with self.assertRaisesRegex(RuntimeError, "accepted package identity differs"):
            profile.verify_package(f.package, profile.common.Frozen())
        f.package_receipt["compiler_version"] = "0.12.0"
        write_json(f.package / "package-receipt.json", f.package_receipt)
        reproduction = json.loads((f.package / "reproduction.json").read_text())
        reproduction["shared_target_directory"] = True
        record = write_json(f.package / "reproduction.json", reproduction)
        f.package_receipt["reproduction_sha256"] = record["sha256"]
        write_json(f.package / "package-receipt.json", f.package_receipt)
        with self.assertRaisesRegex(RuntimeError, "accepted independent reproduction differs"):
            profile.verify_package(f.package, profile.common.Frozen())

    def test_timing_retains_seven_alternating_pairs_and_old_workload(self):
        report = self.run_timing()
        self.assertEqual(len(report["samples"]), 64)
        self.assertEqual([v["order"] for v in report["summaries"]["check"]["pairs"]],
                         ["AB", "BA", "AB", "BA", "AB", "BA", "AB"])
        self.assertEqual(report["configuration"]["trials"], 7)
        self.assertTrue(report["cross_version_ir_equal"])
        self.assertIn("delivered checkpoint0.13", report["scope"]["comparison"])

    def test_timing_cannot_accept_wrong_identity_llvm_or_formatter_span(self):
        for fault in ("identity", "ir", "format-span"):
            with self.subTest(fault=fault), self.assertRaises((RuntimeError, comparison.shared.ProfileError)):
                self.run_timing(fault)

    def test_rehashed_measurement_helper_does_not_change_frozen_workload(self):
        path = self.fixture.candidate / "scripts/profile-compiler.py"
        path.write_bytes(path.read_bytes() + b"\n# changed measurement producer\n")
        self.fixture.refresh_candidate()
        with self.assertRaisesRegex(RuntimeError, "frozen helper manifest differs"):
            profile.inputs(self.arguments(), {})

    def test_allocation_cannot_replace_qualified_acceptance_by_rehashing(self):
        path = self.fixture.candidate / "evidence/nominal-identities/validation.json"
        write_json(path, {"status": "accepted_internal_prerequisite", "compiler_sha256": "0" * 64})
        self.fixture.refresh_candidate()
        args = self.arguments()
        report = {}
        receipt = comparison.Receipt(args.output_dir, report)
        with mock.patch.object(comparison, "validate_allocation_run") as validator:
            with self.assertRaisesRegex(RuntimeError, "identity acceptance manifest differs"):
                profile.allocations(args, report, receipt, comparison)
        validator.assert_not_called()

    def test_candidate_example_rejects_substituted_profiler_before_execution(self):
        args = self.arguments()
        args.candidate_profiler_sha256 = "0" * 64
        report = {"preparation": [], "samples": [], "summaries": {}}
        with mock.patch.object(comparison.shared, "run_child") as child:
            with self.assertRaisesRegex(RuntimeError, "file hash differs"):
                profile.example(args, report, comparison.Receipt(args.output_dir, report), comparison)
        child.assert_not_called()

    def test_candidate_example_records_invalid_identity_before_rejection(self):
        args = self.arguments()
        report = {"preparation": [], "samples": [], "summaries": {}}
        with mock.patch.object(comparison.shared, "run_child", side_effect=self.child("identity")):
            with self.assertRaises(comparison.shared.ProfileError):
                profile.example(args, report, comparison.Receipt(args.output_dir, report), comparison)
        saved = json.loads((args.output_dir / "comparison.json").read_text())
        self.assertEqual(len(saved["preparation"]), 1)
        self.assertFalse(saved["preparation"][0]["valid"])
        self.assertEqual(saved["samples"], [])


if __name__ == "__main__":
    unittest.main()

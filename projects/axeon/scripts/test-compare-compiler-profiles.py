#!/usr/bin/env python3
"""Controlled Python fixtures only: never executes a compiler or profiler."""
import argparse
import copy
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location("axeon_compare_profiles", Path(__file__).with_name("compare-compiler-profiles.py"))
compare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(compare)
HOST = {key: "controlled fixture" for key in compare.HOST_KEYS}
VERSION = b"axeon 0.9.0; subset 9; LLVM/Clang 19.1.7\n"
IR = b'target triple = "x86_64-unknown-linux-gnu"\ndefine i64 @test() { ret i64 0 }\n'


def file_record(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(data)
    return {"path": str(path), **compare.allocation.fingerprint(path)}


def phase(name, calls=0):
    events = {key: 0 for key in compare.allocation.EVENTS}
    events.update(alloc_attempts=calls, alloc_successes=calls, dealloc_calls=calls,
                  allocation_requested_bytes=calls * 8, allocation_successful_bytes=calls * 8,
                  deallocated_bytes=calls * 8)
    return {"name": name, "retained_at_end": "controlled fixture", "accounting_valid": True, "events": events,
            "live_requested_bytes_begin": 100, "live_requested_bytes_end": 100,
            "live_blocks_begin": 2, "live_blocks_end": 2, "peak_live_requested_bytes": 100 + calls * 8,
            "peak_increase_over_begin_bytes": calls * 8, "net_live_requested_bytes": 0}


def allocation_fixture(directory, calls=4):
    """Complete small output receipts with the real shared input-generation API."""
    directory.mkdir()
    sources, arithmetic = compare.shared.generated_sources()
    sources.update(compare.allocation.MODULE_SOURCES)
    inputs = {}
    for name, data in sources.items():
        record = file_record(directory / "inputs" / name, data)
        inputs[name] = {k: record[k] for k in ("bytes", "sha256")}
    preparations = []

    def prepare(name, stdout=b"", stderr=b"", ir=False, code=0):
        prefix = directory / "preparation" / name
        value = {"name": name, "valid": True, "timed_out": False, "returncode": code,
                 "stdout": file_record(prefix.with_suffix(".stdout"), stdout),
                 "stderr": file_record(prefix.with_suffix(".stderr"), stderr)}
        if ir:
            value["artifact"] = file_record(prefix.with_suffix(".ll"), IR)
        preparations.append(value)
        return value

    prepare("compiler-version", VERSION)
    expectations = {}
    artifact_bytes = {}
    for name, stdout in (("arithmetic-project", arithmetic),
                         ("aggregate-modules-project", b"f64:0x8000000000000000\n42\nresult: 42\n")):
        prepare(name + "-check")
        prepare(name + "-interpret", stdout)
        emitted = prepare(name + "-emit-ir", ir=True)
        parts = stdout.rsplit(b"result: ", 1)
        wanted_sources = {"arithmetic.ax": len(sources["arithmetic.ax"])} if name == "arithmetic-project" else {
            Path(p).name: len(data) for p, data in compare.allocation.MODULE_SOURCES.items()}
        expectations[name] = {"mode": "project", "outcome": "success", "exit": 0, "codes": [],
            "result": int(parts[1]), "sources": wanted_sources,
            "artifacts": {"emitted.ll": {"sha256": emitted["artifact"]["sha256"]},
                          "stdout.txt": {"sha256": compare.hashlib.sha256(parts[0]).hexdigest()}}}
        artifact_bytes[name] = {"emitted.ll": IR, "stdout.txt": parts[0]}
    formatted = prepare("format-success", sources["arithmetic.ax"])
    prepare("format-budget-valid-source")
    diagnostic = (str(directory / "inputs/format-budget.ax") + ":300001:1: error[E_FORMAT_LIMIT]: limit (bytes 900065..900066)\n").encode()
    prepare("format-budget-rejection", stderr=diagnostic, code=1)
    expectations["format-success"] = {"mode": "format", "outcome": "success", "exit": 0, "codes": [],
        "result": None, "sources": {"arithmetic.ax": len(sources["arithmetic.ax"])},
        "artifacts": {"formatted.ax": {"sha256": formatted["stdout"]["sha256"]}}}
    expectations["format-budget-rejection"] = {"mode": "format", "outcome": "format_error", "exit": 1,
        "codes": ["E_FORMAT_LIMIT"], "result": None, "sources": {"format-budget.ax": len(sources["format-budget.ax"])},
        "artifacts": {"diagnostics.txt": {"contains": "error[E_FORMAT_LIMIT]"}}}
    artifact_bytes.update({"format-success": {"formatted.ax": sources["arithmetic.ax"]},
                           "format-budget-rejection": {"diagnostics.txt": diagnostic}})
    samples = []
    for trial in (0, 1):
        for workload in compare.allocation.WORKLOADS:
            expected = expectations[workload]
            sample_dir = directory / "samples" / f"{workload}-{trial}"
            sample_dir.mkdir(parents=True)
            outputs = {}
            for name, data in artifact_bytes[workload].items():
                record = file_record(sample_dir / name, data)
                outputs[name] = {k: record[k] for k in ("bytes", "sha256")}
            names = compare.allocation.PROJECT_PHASES if expected["mode"] == "project" else compare.allocation.FORMAT_PHASES
            child_report = {"schema_version": 1, "profiler_version": "0.1.0", "measurement": "observed-rust-global-allocator-calls",
                "status": "complete", "active_phase": None, "mode": expected["mode"], "outcome": expected["outcome"],
                "failure": None, "fuel": compare.allocation.FUEL, "max_fuel": 10_000_000,
                "scope": {"check_includes_verification": True, "verify_ir_repeats_check_verification": True,
                          "phases_are_disjoint_pipeline": False} if expected["mode"] == "project" else {},
                "phases": [phase(name, calls if i == 0 else 0) for i, name in enumerate(names)],
                "skipped_phases": [], "diagnostic_codes": expected["codes"], "result": expected["result"], "trap": None,
                "sources": [{"logical_path": name, "bytes": size} for name, size in expected["sources"].items()],
                "artifacts": [{"path": name, "bytes": len(data)} for name, data in artifact_bytes[workload].items()]}
            record = file_record(sample_dir / "report.json", json.dumps(child_report).encode())
            outputs["report.json"] = {k: record[k] for k in ("bytes", "sha256")}
            process = {"timed_out": False, "returncode": expected["exit"],
                       "stdout": file_record(sample_dir.with_suffix(".stdout"), b""),
                       "stderr": file_record(sample_dir.with_suffix(".stderr"), b""),
                       "wall_seconds": 987654321}  # Must never enter count comparison.
            samples.append({"workload": workload, "round_kind": "warmup" if trial == 0 else "measured", "trial": trial,
                            "valid": True, "process": process, "directory": str(sample_dir),
                            "allocation_report": child_report, "outputs": outputs})
    report = {"schema_version": 1, "workload_version": compare.allocation.VERSION, "status": "passed",
              "compiler": file_record(directory / "compiler", b"not executed compiler"),
              "profiler": file_record(directory / "profiler", f"not executed profiler {calls}".encode()),
              "harness": {"path": str(compare.ALLOCATION_PATH), **compare.allocation.fingerprint(compare.ALLOCATION_PATH)},
              "shared_harness": {"path": str(compare.allocation.SHARED_PATH), **compare.allocation.fingerprint(compare.allocation.SHARED_PATH)},
              "inputs": inputs, "configuration": {"trials": 1, "warmups": 1, "fuel": compare.allocation.FUEL,
                                                  "workload_order": list(compare.allocation.WORKLOADS)},
              "preparation": preparations, "samples": samples, "expected_results": expectations,
              "summaries": compare.allocation.summarize_samples(samples), "host_before": HOST, "host_after": HOST, "scope": {}}
    report["compiler"]["version_output"] = VERSION.decode()
    path = directory / "allocations.json"
    file_record(path, json.dumps(report).encode())
    return path, report


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="axeon-paired-comparison-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.compiler_a, self.compiler_b = self.root / "a", self.root / "b"
        for path in (self.compiler_a, self.compiler_b):
            path.write_bytes(b"controlled mock: never executed")
            path.chmod(0o755)
        self.sequence = 0

    def timing(self, fault=None, trials=7, hosts=None):
        self.sequence += 1
        output = self.root / f"timing-{self.sequence}"
        output.mkdir()
        args = argparse.Namespace(mode="timing", compiler_a=self.compiler_a, compiler_b=self.compiler_b,
                                  output_dir=output, trials=trials, timeout_seconds=1, label="test")

        def run(argv, cwd, stdout_path, stderr_path, timeout, environment):
            side = "a" if Path(argv[0]) == self.compiler_a else "b"
            operation = argv[1]
            stdout, stderr, code = b"", b"", 0
            if operation == "--version":
                stdout = VERSION
            elif operation == "interpret":
                stdout = compare.shared.generated_sources()[1]
            elif operation == "emit-ir":
                Path(argv[-1]).write_bytes(IR)
            elif operation == "format":
                stderr, code = b"format-budget.ax:1:1: error[E_FORMAT_LIMIT]: fixed rejection\n", 1
            sample = {"returncode": code, "timed_out": False, "wall_seconds": 1 if side == "a" else 2,
                      "user_cpu_seconds": 0, "system_cpu_seconds": 0, "max_rss_kib": 10,
                      "stdout": file_record(stdout_path, stdout), "stderr": file_record(stderr_path, stderr)}
            if "startup-measured-007-b" in str(stdout_path):
                sample["wall_seconds"] = 10
            if fault:
                fault(side, operation, sample, argv, Path(cwd), stdout_path)
            return sample

        with mock.patch.object(compare.shared, "run_child", side_effect=run), mock.patch.object(compare.shared, "host_configuration", side_effect=hosts if hosts is not None else [HOST, HOST]), mock.patch("sys.stdout", new=io.StringIO()):
            code = compare.execute(args)
        return code, json.loads((output / "comparison.json").read_text()), output

    def allocations(self, a, b):
        self.sequence += 1
        output = self.root / f"compare-allocations-{self.sequence}"
        output.mkdir()
        args = argparse.Namespace(mode="allocations", report_a=a, report_b=b, output_dir=output, label="test")
        with mock.patch.object(compare.shared, "host_configuration", return_value=HOST), mock.patch("sys.stdout", new=io.StringIO()):
            code = compare.execute(args)
        return code, json.loads((output / "comparison.json").read_text())

    def test_all_pairs_and_slow_trial_are_retained_with_exact_workloads(self):
        code, report, output = self.timing()
        self.assertEqual(code, 0)
        self.assertEqual(len(report["preparation"]), 6)
        self.assertEqual(len(report["samples"]), 64)
        startup = report["summaries"]["startup"]
        self.assertEqual(startup["paired_wall_ratio_b_over_a"]["maximum"], 10)
        self.assertEqual(startup["paired_wall_ratio_b_over_a"]["n"], 7)
        self.assertEqual([p["order"] for p in startup["pairs"]], ["AB", "BA", "AB", "BA", "AB", "BA", "AB"])
        for name, data in compare.shared.generated_sources()[0].items():
            self.assertEqual((output / name).read_bytes(), data)
        self.assertTrue(all(s["valid"] for s in report["samples"]))

    def test_version_difference_stops_before_measurement(self):
        def fault(side, operation, sample, argv, cwd, path):
            if side == "b" and operation == "--version":
                Path(sample["stdout"]["path"]).write_bytes(VERSION.replace(b"19.1.7", b"19.1.8"))
                sample["stdout"].update(compare.allocation.fingerprint(sample["stdout"]["path"]))
        code, report, _ = self.timing(fault)
        self.assertEqual(code, 1)
        self.assertIn("identities differ", report["failure"])
        self.assertEqual(report["samples"], [])

    def test_timing_affinity_change_rejects_complete_measurements(self):
        before, after = copy.deepcopy(HOST), copy.deepcopy(HOST)
        before["affinity"], after["affinity"] = [0, 1], [0]
        code, report, _ = self.timing(trials=1, hosts=[before, after])
        self.assertEqual(code, 1)
        self.assertEqual(report["status"], "failed")
        self.assertIn("timing recorded host keys changed: affinity", report["failure"])
        self.assertEqual(report["host_before"]["affinity"], [0, 1])
        self.assertEqual(report["host_after"]["affinity"], [0])
        self.assertEqual(len(report["samples"]), 16)
        self.assertTrue(all(sample["valid"] for sample in report["samples"]))

    def test_llvm_or_error_output_difference_rejects_candidate(self):
        for operation in ("emit-ir", "format"):
            def fault(side, op, sample, argv, cwd, path):
                if side == "b" and op == operation:
                    if op == "emit-ir":
                        Path(argv[-1]).write_bytes(IR.replace(b"ret i64 0", b"ret i64 1"))
                    else:
                        Path(sample["stderr"]["path"]).write_bytes(b"error[E_FORMAT_LIMIT]: different span/message\n")
                        sample["stderr"].update(compare.allocation.fingerprint(sample["stderr"]["path"]))
            code, report, _ = self.timing(fault)
            self.assertEqual(code, 1)
            self.assertIn("outputs differ", report["failure"])
            self.assertFalse(report["samples"][-1]["valid"])

    def test_timeout_is_preserved_as_failed_observation(self):
        def fault(side, op, sample, argv, cwd, path):
            if side == "b" and op == "check":
                sample["timed_out"] = True
                sample["returncode"] = -9
        code, report, _ = self.timing(fault)
        self.assertEqual(code, 1)
        self.assertTrue(report["preparation"][-1]["timed_out"])
        self.assertFalse(report["preparation"][-1]["valid"])

    def test_late_failure_retains_all_earlier_measurements_without_success_summary(self):
        def fault(side, op, sample, argv, cwd, path):
            if "startup-measured-007-b" in str(path):
                sample["returncode"] = 1
        code, report, _ = self.timing(fault)
        self.assertEqual(code, 1)
        self.assertEqual(len(report["samples"]), 58)
        self.assertTrue(all(s["valid"] for s in report["samples"][:-1]))
        self.assertFalse(report["samples"][-1]["valid"])
        self.assertEqual(report["summaries"], {})

    def test_input_mutation_aborts_without_accepting_sample(self):
        def fault(side, op, sample, argv, cwd, path):
            if op == "interpret":
                (cwd / "arithmetic.ax").write_bytes(b"changed")
        code, report, _ = self.timing(fault)
        self.assertEqual(code, 1)
        self.assertIn("changed", report["failure"])
        self.assertFalse(report["preparation"][-1]["valid"])

    def test_compiler_and_previously_valid_output_mutation_are_detected(self):
        for kind in ("compiler", "output"):
            def fault(side, op, sample, argv, cwd, path):
                if side == "b" and "startup-warmup" in str(path):
                    target = self.compiler_a if kind == "compiler" else cwd / "samples/startup-warmup-000-a.stdout"
                    target.write_bytes(b"changed")
            code, report, _ = self.timing(fault)
            self.assertEqual(code, 1)
            self.assertIn("changed", report["failure"])

    def test_missing_duplicate_and_reordered_pairs_are_invalid(self):
        code, report, _ = self.timing(trials=2)
        self.assertEqual(code, 0)
        for operation in (lambda s: s.pop(), lambda s: s.append(copy.deepcopy(s[-1])), lambda s: s.reverse()):
            samples = copy.deepcopy(report["samples"])
            operation(samples)
            with self.assertRaises(compare.allocation.AllocationError):
                compare.timing_summaries(samples, 2)

    def test_existing_output_is_preserved(self):
        directory = self.root / "existing"
        directory.mkdir()
        marker = directory / "marker"
        marker.write_text("keep")
        with mock.patch("sys.stderr", new=io.StringIO()), self.assertRaises(SystemExit):
            compare.main(["timing", "--compiler-a", str(self.compiler_a), "--compiler-b", str(self.compiler_b), "--output-dir", str(directory)])
        self.assertEqual(marker.read_text(), "keep")

    def test_atomic_receipt_failure_and_external_mutation_preserve_prior_bytes(self):
        directory = self.root / "receipt"
        directory.mkdir()
        receipt = compare.Receipt(directory, {"status": "running"})
        receipt.save()
        prior = (directory / "comparison.json").read_bytes()
        with mock.patch.object(Path, "replace", side_effect=OSError("injected")), self.assertRaises(OSError):
            receipt.save()
        self.assertEqual((directory / "comparison.json").read_bytes(), prior)
        (directory / "comparison.json").write_text("external")
        with self.assertRaisesRegex(compare.allocation.AllocationError, "externally"):
            receipt.save()
        self.assertEqual((directory / "comparison.json").read_text(), "external")

    def test_allocation_counts_compare_with_path_only_normalization(self):
        a, _ = allocation_fixture(self.root / "alloc-a", 4)
        b, _ = allocation_fixture(self.root / "alloc-b", 2)
        code, report = self.allocations(a, b)
        self.assertEqual(code, 0)
        delta = report["differences"]["arithmetic-project"]["load"]["alloc_successes"]
        self.assertEqual(delta["median_b_minus_a"], -2)
        self.assertEqual(delta["median_ratio_b_over_a"], .5)
        self.assertNotIn("wall_seconds", json.dumps(report["differences"]))
        zero = report["differences"]["arithmetic-project"]["load"]["zeroed_successes"]
        self.assertIsNone(zero["median_ratio_b_over_a"])

    def test_matching_affinity_changes_in_both_allocation_runs_are_rejected(self):
        a, original_a = allocation_fixture(self.root / "alloc-a")
        b, original_b = allocation_fixture(self.root / "alloc-b")
        for path, original in ((a, original_a), (b, original_b)):
            original["host_before"] = {**HOST, "affinity": [0, 1]}
            original["host_after"] = {**HOST, "affinity": [0]}
            path.write_text(json.dumps(original))
        code, report = self.allocations(a, b)
        self.assertEqual(code, 1)
        self.assertIn("allocation recorded host keys changed: affinity", report["failure"])
        self.assertNotIn("differences", report)

    def test_all_host_identity_keys_must_stay_constant_across_snapshots(self):
        for key in compare.HOST_KEYS:
            baseline, changed = copy.deepcopy(HOST), copy.deepcopy(HOST)
            changed[key] = "different"
            with self.subTest(key=key), self.assertRaisesRegex(compare.allocation.AllocationError, key):
                compare.require_stable_hosts([baseline, baseline, baseline, changed], "fixture")

    def test_allocation_phase_corruption_missing_trial_or_bad_oracle_is_rejected(self):
        a, _ = allocation_fixture(self.root / "alloc-a")
        b, original = allocation_fixture(self.root / "alloc-b")
        for change in (lambda r: r["samples"].pop(),
                       lambda r: r["samples"][0]["allocation_report"]["phases"][0]["events"].update(deallocated_bytes=999),
                       lambda r: r["expected_results"]["format-budget-rejection"].update(exit=0),
                       lambda r: r["summaries"]["arithmetic-project"]["load"]["events"]["alloc_successes"].update(median=99)):
            altered = copy.deepcopy(original)
            change(altered)
            b.write_text(json.dumps(altered))
            code, report = self.allocations(a, b)
            self.assertEqual(code, 1)
            self.assertEqual(report["status"], "failed")

    def test_allocation_mutated_raw_output_or_mismatched_input_is_rejected(self):
        a, _ = allocation_fixture(self.root / "alloc-a")
        b, report = allocation_fixture(self.root / "alloc-b")
        raw = Path(report["samples"][0]["directory"]) / "emitted.ll"
        original = raw.read_bytes()
        raw.write_bytes(b"changed")
        code, _ = self.allocations(a, b)
        self.assertEqual(code, 1)
        raw.write_bytes(original)
        (b.parent / "inputs/arithmetic.ax").write_bytes(b"changed")
        code, _ = self.allocations(a, b)
        self.assertEqual(code, 1)

    def test_allocation_harness_hash_and_version_mismatch_are_rejected(self):
        a, _ = allocation_fixture(self.root / "alloc-a")
        b, original = allocation_fixture(self.root / "alloc-b")
        for name in ("harness", "version"):
            altered = copy.deepcopy(original)
            if name == "harness":
                altered["harness"]["sha256"] = "0" * 64
            else:
                altered["compiler"]["version_output"] = VERSION.decode().replace("0.9.0", "0.9.1")
            b.write_text(json.dumps(altered))
            code, _ = self.allocations(a, b)
            self.assertEqual(code, 1)
    def test_duplicate_json_keys_and_nonfinite_numbers_are_rejected(self):
        path = self.root / "invalid.json"
        for data in ('{"status":"passed","status":"failed"}', '{"n":NaN}'):
            path.write_text(data)
            with self.assertRaises(compare.allocation.AllocationError):
                compare.read_large_json(path)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Pure Python receipt/harness tests; no compiler or profiler is executed."""
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location("allocation_profile", Path(__file__).with_name("profile-allocations.py"))
profile = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(profile)


def phase(name):
    return {
        "name": name, "retained_at_end": "test baseline", "accounting_valid": True,
        "events": {name: 0 for name in profile.EVENTS},
        "live_requested_bytes_begin": 100, "live_requested_bytes_end": 100,
        "live_blocks_begin": 2, "live_blocks_end": 2,
        "peak_live_requested_bytes": 100, "peak_increase_over_begin_bytes": 0,
        "net_live_requested_bytes": 0,
    }


class ReceiptTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="axeon-allocation-harness-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.directory = self.root / "sample"
        self.directory.mkdir()
        contents = b"fn main() -> i64 { return 42; }\n"
        (self.directory / "formatted.ax").write_bytes(contents)
        self.expected = {
            "mode": "format", "outcome": "success", "exit": 0, "codes": [], "result": None,
            "sources": {"input.ax": 26}, "artifacts": {"formatted.ax": {"sha256": hashlib.sha256(contents).hexdigest()}},
        }
        self.report = {
            "schema_version": 1, "profiler_version": "0.1.0", "measurement": "observed-rust-global-allocator-calls",
            "mode": "format", "status": "complete", "outcome": "success", "active_phase": None,
            "fuel": profile.FUEL, "max_fuel": 10_000_000, "scope": {},
            "phases": [phase(name) for name in profile.FORMAT_PHASES], "skipped_phases": [],
            "sources": [{"logical_path": "input.ax", "bytes": 26}],
            "artifacts": [{"path": "formatted.ax", "bytes": len(contents)}],
            "diagnostic_codes": [], "result": None, "trap": None, "failure": None,
        }
        self.process = {"returncode": 0, "timed_out": False, "stdout": {"bytes": 0}, "stderr": {"bytes": 0}}
        self.write()

    def write(self):
        (self.directory / "report.json").write_text(json.dumps(self.report), encoding="utf-8")

    def validate(self):
        return profile.validate_result(self.directory, self.process, self.expected)

    def test_valid_receipt_and_frees_of_preexisting_blocks(self):
        release = self.report["phases"][2]
        release["events"].update(dealloc_calls=1, deallocated_bytes=32)
        release.update(live_requested_bytes_end=68, live_blocks_end=1, net_live_requested_bytes=-32)
        self.write()
        report, outputs = self.validate()
        self.assertEqual(report["phases"][2]["net_live_requested_bytes"], -32)
        self.assertEqual({p.name for p in outputs}, {"report.json", "formatted.ax"})

    def test_failed_realloc_keeps_old_size_and_cannot_report_retired_bytes(self):
        event = self.report["phases"][1]["events"]
        event.update(realloc_attempts=1, realloc_failures=1, realloc_requested_bytes=64)
        self.write()
        self.validate()
        event["realloc_successful_old_bytes"] = 32
        self.write()
        with self.assertRaises(profile.AllocationError):
            self.validate()

    def test_successful_alloc_zeroed_realloc_and_dealloc_equations(self):
        item = phase("test")
        item["events"].update(alloc_attempts=1, alloc_successes=1, zeroed_attempts=1, zeroed_successes=1,
                              allocation_requested_bytes=48, allocation_successful_bytes=48,
                              realloc_attempts=2, realloc_successes=2, realloc_requested_bytes=80,
                              realloc_successful_old_bytes=96, realloc_successful_new_bytes=80,
                              dealloc_calls=2, deallocated_bytes=32)
        item.update(peak_live_requested_bytes=196, peak_increase_over_begin_bytes=96)
        profile.validate_phase(item)
        for key, value in (("alloc_attempts", 2), ("allocation_requested_bytes", 47),
                           ("realloc_successes", 1), ("deallocated_bytes", 31)):
            broken = copy.deepcopy(item)
            broken["events"][key] = value
            with self.subTest(counter=key), self.assertRaises(profile.AllocationError):
                profile.validate_phase(broken)

    def test_partial_invalid_missing_or_misordered_phases_are_rejected(self):
        original = copy.deepcopy(self.report)
        for change in (
            lambda r: r.update(status="running", active_phase="format"),
            lambda r: r.update(status="invalid"),
            lambda r: r["phases"].pop(),
            lambda r: r["phases"].reverse(),
            lambda r: r["phases"][0].update(accounting_valid=False),
            lambda r: r.update(failure="IO failure"),
        ):
            self.report = copy.deepcopy(original)
            change(self.report)
            self.write()
            with self.assertRaises(profile.AllocationError):
                self.validate()

    def project_report(self):
        (self.directory / "formatted.ax").unlink()
        contents = {"emitted.ll": b"verified fixture IR\n", "stdout.txt": b""}
        for name, content in contents.items():
            (self.directory / name).write_bytes(content)
        self.expected.update(mode="project", result=42, artifacts={
            name: {"sha256": hashlib.sha256(content).hexdigest()} for name, content in contents.items()})
        self.report.update(mode="project", result=42,
            phases=[phase(name) for name in profile.PROJECT_PHASES],
            scope={"check_includes_verification": True, "verify_ir_repeats_check_verification": True,
                   "phases_are_disjoint_pipeline": False},
            artifacts=[{"path": name, "bytes": len(content)} for name, content in contents.items()])
        self.write()

    def test_explicit_verification_is_required_in_order_and_disclosed_as_duplicate(self):
        self.project_report()
        self.validate()
        original = copy.deepcopy(self.report)
        for change in (
            lambda r: r["phases"].pop(2),
            lambda r: r["phases"].insert(3, r["phases"].pop(2)),
            lambda r: r["scope"].pop("check_includes_verification"),
            lambda r: r["scope"].update(verify_ir_repeats_check_verification=False),
            lambda r: r["scope"].update(phases_are_disjoint_pipeline=True),
        ):
            self.report = copy.deepcopy(original)
            change(self.report)
            self.write()
            with self.assertRaises(profile.AllocationError):
                self.validate()

    def test_project_errors_require_exact_cleanup_and_skipped_verification_plan(self):
        self.project_report()
        for name in ("emitted.ll", "stdout.txt"):
            (self.directory / name).unlink()
        diagnostic = b"independently expected rejection\n"
        (self.directory / "diagnostics.txt").write_bytes(diagnostic)
        for outcome in ("load_error", "check_error", "verification_error"):
            phases, skipped = profile.PHASE_PLANS[("project", outcome)]
            self.expected.update(outcome=outcome, exit=1, result=None,
                artifacts={"diagnostics.txt": {"sha256": hashlib.sha256(diagnostic).hexdigest()}})
            self.report.update(outcome=outcome, result=None, phases=[phase(name) for name in phases],
                skipped_phases=list(skipped), artifacts=[{"path": "diagnostics.txt", "bytes": len(diagnostic)}])
            self.process["returncode"] = 1
            self.write()
            self.validate()
            self.report["skipped_phases"] = []
            self.write()
            with self.subTest(outcome=outcome), self.assertRaisesRegex(profile.AllocationError, "skipped phases"):
                self.validate()

    def test_bad_types_and_impossible_peaks_are_rejected(self):
        for key, value in (("live_blocks_begin", True), ("peak_live_requested_bytes", 99),
                           ("peak_live_requested_bytes", 101), ("net_live_requested_bytes", 1)):
            broken = phase("test")
            broken[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(profile.AllocationError):
                profile.validate_phase(broken)
        inflated = phase("test")
        inflated.update(peak_live_requested_bytes=101, peak_increase_over_begin_bytes=1)
        with self.assertRaisesRegex(profile.AllocationError, "peak exceeds"):
            profile.validate_phase(inflated)

    def test_wrong_artifact_bytes_or_wrong_status_are_rejected(self):
        (self.directory / "formatted.ax").write_bytes(b"wrong")
        with self.assertRaises(profile.AllocationError):
            self.validate()
        self.process["returncode"] = 1
        with self.assertRaisesRegex(profile.AllocationError, "exit"):
            self.validate()

    def test_report_and_output_mutation_during_verification_is_detected(self):
        original = profile.load_json
        def mutate(path):
            report = original(path)
            output = self.directory / "formatted.ax"
            output.write_bytes(b"x" * output.stat().st_size)
            return report
        with mock.patch.object(profile, "load_json", side_effect=mutate):
            with self.assertRaisesRegex(profile.AllocationError, "changed"):
                self.validate()

    def test_missing_report_timeout_and_unexpected_output_are_rejected(self):
        self.process["timed_out"] = True
        with self.assertRaisesRegex(profile.AllocationError, "timed out"):
            self.validate()
        self.process["timed_out"] = False
        self.process["stderr"]["bytes"] = 3
        with self.assertRaisesRegex(profile.AllocationError, "unexpected process output"):
            self.validate()
        self.process["stderr"]["bytes"] = 0
        (self.directory / "report.json").unlink()
        with self.assertRaisesRegex(profile.AllocationError, "missing report"):
            self.validate()

    def test_path_traversal_unknown_artifacts_and_symlinks_are_rejected(self):
        self.report["artifacts"][0]["path"] = "../escape"
        self.write()
        with self.assertRaises(profile.AllocationError):
            self.validate()
        self.report["artifacts"][0]["path"] = "formatted.ax"
        self.write()
        output = self.directory / "formatted.ax"
        output.unlink()
        target = self.root / "outside"
        target.write_bytes(b"outside")
        try:
            output.symlink_to(target)
        except (OSError, NotImplementedError):
            self.skipTest("symbolic links unavailable")
        with self.assertRaises(profile.AllocationError):
            self.validate()

    def test_duplicate_keys_and_nonfinite_json_are_rejected(self):
        path = self.directory / "report.json"
        for text in ('{"status":"complete","status":"running"}', '{"count":NaN}'):
            path.write_text(text)
            with self.assertRaises(profile.AllocationError):
                profile.load_json(path)

    def test_input_and_binary_mutation_is_detected_by_hash(self):
        for name in ("input.ax", "profiler"):
            path = self.root / name
            path.write_bytes(b"original")
            frozen = {path: profile.fingerprint(path)}
            path.write_bytes(b"modified")
            with self.subTest(name=name), self.assertRaisesRegex(profile.AllocationError, "changed"):
                profile.unchanged(frozen)

    def test_intentional_format_error_is_valid_only_for_exact_expected_error(self):
        (self.directory / "formatted.ax").unlink()
        message = b"error[E_FORMAT_LIMIT]: formatted output exceeds budget\n"
        (self.directory / "diagnostics.txt").write_bytes(message)
        self.report.update(outcome="format_error", diagnostic_codes=["E_FORMAT_LIMIT"],
                           artifacts=[{"path": "diagnostics.txt", "bytes": len(message)}])
        self.expected.update(outcome="format_error", exit=1, codes=["E_FORMAT_LIMIT"],
                             artifacts={"diagnostics.txt": {"contains": "error[E_FORMAT_LIMIT]"}})
        self.process["returncode"] = 1
        self.write()
        self.validate()
        self.report["diagnostic_codes"] = ["E_PARSE_TYPE"]
        self.write()
        with self.assertRaisesRegex(profile.AllocationError, "diagnostic"):
            self.validate()

    def test_existing_output_directory_is_never_reused(self):
        with mock.patch.object(profile, "profile") as implementation, mock.patch("sys.stderr", new=io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                profile.main(["--compiler", sys.executable, "--profiler", sys.executable,
                              "--output-dir", str(self.directory)])
        self.assertEqual(error.exception.code, 2)
        implementation.assert_not_called()
        self.assertTrue((self.directory / "report.json").exists())

    def test_raw_variation_summaries_exclude_warmup_and_preserve_signed_changes(self):
        samples = []
        for workload in profile.WORKLOADS:
            for kind, number in (("warmup", 99), ("measured", 1), ("measured", 3), ("measured", 5)):
                item = phase("load")
                item["events"]["alloc_attempts"] = number
                item["net_live_requested_bytes"] = -number
                samples.append({"workload": workload, "round_kind": kind, "valid": True,
                                "allocation_report": {"phases": [item]}})
        summary = profile.summarize_samples(samples)
        for workload in profile.WORKLOADS:
            self.assertEqual(summary[workload]["load"]["events"]["alloc_attempts"]["median"], 3)
            self.assertEqual(summary[workload]["load"]["net_live_requested_bytes_raw"], [-1, -3, -5])

    def test_failed_report_publication_preserves_previous_receipt(self):
        profile.save_report(self.root, {"status": "running"})
        previous = (self.root / "allocations.json").read_bytes()
        with mock.patch.object(profile.json, "dump", side_effect=OSError("injected write failure")):
            with self.assertRaises(OSError):
                profile.save_report(self.root, {"status": "passed"})
        self.assertEqual((self.root / "allocations.json").read_bytes(), previous)
        self.assertTrue((self.root / "allocations.json.tmp").exists())


if __name__ == "__main__":
    unittest.main()

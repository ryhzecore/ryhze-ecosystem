#!/usr/bin/env python3
"""Standard-library tests for the compiler profiler; no Axeon build is needed."""
import contextlib
import hashlib
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import signal
import sys
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "axeon_compiler_profiler", Path(__file__).with_name("profile-compiler.py"))
PROFILER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROFILER)


class SummaryTests(unittest.TestCase):
    def test_inclusive_quartiles_variance_and_raw_order_independence(self):
        result = PROFILER.summarize([5, 1, 4, 2, 3])
        self.assertEqual(result["n"], 5)
        self.assertEqual(result["median"], 3)
        self.assertEqual(result["q1"], 2)
        self.assertEqual(result["q3"], 4)
        self.assertEqual(result["iqr"], 2)
        self.assertEqual(result["population_variance"], 2)
        self.assertAlmostEqual(result["population_standard_deviation"], math.sqrt(2))
        self.assertAlmostEqual(result["coefficient_of_variation"], math.sqrt(2) / 3)

    def test_single_zero_and_even_samples_have_defined_statistics(self):
        single = PROFILER.summarize([0])
        self.assertEqual(single["median"], 0)
        self.assertEqual(single["population_variance"], 0)
        self.assertIsNone(single["coefficient_of_variation"])
        pair = PROFILER.summarize([2, 4])
        self.assertEqual(pair["median"], 3)
        self.assertEqual(pair["q1"], 2.5)
        self.assertEqual(pair["q3"], 3.5)
        for invalid in ([], [-1], [math.inf], [math.nan]):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                PROFILER.summarize(invalid)

    def test_fixed_workload_shape_and_independent_arithmetic_result(self):
        sources, expected = PROFILER.generated_sources()
        self.assertEqual(sources, PROFILER.generated_sources()[0])
        arithmetic = sources["arithmetic.ax"]
        rejection = sources["format-budget.ax"]
        self.assertEqual(arithmetic.count(b"fn stage_"), 128)
        self.assertEqual(arithmetic.count(b"x = (x * 3 + 7) % 100003;"), 4096)
        self.assertEqual(rejection.count(b"//\n"), 300_000)
        self.assertEqual(rejection.count(b"{"), 51)
        self.assertEqual(rejection.count(b"}"), 51)
        self.assertGreater(len(rejection), 900_000)
        self.assertLess(len(rejection), 1_048_576)
        self.assertLess(len(arithmetic), 1_048_576)
        # Solve the recurrence independently: 2*x_n + 7 = 3^n * 9 (mod p).
        answer = ((pow(3, 4096, 100003) * 9 - 7) * pow(2, -1, 100003)) % 100003
        self.assertEqual(expected, f"{answer}\nresult: {answer}\n".encode("ascii"))


@unittest.skipUnless(sys.platform == "linux" and hasattr(os, "wait4"), "Linux wait4 required")
class ChildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="axeon-profiler-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def run_python(self, script, timeout=3):
        return PROFILER.run_child(
            [sys.executable, "-c", script], self.root,
            self.root / "out", self.root / "err", timeout)

    def test_regular_file_capture_and_individual_wait4_accounting(self):
        sample = self.run_python("import sys; print('checked'); sys.stderr.write('diagnostic\\n')")
        self.assertEqual(sample["returncode"], 0)
        self.assertFalse(sample["timed_out"])
        self.assertEqual((self.root / "out").read_bytes(), b"checked\n")
        self.assertEqual(sample["stdout"]["sha256"], hashlib.sha256(b"checked\n").hexdigest())
        self.assertEqual(sample["stderr"]["bytes"], len(b"diagnostic\n"))
        self.assertGreater(sample["wall_seconds"], 0)
        self.assertGreater(sample["max_rss_kib"], 0)
        self.assertGreaterEqual(sample["user_cpu_seconds"], 0)
        self.assertGreaterEqual(sample["system_cpu_seconds"], 0)
        PROFILER.require_result(sample, 0, sample["stdout"]["sha256"],
                                stderr_contains=b"diagnostic")

    def test_timeout_is_reaped_recorded_and_rejected(self):
        sample = self.run_python("import time; time.sleep(30)", timeout=0.1)
        self.assertTrue(sample["timed_out"])
        self.assertEqual(sample["returncode"], -signal.SIGKILL)
        self.assertLess(sample["wall_seconds"], 5)
        with self.assertRaisesRegex(PROFILER.ProfileError, "deadline"):
            PROFILER.require_result(sample, -signal.SIGKILL)

    def test_nonzero_status_and_wrong_checksum_fail_validation(self):
        sample = self.run_python("print('wrong result'); raise SystemExit(3)")
        with self.assertRaisesRegex(PROFILER.ProfileError, "expected exit"):
            PROFILER.require_result(sample, 0)
        with self.assertRaisesRegex(PROFILER.ProfileError, "checksum"):
            PROFILER.require_result(sample, 3, PROFILER.EMPTY_SHA256)

    def test_rejection_requires_exact_exit_empty_stdout_and_expected_diagnostic(self):
        sample = self.run_python(
            "import sys; sys.stderr.write('source:1:1: error[E_FORMAT_LIMIT]: limit\\n');"
            " raise SystemExit(1)")
        PROFILER.require_result(sample, 1, PROFILER.EMPTY_SHA256,
                                stderr_contains=b"error[E_FORMAT_LIMIT]")
        with self.assertRaisesRegex(PROFILER.ProfileError, "diagnostic"):
            PROFILER.require_result(sample, 1, stderr_contains=b"error[OTHER]")
        with self.assertRaisesRegex(PROFILER.ProfileError, "stderr"):
            PROFILER.require_result(sample, 1, stderr_empty=True)
        changed = dict(sample)
        changed["artifact"] = {"sha256": "new digest"}
        self.assertNotEqual(PROFILER.output_signature(sample), PROFILER.output_signature(changed))

    def test_existing_capture_is_not_overwritten(self):
        (self.root / "out").write_bytes(b"preserve")
        with self.assertRaises(FileExistsError):
            self.run_python("print('not run')")
        self.assertEqual((self.root / "out").read_bytes(), b"preserve")

    def test_existing_evidence_directory_is_rejected_without_execution(self):
        (self.root / "keep").write_text("existing evidence")
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            PROFILER.main(["--compiler", sys.executable, "--output-dir", str(self.root)])
        self.assertEqual(error.exception.code, 2)
        self.assertEqual(sorted(path.name for path in self.root.iterdir()), ["keep"])

    def test_invalid_identity_saves_partial_failure_without_accepting_statistics(self):
        # Python is intentionally the wrong executable, not an Axeon compiler.
        with contextlib.redirect_stdout(io.StringIO()):
            result = PROFILER.profile(Path(sys.executable).resolve(), self.root, 1,
                                      "invalid identity test", 3)
        self.assertEqual(result, 1)
        report = json.loads((self.root / "profile.json").read_text())
        self.assertEqual(report["status"], "failed")
        self.assertIn("identity", report["failure"])
        self.assertFalse(report["preparation"][0]["valid"])
        self.assertEqual(report["summaries"], {})
        self.assertEqual(report["samples"], [])


if __name__ == "__main__":
    unittest.main()

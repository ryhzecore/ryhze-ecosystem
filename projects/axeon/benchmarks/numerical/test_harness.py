"""Small correctness/protocol tests only; never time a workload."""
import json
import math
import tempfile
import unittest
from pathlib import Path

import kernels
import run


class NumericalHarnessTests(unittest.TestCase):
    def test_observer_uses_signed_bits_and_canonical_nan(self):
        self.assertEqual(kernels.bits(0.0), 0)
        self.assertEqual(kernels.bits(-0.0), -(1 << 63))
        self.assertEqual(kernels.bits(1.0), 4607182418800017408)
        self.assertEqual(kernels.bits(float("nan")), 0x7ff8000000000000)

    def test_ordered_implementations_match_separate_reference(self):
        self.assertEqual(kernels.scalar(0), kernels.bits(.625))
        self.assertEqual(kernels.matrix(0), kernels.bits(-.25))
        for n in (0, 1, 2, 31, 97):
            self.assertEqual(kernels.scalar(n), kernels.independent_reference("scalar", n)["checksum"])
            self.assertEqual(kernels.matrix(n), kernels.independent_reference("matrix", n)["checksum"])
            for workload in ("scalar", "matrix"):
                self.assertTrue(all(math.isfinite(x) for x in kernels.independent_reference(workload, n)["state"]))

    def test_library_validation_reports_tolerance_and_rejects_wrong_vectors(self):
        reference = kernels.independent_reference("matrix", 1)
        data = {"n": 1, "cold_ns": 1, "warmup_ns": 2, "warm_ns": [3]*7,
                "proc_image_peak_rss_kib": 2048, "checksums": [reference["checksum"]]*9,
                "states": [list(reference["state"]) for _ in range(9)]}
        validated = run.parse_library_output(json.dumps(data), 1, reference)
        self.assertEqual(validated["numerical_validation"]["max_abs_error"], 0)
        data["states"][0][0] += 1e-14
        checksum = 0.0
        for value in data["states"][0]:
            checksum = checksum + value
        data["checksums"][0] = kernels.bits(checksum)
        validated = run.parse_library_output(json.dumps(data), 1, reference)
        self.assertGreater(validated["numerical_validation"]["max_abs_error"], 0)
        self.assertFalse(validated["numerical_validation"]["all_checksums_happen_to_match_exactly"])
        data["states"][0][0] += .01
        with self.assertRaisesRegex(ValueError, "tolerance"):
            run.parse_library_output(json.dumps(data), 1, reference)
        data["states"][0][0] = float("nan")
        with self.assertRaisesRegex(ValueError, "finite"):
            run.parse_library_output(json.dumps(data), 1, reference)

    def test_library_checksum_must_observe_reported_values(self):
        reference = kernels.independent_reference("matrix", 0)
        data = {"n": 0, "cold_ns": 1, "warmup_ns": 2, "warm_ns": [3]*7,
                "proc_image_peak_rss_kib": 2048, "checksums": [0]*9,
                "states": [reference["state"]]*9}
        with self.assertRaisesRegex(ValueError, "checksum"):
            run.parse_library_output(json.dumps(data), 0, reference)

    def test_operator_audit_accepts_attributes_and_rejects_fast_math_and_fma(self):
        ir = "define dso_local i64 @kernel(i64 noundef %n) local_unnamed_addr #0 {\n %x = fmul double 1.0, 2.0\n ret i64 0\n}\n"
        assembly = "kernel:\n.Lloop:\n mulsd %xmm0, %xmm1\n jne .Lloop\n .size kernel, .-kernel\n"
        counts = run.operator_counts(ir, assembly, "kernel")
        self.assertEqual(counts["llvm_counts"]["fmul"], 1)
        self.assertEqual(counts["assembly_fp_opcode_counts"], {"mulsd": 1})
        with self.assertRaisesRegex(ValueError, "fast-math"):
            run.operator_counts(ir.replace("fmul double", "fmul fast double"), assembly, "kernel")
        with self.assertRaisesRegex(ValueError, "fused"):
            run.operator_counts(ir, assembly.replace("mulsd", "vfmadd132sd"), "kernel")

    def test_measure_requires_complete_profile_and_unchanged_workloads(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "profiles.json"
            receipt = {"stage": "profile", "status": "passed", "workload_hashes": run.workload_hashes(),
                       "profiles": [{"workload": workload, "language": language, "opt": opt}
                                    for workload in run.WORKLOADS for language in ("Axeon", "C++")
                                    for opt in run.OPTIMIZATIONS]}
            path.write_text(json.dumps(receipt))
            self.assertEqual(run.validate_profile(path), receipt)
            receipt["workload_hashes"]["unexpected"] = "changed"
            path.write_text(json.dumps(receipt))
            with self.assertRaisesRegex(ValueError, "changed"):
                run.validate_profile(path)
            receipt["workload_hashes"] = run.workload_hashes()
            receipt["profiles"].pop()
            path.write_text(json.dumps(receipt))
            with self.assertRaisesRegex(ValueError, "O0/O2/O3"):
                run.validate_profile(path)


if __name__ == "__main__":
    unittest.main()

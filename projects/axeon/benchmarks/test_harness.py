"""Fast protocol tests only; does not execute a measured workload."""
import unittest
import json

from run import adapt_llvm, cluster_bootstrap, percentile, summary, speedup_bootstrap, parse_process_output


class HarnessTests(unittest.TestCase):
    def test_ir_adaptation_changes_only_export_and_hosted_main(self):
        body = "entry:\n  %x = add i64 %arg0, 1\n  ret i64 %x\n}\n"
        original = "define internal i64 @ax_fn_0(i64 %arg0) {\n" + body
        original += "define internal i64 @ax_fn_1() {\n  %x = call i64 @ax_fn_0(i64 5)\n  ret i64 %x\n}\n"
        original += "define i32 @main() {\n  ret i32 0\n}\n"
        adapted = adapt_llvm(original)
        self.assertIn("define i64 @axeon_bench_kernel(i64 %arg0) {\n" + body, adapted)
        self.assertIn("define internal i64 @ax_fn_1()", adapted)
        self.assertIn("call i64 @axeon_bench_kernel(i64 5)", adapted)
        self.assertNotIn("@main", adapted)
        self.assertNotIn("@ax_fn_0", adapted)

    def test_ir_adaptation_rejects_unexpected_signatures(self):
        for text in ["", "define i32 @main() {\n ret i32 0\n}\n",
                     "define internal i64 @ax_fn_0() {\n ret i64 0\n}\n"]:
            with self.assertRaises(ValueError):
                adapt_llvm(text)

    def test_statistics_and_cluster_bootstrap_are_deterministic(self):
        values = [1, 2, 3, 4, 5]
        self.assertEqual(summary(values), {"count": 5, "median": 3, "q1": 2,
                         "q3": 4, "iqr": 2, "sample_variance": 2.5, "sample_stddev": 2.5 ** 0.5,
                         "min": 1, "max": 5})
        self.assertEqual(percentile([1, 3], .25), 1.5)
        self.assertEqual(cluster_bootstrap([[10] * 7] * 3, 99), [10, 10])
        self.assertEqual(speedup_bootstrap([[20] * 7] * 3, [[10] * 7] * 3, 99), [2, 2])
        self.assertEqual(cluster_bootstrap([[1], [5], [12]], 123),
                         cluster_bootstrap([[1], [5], [12]], 123))

    def test_memory_protocol_requires_positive_post_exec_image_peak(self):
        valid = {"n": 7, "cold_ns": 1, "warmup_ns": 2, "warm_ns": [3] * 7,
                 "checksums": [123] * 9, "proc_image_peak_rss_kib": 2048}
        self.assertEqual(parse_process_output(json.dumps(valid), 7, 123), valid)
        for missing_or_invalid in [None, 0, -1, False, "2048"]:
            data = dict(valid, proc_image_peak_rss_kib=missing_or_invalid)
            if missing_or_invalid is None:
                del data["proc_image_peak_rss_kib"]
            with self.assertRaisesRegex(ValueError, "VmHWM"):
                parse_process_output(json.dumps(data), 7, 123)
        with self.assertRaisesRegex(ValueError, "checksum"):
            parse_process_output(json.dumps(valid), 7, 999)


if __name__ == "__main__":
    unittest.main()

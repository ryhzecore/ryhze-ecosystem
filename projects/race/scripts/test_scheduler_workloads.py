"""Exercise workload results and reporting, never assert a performance threshold.

Run against a chosen Release or sanitizer executable with --binary PATH.
The small fixed checksums were evaluated independently with a recursive DAG
reference and uint64 modular arithmetic. They are not learned from the executable
under test. An intentional kernel/graph change requires reviewed fixture changes.
"""
import argparse
import json
import math
import pathlib
import subprocess
import unittest


EXPECTED = {
    "integer": {
        "independent": 15831541669178817500,
        "chain": 17308851091161006359,
        "fork_join": 2319928981772111929,
        "conflict": 15831541669178817500,
    },
    "buffer": {
        "independent": 15257772021033567572,
        "chain": 13331365962949249668,
        "fork_join": 12045915505288088393,
        "conflict": 15257772021033567572,
    },
}
BASE = {
    "kernel": "integer", "pattern": "independent", "mode": "paired",
    "work": 7, "jobs": 43, "batch": 20, "capacity": 37, "workers": 4,
    "warmups": 1, "trials": 2, "latency": 0,
}
LATENCY_FIELDS = (
    "batch_marker_to_callback_start_ns", "callback_duration_ns",
    "batch_marker_to_callback_finish_ns",
)
PHASE_FIELDS = (
    "setup_ns", "group_create_ns", "admission_ns", "close_ns", "wait_ns",
    "dispatch_wait_ns", "verification_ns", "release_ns", "group_cycle_ns",
    "total_ns",
)


def summary_samples(trials, mode, metric, jobs):
    # Summaries use unrounded rates. Reconstruct from exact integer durations;
    # JSON-rounded trial rates can distort a small population standard deviation.
    duration = {"jobs_per_second": "group_cycle_ns",
                "dispatch_jobs_per_second": "dispatch_wait_ns"}.get(metric)
    if duration is not None:
        return [jobs * 1e9 / trial[mode][duration] for trial in trials]
    return [trial[mode][metric] for trial in trials]


class SchedulerWorkloadTests(unittest.TestCase):
    binary = None
    process_timeout = 90

    def invoke(self, arguments):
        return subprocess.run(
            [str(self.binary), *arguments], capture_output=True, text=True,
            timeout=self.process_timeout, check=False,
        )

    @staticmethod
    def arguments(options):
        return [part for key, value in options.items()
                for part in ("--" + key, str(value))]

    def assert_near(self, actual, expected):
        self.assertTrue(math.isfinite(actual), actual)
        self.assertTrue(math.isclose(actual, expected, rel_tol=1e-9, abs_tol=1e-8),
                        (actual, expected))

    def assert_distribution(self, distribution, count, values=None):
        self.assertEqual(distribution["count"], count)
        ordered = [distribution[key] for key in ("min", "p50", "p95", "p99", "max")]
        self.assertEqual(ordered, sorted(ordered))
        for value in (*ordered, distribution["mean"], distribution["population_stddev"]):
            self.assertTrue(math.isfinite(value) and value >= 0, distribution)
        self.assertLessEqual(distribution["min"], distribution["mean"])
        self.assertLessEqual(distribution["mean"], distribution["max"])
        if values is not None:
            self.assertEqual(len(values), count)
            ordered = sorted(values)
            expected = {
                "min": ordered[0], "max": ordered[-1],
                "p50": ordered[math.ceil(count * 0.50) - 1],
                "p95": ordered[math.ceil(count * 0.95) - 1],
                "p99": ordered[math.ceil(count * 0.99) - 1],
                "mean": sum(values) / count,
            }
            expected["population_stddev"] = math.sqrt(
                sum((value - expected["mean"]) ** 2 for value in values) / count)
            for key, value in expected.items():
                self.assert_near(distribution[key], value)

    def assert_trial(self, trial, options, mode):
        for key in PHASE_FIELDS:
            self.assertIsInstance(trial[key], int)
            self.assertGreaterEqual(trial[key], 0)
        self.assertGreater(trial["dispatch_wait_ns"], 0)
        self.assertEqual(trial["group_cycle_ns"],
                         trial["group_create_ns"] + trial["dispatch_wait_ns"] + trial["release_ns"])
        self.assertGreaterEqual(trial["dispatch_wait_ns"],
                                trial["admission_ns"] + trial["close_ns"] + trial["wait_ns"])
        self.assertGreaterEqual(trial["total_ns"],
                                trial["setup_ns"] + trial["group_cycle_ns"] + trial["verification_ns"])
        self.assert_near(trial["jobs_per_second"], options["jobs"] * 1e9 / trial["group_cycle_ns"])
        self.assert_near(trial["dispatch_jobs_per_second"],
                         options["jobs"] * 1e9 / trial["dispatch_wait_ns"])
        self.assertEqual(trial["verified_callbacks"], options["jobs"])
        self.assertEqual(trial["output_checksum"], EXPECTED[options["kernel"]][options["pattern"]])
        self.assert_memory_observation(trial)
        if mode == "inline":
            for key in ("group_create_ns", "admission_ns", "close_ns", "wait_ns", "release_ns"):
                self.assertEqual(trial[key], 0)
        else:
            self.assertGreater(trial["release_ns"], 0)
        for field in LATENCY_FIELDS:
            if options["latency"]:
                self.assert_distribution(trial[field], options["jobs"])
            else:
                self.assertNotIn(field, trial)

    def assert_memory_observation(self, record, prefix="process_"):
        resident, peak = (record[prefix + key] for key in ("resident_bytes", "peak_rss_bytes"))
        self.assertIsInstance(resident, int)
        self.assertIsInstance(peak, int)
        self.assertGreater(resident, 0)
        self.assertGreater(peak, 0)
        self.assertIs(record[prefix + "memory_counters_coherent"], peak >= resident)

    def test_raw_memory_counter_coherence_is_explicit(self):
        for resident, peak, coherent in ((4358144, 4362240, True), (4362240, 4358144, False)):
            record = {"process_resident_bytes": resident, "process_peak_rss_bytes": peak,
                      "process_memory_counters_coherent": coherent}
            original = record.copy()
            self.assert_memory_observation(record)
            self.assertEqual(record, original)
            record["process_memory_counters_coherent"] = not coherent
            with self.assertRaises(AssertionError):
                self.assert_memory_observation(record)

    def run_case(self, **changes):
        options = BASE | changes
        result = self.invoke(self.arguments(options))
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(result.stderr, "")
        self.assertEqual(len(result.stdout.splitlines()), 1, result.stdout)
        record = json.loads(result.stdout)
        self.assertEqual(record["schema"], 1)
        self.assertEqual(record["kind"], "race-scheduler-workloads")
        self.assertRegex(record["source_commit"], r"^[0-9a-f]{40}$")
        self.assertRegex(record["authored_input_sha256"], r"^[0-9a-f]{64}$")
        self.assertTrue(record["configuration"])
        self.assertIn(record["sanitizer"], ("none", "address", "thread"))
        self.assertIs(record["all_outputs_verified"], True)
        self.assert_memory_observation(record, "final_process_")
        for key in ("kernel", "pattern", "mode", "work", "jobs", "batch", "capacity", "workers", "warmups"):
            self.assertEqual(record[key], options[key])
        self.assertEqual(record["measured_trials"], options["trials"])
        self.assertEqual(record["latency_enabled"], bool(options["latency"]))
        payload = options["batch"] * options["work"] * 8 if options["kernel"] == "buffer" else 0
        self.assertEqual(record["active_buffer_payload_bytes"], payload)
        modes = ("inline", "scheduled") if options["mode"] == "paired" else (options["mode"],)
        self.assertEqual(len(record["trials"]), options["warmups"] + options["trials"])
        for index, trial in enumerate(record["trials"]):
            self.assertEqual(trial["index"], index)
            self.assertEqual(trial["warmup"], index < options["warmups"])
            first = ("inline", "scheduled")[index % 2] if options["mode"] == "paired" else options["mode"]
            self.assertEqual(trial["first_mode"], first)
            for mode in ("inline", "scheduled"):
                if mode in modes:
                    self.assert_trial(trial[mode], options, mode)
                else:
                    self.assertNotIn(mode, trial)
        measured = record["trials"][options["warmups"]:]
        for mode in ("inline", "scheduled"):
            key = mode + "_summary"
            if mode not in modes:
                self.assertNotIn(key, record)
                continue
            for metric in ("dispatch_wait_ns", "group_cycle_ns", "dispatch_jobs_per_second",
                           "jobs_per_second", "total_ns"):
                self.assert_distribution(record[key][metric], options["trials"],
                                         summary_samples(measured, mode, metric, options["jobs"]))
        stats = record["scheduler_stats"]
        scheduled_jobs = options["jobs"] * (options["warmups"] + options["trials"]) if "scheduled" in modes else 0
        self.assertEqual(stats["admitted"], scheduled_jobs)
        self.assertEqual(stats["completed"], scheduled_jobs)
        for key in ("occupied", "pending", "running"):
            self.assertEqual(stats[key], 0)
        self.assertLessEqual(stats["peak_pending_including_warmups"], options["batch"])
        if options["mode"] == "paired":
            self.assert_distribution(record["paired_speedup"], options["trials"],
                                     [trial["inline"]["group_cycle_ns"] / trial["scheduled"]["group_cycle_ns"]
                                      for trial in measured])
            self.assert_distribution(record["paired_dispatch_speedup"], options["trials"],
                                     [trial["inline"]["dispatch_wait_ns"] / trial["scheduled"]["dispatch_wait_ns"]
                                      for trial in measured])
        else:
            self.assertNotIn("paired_speedup", record)
            self.assertNotIn("paired_dispatch_speedup", record)
        return record

    def test_rate_summary_uses_exact_duration_samples(self):
        # Fixed 43-job samples at 1,000,000 and 1,000,001 ns. Independently
        # serialized 12-digit rates lose detail needed for their small spread.
        expected = {"count": 2, "min": 42999.9570000, "p50": 42999.9570000,
                    "p95": 43000.0, "p99": 43000.0, "max": 43000.0,
                    "mean": 42999.9785000, "population_stddev": 0.0214999784985}
        for metric, duration in (("jobs_per_second", "group_cycle_ns"),
                                 ("dispatch_jobs_per_second", "dispatch_wait_ns")):
            with self.subTest(metric=metric):
                trials = [{"inline": {duration: 1000000, metric: 43000.0}},
                          {"inline": {duration: 1000001, metric: 42999.9570000}}]
                # Confirm the old rounded-rate oracle rejects this valid fixture.
                with self.assertRaises(AssertionError):
                    self.assert_distribution(expected, 2, [trial["inline"][metric] for trial in trials])
                self.assert_distribution(expected, 2,
                                         summary_samples(trials, "inline", metric, 43))

    def test_integer_workload_patterns(self):
        for pattern in EXPECTED["integer"]:
            with self.subTest(pattern=pattern):
                self.run_case(pattern=pattern, workers=1 if pattern == "chain" else 4)

    def test_buffer_workload_patterns(self):
        for pattern in EXPECTED["buffer"]:
            with self.subTest(pattern=pattern):
                self.run_case(kernel="buffer", pattern=pattern, workers=1 if pattern == "chain" else 4)

    def test_serial_and_parallel_dependency_results_match(self):
        # Batch 20 includes a 16-leaf diamond and two-node tail; jobs 43 adds
        # a final three-node diamond. This exercises maximum fan-in and tails.
        for workers, capacity in ((1, 20), (3, 47)):
            with self.subTest(workers=workers, capacity=capacity):
                self.run_case(pattern="fork_join", workers=workers, capacity=capacity,
                              warmups=0, trials=1)

    def test_standalone_modes_keep_reference_results(self):
        for mode in ("inline", "scheduled"):
            with self.subTest(mode=mode):
                self.run_case(kernel="buffer", pattern="chain", mode=mode,
                              workers=1, warmups=0, trials=1)

    def test_latency_instrumentation_keeps_reference_results(self):
        for kernel in ("integer", "buffer"):
            with self.subTest(kernel=kernel):
                self.run_case(kernel=kernel, pattern="fork_join", latency=1)

    def test_invalid_arguments_are_rejected_before_work(self):
        invalid = (
            ["--unknown", "1"], ["--work"], ["--work", "1", "--work", "2"],
            ["--work", "-1"], ["--jobs", "abc"], ["--kernel", "gpu"],
            ["--pattern", "cycle"], ["--mode", "neither"], ["--work", "0"],
            ["--jobs", "0"], ["--batch", "0"], ["--capacity", "1"],
            ["--workers", "0"], ["--workers", "257"], ["--trials", "0"],
            ["--trials", "101"], ["--warmups", "101"], ["--latency", "2"],
            ["--pattern", "fork_join", "--batch", "2"],
            ["--kernel", "buffer", "--work", "16777216", "--jobs", "5",
             "--batch", "5", "--capacity", "5"],
        )
        for arguments in invalid:
            with self.subTest(arguments=arguments):
                result = self.invoke(arguments)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
                self.assertIn("scheduler workload failed:", result.stderr)

    def test_help_does_not_execute_a_workload(self):
        result = self.invoke(["--help"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--kernel integer|buffer", result.stdout)
        self.assertNotIn('"all_outputs_verified"', result.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=pathlib.Path)
    parser.add_argument("--process-timeout", type=float, default=90)
    options, remaining = parser.parse_known_args()
    if not options.binary.is_file():
        parser.error("--binary must name an existing workload executable")
    if options.process_timeout <= 0:
        parser.error("--process-timeout must be positive")
    SchedulerWorkloadTests.binary = options.binary.resolve()
    SchedulerWorkloadTests.process_timeout = options.process_timeout
    unittest.main(argv=[__file__, *remaining], verbosity=2)

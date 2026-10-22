#!/usr/bin/env python3
"""Run a reproducible, checked scheduler matrix and retain raw cloud evidence.

This is a synthetic workload study, not a production performance guarantee.
Run after validation, without concurrent builds, in the recorded Linux image.
"""
from __future__ import annotations
from output_paths import evidence_directory

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import random
import signal
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from source_manifest import ROOT, file_sha256, manifest, write_json
from validate import compile_configuration, hardware


def cases() -> list[dict]:
    result = []
    seen = set()

    def add(category, *, kernel="integer", work=64, jobs=256, batch=64,
            capacity=None, workers=4, pattern="independent", latency=0):
        config = dict(kernel=kernel, work=work, jobs=jobs, batch=batch,
                      capacity=batch if capacity is None else capacity,
                      workers=workers, pattern=pattern, latency=latency)
        key = tuple(config.items())
        if key in seen:
            return
        seen.add(key)
        identity = hashlib.sha256(json.dumps(config, sort_keys=True).encode()).hexdigest()[:10]
        result.append(dict(id=f"{category}-{identity}", category=category, options=config))

    for work in (64, 1024, 16384, 262144, 1048576):
        for workers in (1, 2, 4, 5, 8):
            add("integer-size", work=work, workers=workers)
    for work in (256, 4096, 65536, 262144):
        for workers in (1, 2, 4, 5):
            add("buffer-size", kernel="buffer", work=work, batch=32, workers=workers)
    for work in (64, 16384):
        for batch in (1, 8, 32, 128, 256):
            add("batch", work=work, batch=batch, jobs=512)
        for capacity in (32, 128, 512, 2048):
            add("capacity", work=work, batch=32, capacity=capacity)
    for work in (64, 16384, 262144):
        for workers in (1, 4):
            for pattern in ("chain", "fork_join", "conflict"):
                add("dependency", work=work, workers=workers, pattern=pattern)
    for kernel, work, pattern, batch in (
        ("integer", 64, "independent", 64),
        ("integer", 16384, "independent", 64),
        ("integer", 262144, "independent", 64),
        ("integer", 16384, "chain", 64),
        ("integer", 16384, "fork_join", 64),
        ("buffer", 65536, "independent", 32),
    ):
        add("latency", kernel=kernel, work=work, pattern=pattern, batch=batch, latency=1)
    random.Random(0x52414345).shuffle(result)
    return result


def optional_text(path):
    try:
        return Path(path).read_text().strip()
    except OSError as error:
        return {"unavailable": type(error).__name__, "errno": error.errno}


def environment():
    status = Path("/proc/self/status").read_text().splitlines()
    return {
        "hardware": hardware(), "uname": list(os.uname()), "uid": os.getuid(),
        "security": [line for line in status if line.startswith(("Cap", "NoNewPrivs", "Seccomp"))],
        "cgroup": optional_text("/proc/self/cgroup"),
        "visible_cgroup_controls": {key: optional_text("/sys/fs/cgroup/" + key) for key in
                                    ("cpu.max", "cpu.stat", "cpuset.cpus.effective", "memory.max", "pids.max")},
        "limitations": ["This matrix targets the recorded five-vCPU KVM cloud; re-evaluate topology assumptions on other hosts",
                        "Host contention and power/frequency policy uncontrolled",
                        "Visible cgroup limits may not expose ancestor limits",
                        "Eight workers oversubscribe the five visible vCPUs",
                        "No cloud/Deck timing equivalence is asserted"],
    }


def run_case(binary, case, output, warmups, trials, timeout):
    prefix = output / case["id"]
    command = [str(binary), "--mode", "paired", "--warmups", str(warmups), "--trials", str(trials)]
    for key, value in case["options"].items():
        command.extend(["--" + key, str(value)])
    # GNU time measures the complete child process, including oracle/setup and
    # both modes; those CPU/RSS observations are not scheduler-only counters.
    wrapped = ["/usr/bin/time", "-f", '{"user_seconds":%U,"system_seconds":%S,"wall_seconds":%e,"max_rss_kib":%M,"voluntary_context_switches":%w,"involuntary_context_switches":%c,"exit_status":%x}',
               "-o", str(prefix.with_suffix(".resource.json")), *command]
    started = time.monotonic()
    record = dict(case, command=wrapped, started_utc=datetime.now(timezone.utc).isoformat())
    with prefix.with_suffix(".json").open("w") as stdout, prefix.with_suffix(".stderr.log").open("w") as stderr:
        process = subprocess.Popen(wrapped, cwd=ROOT, stdout=stdout, stderr=stderr, start_new_session=True)
        try:
            record["exit_code"] = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            record.update(exit_code=process.returncode, timed_out=True)
    record["elapsed_seconds"] = time.monotonic() - started
    record["accepted"] = False
    if record["exit_code"] == 0:
        try:
            row = json.loads(prefix.with_suffix(".json").read_text())
            record["accepted"] = (row["schema"] == 1 and row["kind"] == "race-scheduler-workloads"
                                  and row["all_outputs_verified"] is True and row["measured_trials"] == trials
                                  and row["warmups"] == warmups and row["mode"] == "paired")
            record["accepted"] &= all(row["latency_enabled"] == bool(value) if key == "latency" else row[key] == value
                                      for key, value in case["options"].items())
            record["embedded_identity"] = {key: row[key] for key in
                                           ("source_commit", "authored_input_sha256", "configuration", "sanitizer")}
            record["group_cycle_speedup"] = row["paired_group_cycle_speedup"]
            record["scheduled_jobs_per_second"] = row["scheduled_summary"]["jobs_per_second"]
            record["inline_ns_per_job_median"] = row["inline_summary"]["group_cycle_ns"]["p50"] / row["jobs"]
            record["process_peak_rss_bytes"] = row["final_process_peak_rss_bytes"]
        except (ValueError, KeyError, TypeError, ZeroDivisionError) as error:
            record.update(accepted=False, output_error=type(error).__name__)
    write_json(prefix.with_suffix(".run.json"), record)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "out/release/race_scheduler_workloads")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--trials", type=int, default=9)
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--category", action="append", help="Select matrix categories; default all")
    parser.add_argument("--list", action="store_true", help="Print the fixed matrix without executing")
    args = parser.parse_args()
    selected = [case for case in cases() if not args.category or case["category"] in args.category]
    if not selected:
        parser.error("selection contains no cases")
    if args.list:
        print(json.dumps(selected, indent=2))
        return 0
    if args.warmups < 2 or args.trials < 9 or args.timeout < 1:
        parser.error("provide at least two warmups/nine trials and a positive timeout")
    output = evidence_directory("measure-scheduler", args.output, allow_empty=True)
    binary = args.binary.resolve(strict=True)
    before = manifest()
    if not before["git"]["working_tree_clean"]:
        parser.error("commit or isolate tracked/untracked changes before measurement")
    receipt = {"schema": 1, "kind": "race-scheduler-matrix-receipt",
               "started_utc": datetime.now(timezone.utc).isoformat(),
               "source": before, "environment_before": environment(),
               "build": compile_configuration(binary.parent),
               "binary": {"path": str(binary), "sha256": file_sha256(binary)},
               "warmups": args.warmups, "trials": args.trials, "order_seed": "0x52414345",
               "matrix": selected, "runs": [], "accepted": False}
    write_json(output / "source-manifest.json", before)
    write_json(output / "receipt.json", receipt)
    for index, case in enumerate(selected):
        result = run_case(binary, case, output, args.warmups, args.trials, args.timeout)
        expected = {"source_commit": before["git"]["commit"],
                    "authored_input_sha256": before["authored_inputs"]["sha256"],
                    "configuration": "Release", "sanitizer": "none"}
        result["accepted"] &= result.get("embedded_identity") == expected
        write_json(output / (case["id"] + ".run.json"), result)
        receipt["runs"].append(result)
        write_json(output / "receipt.json", receipt)
        print(f"{index+1}/{len(selected)} {case['id']}: {'passed' if result['accepted'] else 'FAILED'}", flush=True)
        if not result["accepted"]:
            break
    after = manifest()
    receipt["source_after"] = after
    receipt["environment_after"] = environment()
    receipt["finished_utc"] = datetime.now(timezone.utc).isoformat()
    receipt["accepted"] = (len(receipt["runs"]) == len(selected) and all(row["accepted"] for row in receipt["runs"])
                           and before["git"] == after["git"]
                           and before["authored_inputs"] == after["authored_inputs"]
                           and file_sha256(binary) == receipt["binary"]["sha256"])
    receipt["evidence_files"] = [{"path": path.name, "bytes": path.stat().st_size, "sha256": file_sha256(path)}
                                 for path in sorted(output.iterdir()) if path.name != "receipt.json"]
    write_json(output / "receipt.json", receipt)
    return 0 if receipt["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

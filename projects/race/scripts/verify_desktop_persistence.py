#!/usr/bin/env python3
"""Verify a fixed-work actual Qt persistence profile; no performance threshold."""
from output_paths import evidence_directory
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time

from verify_desktop_performance import percentile, scene
from verify_runtime_desktop import ppm, require


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def statistics(values):
    require(bool(values) and all(math.isfinite(x) and x >= 0 for x in values), "invalid timing samples")
    return {"count": len(values), "p50": percentile(values, .5), "p95": percentile(values, .95),
            "p99": percentile(values, .99), "maximum": max(values),
            "over_16_67_ms": sum(x > 16.67 for x in values)}


def validate(folder, mode, count):
    receipt = json.loads((folder / "file-profile.json").read_text(encoding="utf-8"))
    require(receipt["schema"] == 1 and receipt["passed"] and receipt["workload_verified"], "profile failed")
    require(receipt["file_mode"] == mode and receipt["objects"] == receipt["draw_count"] == count, "workload differs")
    require(receipt["expected_submitted_frames"] == receipt["submitted_frames"] == receipt["completed_frames"] == 600,
            "submission/completion count differs")
    require(receipt["hardware"] and receipt["adapter"] and receipt["gpu_samples_complete"], "hardware GPU evidence missing")
    require(receipt["native_resolution"] and receipt["render_scale"] == 1 and not receipt["frame_generation"] and
            receipt["antialiasing"] == "none" and receipt["present_sync_interval"] == 1, "quality/presentation changed")
    require(not receipt["allocation_counting_available"], "use ordinary Release for this comparison")
    require(receipt["debug_errors"] == 0 and not receipt["errors"], "validation errors")
    require(receipt["saves"] == receipt["loads"] == receipt["inspector_edits"] == 20, "operation counts changed")
    samples, operations, inputs, gpu = (receipt[key] for key in ("samples", "operations", "input_samples", "gpu_samples"))
    require(len(samples) == len(gpu) == 600 and len(operations) == 40 and len(inputs) == 20, "raw sample counts")
    require(Counter(x["kind"] for x in operations) == {"save": 20, "load": 20}, "operation families")
    for index, (row, timed) in enumerate(zip(samples, gpu), 1):
        require(row["frame"] == row["callback"] == row["submitted_frame_id"] == row["last_cpu_frame"] == timed["frame"] == index,
                "frame identities differ")
        require(row["width"] == receipt["width"] and row["height"] == receipt["height"] and row["simulation_tick"] == 0,
                "resolution/simulation changed")
        require(row["phase"] == ("warmup" if index <= 60 else "static"), "phase mismatch")
    for kind, first in (("save", 61), ("load", 73)):
        entries = [x for x in operations if x["kind"] == kind]
        require([x["start_frame"] for x in entries] == [first + 24*n for n in range(20)], "operation admission slots differ")
        require(all(x["passed"] and x["start_frame"] <= x["completion_frame"] < x["start_frame"] + 12 for x in entries),
                "operation did not finish in fixed slot")
        for entry in entries:
            require(all((entry[key] is None) == (mode == "sync") for key in
                        ("until_poll_ms", "publication_and_ui_ms", "snapshot_ms", "worker_ms")), "timing availability mismatch")
    require(all(x["passed"] for x in inputs), "input failed")
    require([x["after_frame"] for x in inputs] == [61 + 24*n for n in range(20)], "queued edits skipped/delayed to another frame")
    require((folder / "roundtrip.scene.json").read_bytes() == (folder / "final.scene.json").read_bytes(), "final Scene differs from saved snapshot")
    frame_stats = {key: statistics([row[key] for row in samples[60:]]) for key in samples[0]
                   if key.endswith("_ms") and key != "gpu_ms"}
    op_stats = {kind: {key: statistics([x[key] for x in operations if x["kind"] == kind]) for key in operations[0]
                       if key.endswith("_ms") and operations[0][key] is not None} for kind in ("save", "load")}
    return {"passed": True, "mode": mode, "source_commit": receipt["source_commit"],
            "authored_input_sha256": receipt["authored_input_sha256"], "receipt_sha256": digest(folder / "file-profile.json"),
            "dimensions": [receipt["width"], receipt["height"]], "camera": receipt["camera_pose"],
            "adapter": receipt["adapter"], "frame_statistics_ms": frame_stats, "operations_ms": op_stats,
            "input_ms": {key: statistics([x[key] for x in inputs]) for key in ("queue_delay_ms", "apply_ms")},
            "gpu_ms": statistics([x["milliseconds"] for x in gpu[60:]]),
            "scene_sha256": digest(folder / "final.scene.json"), "image": ppm(folder / "final-scene.ppm"),
            "percentile_method": "linear interpolation at p*(n-1)",
            "limits": "Fixed Box workflow on this device; queued event delay is not physical input latency. Async publication timing can change intermediate visible states. GPU duration is not actual FPS. No automated performance acceptance threshold."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--mode", choices=("sync", "async"), required=True)
    args = parser.parse_args()
    output = evidence_directory("desktop-persistence", args.output)
    workload = output / "boxes-512.scene.json"
    workload.write_text(json.dumps(scene(512), indent=2) + "\n", encoding="utf-8")
    folder = output / "editor"
    command = [str(args.exe.resolve(strict=True)), "--file-profile", str(folder), "--file-mode", args.mode, "--scene", str(workload)]
    begin = time.perf_counter()
    try:
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=210)
        process = {"command": command, "exit_code": result.returncode, "stdout": result.stdout, "stderr": result.stderr,
                   "elapsed_seconds": time.perf_counter()-begin}
    except subprocess.TimeoutExpired as error:
        (output / "process.json").write_text(json.dumps({"command": command, "timeout": 210, "error": str(error)}, indent=2))
        raise
    (output / "process.json").write_text(json.dumps(process, indent=2), encoding="utf-8")
    require(result.returncode == 0, f"editor profile failed: {result.returncode}")
    summary = validate(folder, args.mode, 512)
    summary.update(executable_sha256=digest(args.exe), workload_sha256=digest(workload))
    (output / "verification.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()

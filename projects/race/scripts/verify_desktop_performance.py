#!/usr/bin/env python3
"""Generate fixed Box workloads and measure the actual SDL/D3D12 runtime."""
from output_paths import evidence_directory
import argparse
from collections import Counter
import csv
import hashlib
import json
import math
from pathlib import Path
import platform
import subprocess
import time

from verify_runtime_desktop import ppm, require


def scene(count):
    columns = {1: 1, 128: 16, 512: 32}[count]
    rows = math.ceil(count / columns)
    zero = "0" * 32
    objects = []
    for index in range(count):
        x = (index % columns - (columns - 1) / 2) * 1.25
        z = (index // columns - (rows - 1) / 2) * 1.25
        objects.append({
            "id": f"52414345424f5801{index + 1:016x}", "section": zero,
            "components": [
                {"type": 1, "version": 1, "fields": [
                    {"id": 1, "kind": "vector", "value": [x, .5, z]},
                    {"id": 2, "kind": "quaternion", "value": [0, 0, 0, 1]},
                    {"id": 3, "kind": "vector", "value": [1, 1, 1]},
                    {"id": 4, "kind": "reference", "value": {"section": zero, "object": zero}}]},
                {"type": 2, "version": 1, "fields": [
                    {"id": 1, "kind": "vector", "value": [.8, 1, .8]},
                    {"id": 2, "kind": "vector", "value": [[.28, .52, .78], [.65, .3, .15], [.2, .6, .32]][index % 3]}]}],
            "template": None})
    return {"schema": 1, "origin": [0, 0, 0], "sections": [], "objects": objects}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def percentile(values, fraction):
    ordered = sorted(values)
    index = fraction * (len(ordered) - 1)
    lower, upper = math.floor(index), math.ceil(index)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def validate(folder, count, frames):
    summary = json.loads((folder / "profile-summary.json").read_text(encoding="utf-8"))
    with (folder / "frames.csv").open(newline="", encoding="utf-8") as stream:
        records = list(csv.DictReader(stream))
    require(summary["hardware"] and summary["adapter"] and (summary["width"], summary["height"]) == (1280, 720),
            "fixed native hardware profile not satisfied")
    require(summary["objects"] == count, "workload draw count changed")
    require(summary["warmup_frames"] == 60 and summary["measured_frames"] == frames - 60,
            "warmup/measurement counts differ")
    require(summary["submitted_frames"] == summary["completed_frames"] == frames and len(records) == frames,
            "submitted/completed/raw frame counts differ")
    require(summary["schema"] == 2 and summary["workload_verified"] and
            summary["expected_submitted_frames"] == frames, "runtime fixed-work verification missing")
    require(not summary["device_lost"] and summary["debug_errors"] == 0, "GPU validation failed")
    require(summary["native_resolution"] and summary["render_scale"] == 1 and
            summary["antialiasing"] == "none" and not summary["frame_generation"], "render profile changed")
    require(not summary["allocation_overflow"], "allocation counter overflow")
    measured = records[60:]
    require(type(summary["allocation_counting_available"]) is bool and
            summary["allocation_scope_active"] is summary["allocation_counting_available"],
            "runtime allocation scope disagrees with build availability")
    if not summary["allocation_counting_available"]:
        require(all(int(row["allocation_requests"]) == 0 and int(row["allocation_requested_bytes"]) == 0
                    for row in records), "unavailable runtime allocation counters contain measurements")
    for index, row in enumerate(records, 1):
        require(int(row["frame"]) == index and int(row["backend_cpu_frame"]) == index,
                "CPU frame provenance mismatch")
        require(int(row["submitted_frame_id"]) == index and
                (int(row["width"]), int(row["height"])) == (1280, 720),
                "per-frame submission/drawable provenance mismatch")
        require(row["phase"] == ("warmup" if index <= 60 else "measure"), "phase mismatch")
        require(int(row["simulation_tick"]) == index, "fixed simulation work changed")
        require(row["allocation_overflow"] == "0", "allocation overflow in raw sample")
        require(row["gpu_valid"] in ("0", "1") and (row["gpu_ms"] != "") == (row["gpu_valid"] == "1"),
                "GPU validity and timing disagree")
        for key, value in row.items():
            if key.endswith("_ms") and value:
                require(math.isfinite(float(value)) and float(value) >= 0, f"invalid timing {key}")
    for name, expected in summary["statistics_ms"].items():
        values = [float(row[name]) for row in measured if row[name] != ""]
        if expected is None:
            require(not values, f"missing summary with valid {name} data")
            continue
        require(len(values) == expected["count"], f"summary sample count differs: {name}")
        for key, actual in [("median", percentile(values, .5)), ("p95", percentile(values, .95)),
                            ("p99", percentile(values, .99)), ("max", max(values))]:
            require(math.isclose(actual, expected[key], abs_tol=1e-10, rel_tol=1e-10),
                    f"independent {name}/{key} calculation differs")
        require(sum(value > 16.67 for value in values) == expected["spikes_gt_16_67_ms"], "spike count differs")
    valid_gpu = sum(row["gpu_valid"] == "1" for row in measured)
    all_valid_gpu = sum(row["gpu_valid"] == "1" for row in records)
    require(valid_gpu == summary["measured_gpu_samples"], "GPU timing IDs/sample counts differ")
    require(all_valid_gpu == summary["gpu_sample_count"] and
            summary["gpu_samples_complete"] == (all_valid_gpu == frames), "GPU completeness receipt differs")
    if summary["gpu_timing_available"]:
        require(all_valid_gpu == frames, "completed GPU timestamp series has missing frames including warmup")
    require(sum(int(row["allocation_requests"]) for row in measured) == summary["measured_allocation_requests"],
            "allocation request sum differs")
    require(sum(int(row["allocation_requested_bytes"]) for row in measured) == summary["measured_allocation_requested_bytes"],
            "allocation byte sum differs")
    memory = [row for row in measured if row["memory_sampled"] == "1"]
    require(memory and len(memory) == summary["measured_memory_samples"], "periodic memory evidence missing")
    images = [ppm(folder / name) for name in ("profile-initial.ppm", "profile-final.ppm")]
    require(all((image["width"], image["height"]) == (1280, 720) for image in images), "profile capture dimensions changed")
    return {"summary": summary, "images": images, "raw_csv_sha256": digest(folder / "frames.csv"),
            "receipt_sha256": digest(folder / "profile-summary.json"),
            "cooked_scene_sha256": digest(folder / "profile-scene.rscene"),
            "independent_statistics_verified": True}


def stats(values):
    if not values:
        return None
    require(all(math.isfinite(value) and value >= 0 for value in values), "invalid editor timing")
    return {"count": len(values), "median": percentile(values, .5), "p95": percentile(values, .95),
            "p99": percentile(values, .99), "max": max(values),
            "spikes_gt_16_67_ms": sum(value > 16.67 for value in values)}


def validate_editor(folder, count, measured_frames):
    receipt = json.loads((folder / "editor-profile.json").read_text(encoding="utf-8"))
    require(receipt["schema"] == 2 and receipt["passed"] and receipt["hardware"] and receipt["debug_errors"] == 0,
            "editor hardware profile failed")
    require(receipt["workload_verified"] and not receipt["allocation_overflow"], "editor fixed-work verification failed")
    require(receipt["objects"] == count and receipt["warmup_frames"] == 60 and
            receipt["static_measured_frames"] == measured_frames, "editor workload changed")
    require(receipt["width"] == receipt["internal_width"] and receipt["height"] == receipt["internal_height"],
            "editor internal resolution differs from native viewport")
    samples = receipt["samples"]
    require(type(receipt["allocation_counting_available"]) is bool, "editor allocation availability must be explicit")
    if not receipt["allocation_counting_available"]:
        require(all(sample["allocation_requests"] == 0 and sample["allocation_requested_bytes"] == 0
                    for sample in samples), "unavailable editor allocation counters contain measurements")
    require(len(samples) == 60 + measured_frames + 240, "editor phase sample count differs")
    require(receipt["expected_submitted_frames"] == receipt["submitted_frames"] ==
            receipt["completed_frames"] == len(samples), "editor successful frame/submission counts differ")
    expected_counts = {"warmup": 60, "static": measured_frames, "editing": 100, "save_load": 20, "play": 120}
    require(receipt["actual_phase_frames"] == expected_counts, "editor actual phase counts differ")
    require(receipt["inspector_edits"] == receipt["inspector_undos"] == 50 and
            receipt["saves"] == receipt["loads"] == 20 and
            receipt["play_ticks"] == 120, "editor fixed operations differ")
    by_submission = {}
    for index, sample in enumerate(samples, 1):
        require(sample["frame"] == sample["submitted_frame_id"] == sample["last_cpu_frame"] == index,
                "editor callback/submission/CPU frame sequence differs")
        require((sample["width"], sample["height"]) == (receipt["width"], receipt["height"]),
                "editor per-frame dimensions changed")
        expected_tick = index - (60 + measured_frames + 120) if sample["phase"] == "play" else 0
        require(sample["simulation_tick"] == expected_tick, "editor fixed Play tick provenance changed")
        for key, value in sample.items():
            if key.endswith("_ms"):
                require(math.isfinite(value) and value >= 0, f"invalid editor {key}")
        by_submission[sample["submitted_frame_id"]] = sample
    with (folder / "editor-frames.csv").open("w", newline="", encoding="utf-8") as stream:
        keys = list(dict.fromkeys(key for sample in samples for key in sample))
        writer = csv.DictWriter(stream, fieldnames=keys)
        writer.writeheader()
        writer.writerows(samples)
    operations = receipt["operations"]
    with (folder / "editor-operations.csv").open("w", newline="", encoding="utf-8") as stream:
        keys = list(dict.fromkeys(key for operation in operations for key in operation))
        writer = csv.DictWriter(stream, fieldnames=keys)
        writer.writeheader()
        writer.writerows(operations)
    for operation in operations:
        require(operation.get("passed", True), f"editor operation failed: {operation['kind']}")
    expected_operations = {"inspector_edit": 50, "inspector_undo": 50, "save": 20,
                           "load_and_refresh": 20, "start_play": 1, "stop_play": 1,
                           "static_capture": 1, "final_capture_and_drain": 1}
    require(Counter(operation["kind"] for operation in operations) == expected_operations,
            "editor recorded operation counts differ from fixed workload")
    editing_operations = [operation["kind"] for operation in operations
                          if operation["kind"] in ("inspector_edit", "inspector_undo")]
    require(editing_operations == [kind for _ in range(50) for kind in ("inspector_edit", "inspector_undo")],
            "editor edit/Undo sequence differs from alternating bounded-history workload")
    # Read the entire completed query series after final drain. The latest query
    # on a CPU sample is a lagging observation and is never copied into percentiles.
    gpu = {}
    for sample in receipt["gpu_samples"]:
        identifier = sample["frame"]
        require(identifier in by_submission and identifier not in gpu, "editor GPU frame ID invalid or duplicated")
        require(sample["phase"] == by_submission[identifier]["phase"], "editor GPU phase differs from its submission")
        require(math.isfinite(sample["milliseconds"]) and sample["milliseconds"] >= 0, "editor GPU time invalid")
        gpu[identifier] = sample["milliseconds"]
    require(list(gpu) == sorted(gpu), "editor GPU sample order changed")
    require(receipt["gpu_sample_count"] == len(gpu) and
            receipt["gpu_samples_complete"] == (len(gpu) == len(samples)), "editor GPU completeness receipt differs")
    if receipt["gpu_timing_available"]:
        require(len(gpu) == len(samples), "editor final drained GPU series is incomplete")
    phases = {}
    for phase, expected_count in expected_counts.items():
        selected = [sample for sample in samples if sample["phase"] == phase]
        require(len(selected) == expected_count, f"editor phase count differs: {phase}")
        phases[phase] = {key: stats([sample[key] for sample in selected]) for key in
                         ("interval_ms", "cpu_frame_ms", "extraction_ms", "render_submit_ms")}
        phases[phase]["gpu_ms"] = stats([value for identifier, value in gpu.items()
                                           if by_submission[identifier]["phase"] == phase])
        phases[phase]["allocation_requests"] = sum(sample["allocation_requests"] for sample in selected)
        phases[phase]["allocation_requested_bytes"] = sum(sample["allocation_requested_bytes"] for sample in selected)
        phases[phase]["resident_max_bytes"] = max(sample["resident_bytes"] for sample in selected)
        phases[phase]["peak_resident_bytes"] = max(sample["peak_resident_bytes"] for sample in selected)
    operation_stats = {kind: stats([operation["milliseconds"] for operation in operations if operation["kind"] == kind])
                       for kind in dict.fromkeys(operation["kind"] for operation in operations)}
    comparison_keys = ("schema", "application", "adapter", "hardware", "width", "height", "internal_width",
                       "internal_height", "mode", "quality", "objects", "warmup_frames", "static_measured_frames",
                       "allocation_counting_available", "allocation_scope", "memory_scope", "timing_scope",
                       "play_workload", "debug_layer", "present_sync_interval", "native_resolution",
                       "antialiasing", "render_scale", "frame_generation", "gpu_timing_available",
                       "expected_submitted_frames", "submitted_frames", "completed_frames", "actual_phase_frames",
                       "inspector_edits", "inspector_undos", "saves", "loads", "play_ticks", "editing_workload")
    result = {"receipt_path": str(folder / "editor-profile.json"),
              "receipt_sha256": digest(folder / "editor-profile.json"),
              "raw_csv_sha256": digest(folder / "editor-frames.csv"),
              "operations_csv_sha256": digest(folder / "editor-operations.csv"),
              "comparison_settings": {key: receipt[key] for key in comparison_keys},
              "source_commit": receipt["source_commit"], "authored_input_sha256": receipt["authored_input_sha256"],
              "adapter": receipt["adapter"], "dimensions": [receipt["width"], receipt["height"]],
              "objects": count, "startup_first_present_ms": receipt["startup_first_present_ms"],
              "initial_scene_load_ms": receipt["initial_scene_load_ms"],
              "initial_scene_load_measured": receipt["initial_scene_load_measured"],
              "allocation_counting_available": receipt["allocation_counting_available"],
              "debug_warnings": receipt["debug_warnings"], "debug_errors": receipt["debug_errors"],
              "phase_statistics_ms": phases, "operations_ms": operation_stats,
              "gpu_distinct_timed_frames": len(gpu), "gpu_missing_frames": len(samples) - len(gpu),
              "gpu_scope": "Unique sorted submission-ID-associated timestamps after final drain; availability/completeness checked",
              "percentile_method": "linear interpolation at p*(n-1)",
              "static_image": ppm(folder / "static-scene.ppm")}
    (folder / "editor-profile-summary.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, help="built runtime; not needed for --generate-only")
    parser.add_argument("--editor-exe", type=Path, help="optional Qt executable; also exercise editor profile phases")
    parser.add_argument("--output", type=Path, help="fresh Evidence directory; allocated automatically when omitted")
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--frames", type=int, default=660, help="total frames including fixed60 warmup")
    parser.add_argument("--counts", type=int, nargs="+", choices=(1, 128, 512), default=[1, 128, 512])
    parser.add_argument("--shaders", type=Path)
    parser.add_argument("--label", default="baseline")
    args = parser.parse_args()
    require(60 < args.frames <= 3600, "frames must include60 warmup and at least1 measured")
    require(args.generate_only or args.exe is not None, "--exe required for actual measurements")
    require(len(set(args.counts)) == len(args.counts), "duplicate workload count")
    output = evidence_directory("desktop-performance", args.output)
    workloads = []
    for count in args.counts:
        path = output / f"boxes-{count}.scene.json"
        path.write_text(json.dumps(scene(count), indent=2) + "\n", encoding="utf-8")
        workloads.append({"objects": count, "path": str(path), "sha256": digest(path)})
    manifest = {"schema": 1, "label": args.label, "workloads": workloads,
                "warmup_frames": 60, "measured_frames": args.frames - 60,
                "dimensions": [1280, 720], "native_resolution": True, "antialiasing": "none",
                "camera": "fixed target(0,.6,0), yaw.64, pitch.60, distance50m",
                "simulation": "fixed1/60s step per rendered frame; first Box sample-Play rotation",
                "platform": platform.platform(), "python": platform.python_version(),
                "status": "generated workload data only" if args.generate_only else "measurement pending"}
    (output / "workload-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    if args.generate_only:
        print(f"Generated {len(workloads)} fixed Scene workloads: {output}")
        return
    executable = args.exe.resolve(strict=True)
    results, editor_results, commands = [], [], []
    editor_executable = args.editor_exe.resolve(strict=True) if args.editor_exe else None
    for workload in workloads:
        folder = output / f"runtime-{workload['objects']}"
        command = [str(executable), "--profile", str(folder), "--frames", str(args.frames), "--scene", workload["path"]]
        if args.shaders:
            command.extend(["--shaders", str(args.shaders.resolve(strict=True))])
        start = time.perf_counter()
        try:
            result = subprocess.run(command, text=True, encoding="utf-8", errors="replace",
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=210)
        except subprocess.TimeoutExpired as error:
            commands.append({"command": command, "timeout_seconds": 210, "output": str(error.stdout)})
            (output / "commands.json").write_text(json.dumps(commands, indent=2), encoding="utf-8")
            raise
        commands.append({"command": command, "returncode": result.returncode,
                         "elapsed_seconds": time.perf_counter() - start, "output": result.stdout})
        (output / "commands.json").write_text(json.dumps(commands, indent=2), encoding="utf-8")
        require(result.returncode == 0, f"actual runtime profile failed: {result.stdout}")
        results.append(validate(folder, workload["objects"], args.frames))
        print(f"Runtime {workload['objects']} boxes: measured {args.frames - 60} frames and verified raw statistics", flush=True)
        if editor_executable:
            editor_folder = output / f"editor-{workload['objects']}"
            command = [str(editor_executable), "--profile", str(editor_folder), "--frames", str(args.frames - 60),
                       "--scene", workload["path"]]
            if args.shaders:
                command.extend(["--shaders", str(args.shaders.resolve(strict=True))])
            start = time.perf_counter()
            try:
                result = subprocess.run(command, text=True, encoding="utf-8", errors="replace",
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
            except subprocess.TimeoutExpired as error:
                commands.append({"command": command, "timeout_seconds": 240, "output": str(error.stdout)})
                (output / "commands.json").write_text(json.dumps(commands, indent=2), encoding="utf-8")
                raise
            commands.append({"command": command, "returncode": result.returncode,
                             "elapsed_seconds": time.perf_counter() - start, "output": result.stdout})
            (output / "commands.json").write_text(json.dumps(commands, indent=2), encoding="utf-8")
            require(result.returncode == 0, f"actual editor profile failed: {result.stdout}")
            editor_results.append(validate_editor(editor_folder, workload["objects"], args.frames - 60))
            print(f"Editor {workload['objects']} boxes: static/edit/save-load/Play phases summarized", flush=True)
    manifest["status"] = "measured and independently verified"
    (output / "workload-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    verification = {"schema": 1, "label": args.label, "passed": True,
                    "executable": str(executable), "executable_sha256": digest(executable),
                    "workload_manifest": manifest, "commands": commands, "results": results,
                    "editor_executable": str(editor_executable) if editor_executable else None,
                    "editor_executable_sha256": digest(editor_executable) if editor_executable else None,
                    "editor_results": editor_results,
                    "limits": "Observation only, no optimization/quality reduction. Timings cover this hardware, fixed Box workloads and current debug-layer/presentation configuration; not8K/RTX5090 or production performance acceptance."}
    (output / "verification.json").write_text(json.dumps(verification, indent=2), encoding="utf-8")
    print(f"Validated {len(results)} actual hardware profiles, raw percentiles/GPU frame IDs/memory/allocation counts: {output}")


if __name__ == "__main__":
    main()

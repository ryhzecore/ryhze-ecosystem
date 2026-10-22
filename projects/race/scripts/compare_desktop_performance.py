#!/usr/bin/env python3
"""Compare verified desktop profiles without launching applications or changing evidence.

Requires each original evidence directory, including runtime and Qt editor captures.
Repeated runs stay separate: percentiles are never averaged or pooled. A zero exit
code certifies comparable inputs and exact checked artifacts, not faster performance.
"""
from output_paths import evidence_report
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import sys

from verify_runtime_desktop import ppm

MAX_JSON_BYTES = 32 * 1024 * 1024
MAX_ARTIFACT_BYTES = 128 * 1024 * 1024
MAX_PAIRS = 8
MANIFEST_SETTINGS = (
    "schema", "warmup_frames", "measured_frames", "dimensions", "native_resolution",
    "antialiasing", "camera", "simulation", "platform", "python")
RUNTIME_SETTINGS = (
    "schema", "application", "adapter", "hardware", "vendor_id", "device_id",
    "dedicated_video_bytes", "width", "height", "objects", "submitted_frames",
    "completed_frames", "warmup_frames", "measured_frames", "native_resolution",
    "antialiasing", "quality_tier", "mode", "present_sync_interval", "render_scale",
    "frame_generation", "camera", "simulation", "allocation_counting_available",
    "allocation_scope_active", "allocation_scope", "instrumentation", "memory_scope",
    "gpu_scope", "cpu_scope", "percentile_method", "debug_layer", "gpu_timing_available",
    "expected_submitted_frames", "workload_verified", "gpu_sample_count", "gpu_samples_complete", "scene_load_measured")
EDITOR_SETTINGS = (
    "schema", "application", "adapter", "hardware", "width", "height", "internal_width",
    "internal_height", "mode", "quality", "objects", "warmup_frames", "static_measured_frames",
    "allocation_counting_available", "allocation_scope", "memory_scope", "timing_scope",
    "play_workload", "debug_layer", "present_sync_interval", "native_resolution",
    "antialiasing", "render_scale", "frame_generation", "gpu_timing_available",
    "expected_submitted_frames", "submitted_frames", "completed_frames", "actual_phase_frames",
    "inspector_edits", "inspector_undos", "saves", "loads", "play_ticks", "editing_workload")
RUNTIME_MEMORY = (
    "measured_allocation_requests", "measured_allocation_requested_bytes", "allocation_overflow",
    "measured_memory_samples", "resident_min_bytes", "resident_max_bytes", "peak_resident_bytes")
RUNTIME_STARTUP = ("document_create_ms", "scene_load_ms", "startup_first_present_ms", "profile_total_ms")
EDITOR_STARTUP = ("startup_first_present_ms", "initial_scene_load_ms")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def strict_pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json(path):
    require(path.stat().st_size <= MAX_JSON_BYTES, f"JSON exceeds {MAX_JSON_BYTES} bytes: {path}")
    def invalid_constant(value):
        raise ValueError(f"nonfinite JSON constant: {value}")
    data = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=strict_pairs,
                      parse_constant=invalid_constant)
    require(isinstance(data, dict), f"expected JSON object: {path}")
    return data


def selected(data, keys, context):
    require(isinstance(data, dict), f"{context}: expected object")
    missing = set(keys) - data.keys()
    require(not missing, f"{context}: missing fields {sorted(missing)}; regenerate evidence with matching instrumentation")
    return {key: data[key] for key in keys}


def same(before, after, context):
    # Python considers True == 1; measurement metadata must retain JSON types too.
    require(type(before) is type(after), f"{context}: metadata types differ")
    if isinstance(before, dict):
        require(before.keys() == after.keys(), f"{context}: fields differ")
        for key in before:
            same(before[key], after[key], f"{context}.{key}")
    elif isinstance(before, list):
        require(len(before) == len(after), f"{context}: lengths differ")
        for index, (left, right) in enumerate(zip(before, after)):
            same(left, right, f"{context}[{index}]")
    else:
        require(before == after, f"{context}: differs ({before!r} versus {after!r})")


def digest(path):
    require(path.is_file(), f"missing evidence file: {path}")
    require(path.stat().st_size <= MAX_ARTIFACT_BYTES, f"artifact exceeds bounded read size: {path}")
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def verified_hash(path, recorded):
    require(isinstance(recorded, str) and len(recorded) == 64 and
            all(c in "0123456789abcdef" for c in recorded), f"invalid SHA256 receipt: {path}")
    actual = digest(path)
    require(actual == recorded, f"artifact no longer matches recorded SHA256: {path}")
    return actual


def image_record(path, recorded):
    verified_hash(path, recorded["sha256"])
    actual = ppm(path)
    same(selected(recorded, ("width", "height", "bytes", "sha256"), str(path)),
         selected(actual, ("width", "height", "bytes", "sha256"), str(path)), str(path))
    return {key: actual[key] for key in ("width", "height", "bytes", "sha256")}


def indexed(records, key, context):
    require(isinstance(records, list) and 1 <= len(records) <= 3, f"{context}: expected one to three workloads")
    result = {}
    for item in records:
        count = item[key]
        require(type(count) is int and count in (1, 128, 512), f"{context}: unsupported workload size")
        require(count not in result, f"{context}: duplicate workload {count}")
        result[count] = item
    return result


def editor_operations(receipt):
    expected_counters = {"inspector_edits": 50, "inspector_undos": 50, "saves": 20, "loads": 20, "play_ticks": 120}
    same(selected(receipt, expected_counters, "editor operation counters"), expected_counters, "editor operation counters")
    require(isinstance(receipt["editing_workload"], str) and receipt["editing_workload"], "editor editing workload description missing")
    expected_operations = {"inspector_edit": 50, "inspector_undo": 50, "save": 20, "load_and_refresh": 20,
                           "start_play": 1, "stop_play": 1, "static_capture": 1, "final_capture_and_drain": 1}
    operations = receipt["operations"]
    require(all(item.get("passed", True) is True for item in operations), "editor operation failed")
    counts = dict(Counter(operation["kind"] for operation in operations))
    same(counts, expected_operations, "editor actual operation counts")
    editing = [item["kind"] for item in operations if item["kind"] in ("inspector_edit", "inspector_undo")]
    same(editing, ["inspector_edit", "inspector_undo"] * 50, "editor edit/Undo order")
    return counts


def numeric(value, context):
    require(type(value) in (int, float) and math.isfinite(value) and value >= 0,
            f"{context}: expected finite nonnegative measurement")


def distributions(data, context):
    require(isinstance(data, dict) and data, f"{context}: missing distributions")
    for name, values in data.items():
        if values is None:
            continue
        required = ("count", "median", "p95", "p99", "max", "spikes_gt_16_67_ms")
        selected(values, required, f"{context}.{name}")
        for key in required:
            numeric(values[key], f"{context}.{name}.{key}")
        require(type(values["count"]) is int and values["count"] > 0, f"{context}.{name}: invalid sample count")
        require(values["median"] <= values["p95"] <= values["p99"] <= values["max"],
                f"{context}.{name}: unordered percentiles")
        require(type(values["spikes_gt_16_67_ms"]) is int and values["spikes_gt_16_67_ms"] <= values["count"],
                f"{context}.{name}: invalid spike count")
    return data


def read_run(path):
    path = path.resolve(strict=True)
    evidence = load_json(path)
    require(type(evidence.get("schema")) is int and evidence["schema"] == 1 and evidence.get("passed") is True, f"{path}: successful schema1 verification required")
    manifest = evidence["workload_manifest"]
    require(manifest.get("status") == "measured and independently verified", f"{path}: not measured verification")
    manifest_settings = selected(manifest, MANIFEST_SETTINGS, "manifest")
    same(manifest, load_json(path.parent / "workload-manifest.json"), "embedded and original manifest")
    require(type(manifest["warmup_frames"]) is int and manifest["warmup_frames"] == 60 and
            type(manifest["measured_frames"]) is int and 0 < manifest["measured_frames"] <= 3540,
            "unsupported bounded frame counts")
    workloads = indexed(manifest["workloads"], "objects", "workloads")
    runtimes = indexed([dict(item, objects=item["summary"]["objects"]) for item in evidence["results"]], "objects", "runtime")
    editors = indexed(evidence["editor_results"], "objects", "Qt editor")
    require(workloads.keys() == runtimes.keys() == editors.keys(), "both runtime and Qt evidence required for every workload")
    reports = {}
    for count, workload in workloads.items():
        runtime, editor = runtimes[count], editors[count]
        summary = runtime["summary"]
        runtime_dir, editor_dir = path.parent / f"runtime-{count}", path.parent / f"editor-{count}"
        same(summary, load_json(runtime_dir / "profile-summary.json"), "embedded and original runtime summary")
        same(editor, load_json(editor_dir / "editor-profile-summary.json"), "embedded and original editor summary")
        receipt = load_json(editor_dir / "editor-profile.json")
        runtime_receipt_sha = verified_hash(runtime_dir / "profile-summary.json", runtime["receipt_sha256"])
        editor_receipt_sha = verified_hash(editor_dir / "editor-profile.json", editor["receipt_sha256"])
        editor_frames_sha = verified_hash(editor_dir / "editor-frames.csv", editor["raw_csv_sha256"])
        editor_operations_sha = verified_hash(editor_dir / "editor-operations.csv", editor["operations_csv_sha256"])
        require(type(summary["schema"]) is int and summary["schema"] == 2 and
                type(receipt["schema"]) is int and receipt["schema"] == 2 and
                summary["workload_verified"] is True and receipt["workload_verified"] is True,
                "schema2 verified runtime and editor workload receipts required")
        require(summary["scene_load_measured"] is True and receipt["initial_scene_load_measured"] is True and
                editor["initial_scene_load_measured"] is True, "explicit workload scene-load measurements required")
        require(runtime.get("independent_statistics_verified") is True, "runtime raw statistics not verified")
        require(receipt.get("passed") is True and receipt.get("allocation_overflow") is False and
                summary.get("allocation_overflow") is False, "failed run or saturated allocation counters")
        require(summary.get("hardware") is True and receipt.get("hardware") is True and
                summary.get("device_lost") is False and summary.get("debug_errors") == receipt.get("debug_errors") == 0,
                "hardware execution errors invalidate comparison")
        runtime_settings = selected(summary, RUNTIME_SETTINGS, "runtime settings")
        editor_settings = selected(receipt, EDITOR_SETTINGS, "editor settings")
        same(editor["comparison_settings"], editor_settings, "embedded editor comparison settings")
        editor_settings["initial_scene_load_measured"] = receipt["initial_scene_load_measured"]
        for key in ("source_commit", "authored_input_sha256", "adapter", "objects", "allocation_counting_available"):
            same(editor[key], receipt[key], f"editor summary/receipt {key}")
        same(editor["dimensions"], [receipt["width"], receipt["height"]], "editor summary dimensions")
        same([summary["width"], summary["height"]], manifest["dimensions"], "runtime/manifest dimensions")
        for data in (summary, receipt):
            same(data["warmup_frames"], manifest["warmup_frames"], "warmup count")
            require(data["objects"] == count, "object count mismatch")
        same(summary["measured_frames"], manifest["measured_frames"], "runtime measured count")
        same(receipt["static_measured_frames"], manifest["measured_frames"], "editor measured count")
        total = manifest["warmup_frames"] + manifest["measured_frames"]
        require(summary["expected_submitted_frames"] == summary["submitted_frames"] == summary["completed_frames"] == total,
                "runtime frame completion mismatch")
        require(receipt["expected_submitted_frames"] == receipt["submitted_frames"] == receipt["completed_frames"] == total + 240,
                "editor frame completion mismatch")
        phases = Counter(sample["phase"] for sample in receipt["samples"])
        same(dict(phases), {"warmup": 60, "static": manifest["measured_frames"], "editing": 100, "save_load": 20, "play": 120},
             "editor phase frame counts")
        same(receipt["actual_phase_frames"], dict(phases), "editor actual phase counters")
        require([sample["frame"] for sample in receipt["samples"]] == list(range(1, total + 241)), "editor frame sequence mismatch")
        operation_counts = editor_operations(receipt)
        editor_settings["phase_frame_counts"] = dict(phases)
        editor_settings["operation_counts"] = operation_counts
        require(len(runtime["images"]) == 2, "runtime requires initial and final captures")
        artifacts = {
            "workload_sha256": verified_hash(path.parent / f"boxes-{count}.scene.json", workload["sha256"]),
            "cooked_scene_sha256": verified_hash(runtime_dir / "profile-scene.rscene", runtime["cooked_scene_sha256"]),
            "runtime_initial": image_record(runtime_dir / "profile-initial.ppm", runtime["images"][0]),
            "runtime_final": image_record(runtime_dir / "profile-final.ppm", runtime["images"][1]),
            "editor_static": image_record(editor_dir / "static-scene.ppm", editor["static_image"])}
        for name in ("runtime_initial", "runtime_final"):
            same([artifacts[name]["width"], artifacts[name]["height"]], [summary["width"], summary["height"]], "runtime capture dimensions")
        same([artifacts["editor_static"]["width"], artifacts["editor_static"]["height"]], editor["dimensions"], "editor capture dimensions")
        raw_csv = verified_hash(runtime_dir / "frames.csv", runtime["raw_csv_sha256"])
        runtime_stats = distributions(summary["statistics_ms"], "runtime timing")
        if summary["gpu_timing_available"]:
            require(summary["gpu_samples_complete"] is True and summary["gpu_sample_count"] == total,
                    "runtime full GPU series is incomplete")
            require(summary["measured_gpu_samples"] == manifest["measured_frames"] and runtime_stats.get("gpu_ms") is not None and
                    runtime_stats["gpu_ms"]["count"] == manifest["measured_frames"], "runtime completed GPU sample series is incomplete")
        for name, values in runtime_stats.items():
            if name != "gpu_ms":
                require(values is not None and values["count"] == manifest["measured_frames"], "CPU timing sample count mismatch")
        require(editor["gpu_distinct_timed_frames"] == receipt["gpu_sample_count"] and
                editor["gpu_missing_frames"] == total + 240 - receipt["gpu_sample_count"], "editor GPU count receipts differ")
        if receipt["gpu_timing_available"]:
            require(receipt["gpu_samples_complete"] is True and editor["gpu_missing_frames"] == 0,
                    "editor full GPU series is incomplete")
        phase_stats = editor["phase_statistics_ms"]
        same(set(phase_stats), set(phases), "editor phase summary names")
        for phase, metrics in phase_stats.items():
            distributions({key: value for key, value in metrics.items() if key.endswith("_ms")}, f"editor {phase}")
            for key, value in metrics.items():
                if not key.endswith("_ms"):
                    numeric(value, f"editor {phase}.{key}")
                elif key != "gpu_ms":
                    require(value is not None and value["count"] == phases[phase], "editor CPU timing count mismatch")
        operations = distributions(editor["operations_ms"], "editor operations")
        same(set(operations), set(operation_counts), "editor operation summary names")
        for name, values in operations.items():
            require(values is not None and values["count"] == operation_counts[name], "editor operation sample count mismatch")
        reports[count] = {
            "settings": {"runtime": runtime_settings, "editor": editor_settings}, "artifacts": artifacts,
            "provenance": {"runtime_source_commit": summary["source_commit"], "runtime_authored_input_sha256": summary["authored_input_sha256"],
                           "editor_source_commit": editor["source_commit"], "editor_authored_input_sha256": editor["authored_input_sha256"],
                           "runtime_receipt_sha256": runtime_receipt_sha, "raw_runtime_csv_sha256": raw_csv,
                           "raw_editor_receipt_sha256": editor_receipt_sha, "raw_editor_frames_csv_sha256": editor_frames_sha,
                           "raw_editor_operations_csv_sha256": editor_operations_sha},
            "runtime_timings_ms": runtime_stats, "runtime_startup_ms": selected(summary, RUNTIME_STARTUP, "runtime startup"),
            "runtime_memory_and_allocations": selected(summary, RUNTIME_MEMORY, "runtime memory/allocations"),
            "editor_phase_metrics": phase_stats, "editor_operations_ms": operations,
            "editor_startup_ms": selected(editor, EDITOR_STARTUP, "editor startup"),
            "observations": {"runtime_gpu_samples": summary["measured_gpu_samples"], "editor_gpu_samples": editor["gpu_distinct_timed_frames"],
                             "editor_gpu_missing_frames": editor["gpu_missing_frames"], "runtime_debug_warnings": summary["debug_warnings"],
                             "editor_debug_warnings": editor["debug_warnings"]}}
    return {"path": str(path), "sha256": digest(path), "label": evidence["label"], "manifest_settings": manifest_settings,
            "executable_sha256": evidence["executable_sha256"], "editor_executable_sha256": evidence["editor_executable_sha256"],
            "workloads": reports}


def changes(before, after, context="measurement"):
    """Preserve raw metrics and annotate each numeric leaf without a verdict."""
    if isinstance(before, dict) and isinstance(after, dict):
        require(before.keys() == after.keys(), f"{context}: measurement fields differ")
        return {key: changes(before[key], after[key], f"{context}.{key}") for key in before}
    if before is None or after is None:
        return {"absolute": None, "percent": None, "reason": "one or both measurements unavailable"}
    if type(before) is bool or type(after) is bool:
        same(before, after, context)
        return {"changed": False}
    numeric(before, f"{context}.before")
    numeric(after, f"{context}.after")
    difference = after - before
    percent = 100 * difference / before if before else None
    require(percent is None or math.isfinite(percent), f"{context}: percent change overflow")
    return {"absolute": difference, "percent": percent,
            **({"reason": "percent undefined for zero baseline"} if before == 0 else {})}


def compare(before, after):
    same(before["manifest_settings"], after["manifest_settings"], "workload manifest settings")
    require(before["workloads"].keys() == after["workloads"].keys(), "workload counts differ")
    workloads = {}
    metrics = ("runtime_timings_ms", "runtime_startup_ms", "runtime_memory_and_allocations", "editor_phase_metrics",
               "editor_operations_ms", "editor_startup_ms", "observations")
    for count, left in before["workloads"].items():
        right = after["workloads"][count]
        same(left["settings"], right["settings"], f"{count} objects settings")
        same(left["artifacts"], right["artifacts"], f"{count} objects exact workload/scene/PPM equivalence")
        workloads[count] = {"settings": left["settings"], "exact_artifacts": left["artifacts"],
                            "before_provenance": left["provenance"], "after_provenance": right["provenance"]}
        for metric in metrics:
            workloads[count][metric] = {"before": left[metric], "after": right[metric], "change": changes(left[metric], right[metric], metric)}
    return {"before": {key: value for key, value in before.items() if key != "workloads"},
            "after": {key: value for key, value in after.items() if key != "workloads"}, "workloads": workloads}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, nargs="+", required=True, help="one to eight verification.json files, paired by position")
    parser.add_argument("--after", type=Path, nargs="+", required=True)
    parser.add_argument("--output", type=Path, help="new JSON Evidence report; allocated automatically when omitted")
    args = parser.parse_args(argv)
    require(1 <= len(args.before) == len(args.after) <= MAX_PAIRS, "one to eight equally sized before/after lists required")
    args.output = evidence_report("desktop-performance-comparison", args.output, filename="comparison.json")
    require(not args.output.exists() and not args.output.is_symlink(), "fresh output file required")
    inputs = [path.resolve(strict=True) for path in args.before + args.after]
    require(len(set(inputs)) == len(inputs), "each before/after evidence file must be distinct; repeated paths are not repeated runs")
    pairs = []
    anchor = None
    for before_path, after_path in zip(args.before, args.after):
        before, after = read_run(before_path), read_run(after_path)
        require(before["path"] != after["path"], "before and after must be distinct verification files")
        pair = compare(before, after)
        identity = {"manifest": before["manifest_settings"], "workloads": {
            count: {"settings": value["settings"], "artifacts": value["artifacts"]} for count, value in before["workloads"].items()}}
        if anchor is not None:
            same(anchor, identity, "repeated pair comparability")
        anchor = identity
        pairs.append(pair)
    report = {"schema": 1, "comparability_and_exact_artifacts_verified": True, "pairs": pairs,
              "interpretation": "Changes are after minus before; percentages use before as denominator. Negative durations/bytes/requests mean lower observations. Zero baselines have null percentages. No performance acceptance threshold or speedup assertion.",
              "limits": "Pairs remain separate; percentiles are not averaged. CPU timings are wall durations including documented stalls; GPU scope and missing-sample counts are retained. Allocation requests are not live heap ownership; process resident bytes are sampled, not private bytes. Results do not establish causality, statistical significance, or production performance."}
    text = json.dumps(report, indent=2, allow_nan=False) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(text)
    print(f"Compared {len(pairs)} run pair(s); exact artifacts and settings match. Observation report: {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, RuntimeError, OSError, KeyError, TypeError) as error:
        print(f"Desktop performance comparison rejected: {error}", file=sys.stderr)
        sys.exit(1)

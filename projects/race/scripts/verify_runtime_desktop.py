#!/usr/bin/env python3
"""Opt-in, visible Windows SDL/D3D12 application smoke; no dependency install."""
from output_paths import evidence_directory
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def ppm(path):
    data = path.read_bytes()
    cursor = 0

    def token():
        nonlocal cursor
        while cursor < len(data):
            if data[cursor] in b" \t\r\n":
                cursor += 1
            elif data[cursor] == 35:
                cursor = data.index(b"\n", cursor) + 1
            else:
                break
        start = cursor
        while cursor < len(data) and data[cursor] not in b" \t\r\n":
            cursor += 1
        return data[start:cursor]

    require(token() == b"P6", f"{path.name}: expected binary P6")
    width, height, maximum = int(token()), int(token()), int(token())
    require(0 < width <= 4096 and 0 < height <= 4096 and maximum == 255,
            f"{path.name}: invalid dimensions/range")
    require(cursor < len(data) and data[cursor] in b" \t\r\n", "PPM separator absent")
    cursor += 2 if data[cursor:cursor + 2] == b"\r\n" else 1
    pixels = data[cursor:]
    require(len(pixels) == width * height * 3, f"{path.name}: truncated/extra pixel bytes")
    colors = set()
    for offset in range(0, len(pixels), 3):
        colors.add(pixels[offset:offset + 3])
        if len(colors) >= 16:
            break
    require(len(colors) >= 4, f"{path.name}: image is blank or nearly constant")
    return {"path": str(path), "width": width, "height": height,
            "distinct_colors_at_least": len(colors), "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


def validate(directory):
    receipt = json.loads((directory / "runtime-receipt.json").read_text(encoding="utf-8"))
    require(receipt["graphics_api"] == "D3D12" and receipt["hardware"], "hardware D3D12 required")
    require(receipt["adapter"] and receipt["objects"], "adapter/visible scene missing")
    for key in ("canonical_scene_roundtrip_equal", "authored_scene_unchanged_by_play",
                "resize_observed", "minimize_observed", "restore_observed",
                "close_event_observed", "scripted_camera_probe", "navigation_event_probe"):
        require(receipt[key] is True, f"receipt check failed: {key}")
    require(receipt["submitted_frames"] == receipt["completed_frames"] == receipt["rendered_frames"],
            "GPU submitted/completed/application frame counts differ")
    require(receipt["rendered_frames"] >= 8 and receipt["scene_generation"] > 0 and
            receipt["origin_revision"] > 0, "missing frame/Scene revisions")
    require(0 <= receipt["maximum_input_dt_seconds"] <= 0.05, "input elapsed-time bound exceeded")
    require(not receipt["device_lost"] and receipt["debug_errors"] == 0, "GPU errors reported")
    acks = receipt["navigation_acknowledgments"]
    require(len(acks) == 8 and receipt["navigation_external_events"] == 0,
            "navigation probe acknowledgments/interference invalid")
    previous_sequence = 0
    for index, ack in enumerate(acks):
        require(ack["source_token"] == 0x5241434500000000 + index + 1 and
                previous_sequence < ack["sequence"] <= receipt["input_delivered"] and
                ack["delivered_ns"] > 0 and ack["context"] > 0 and
                0 <= ack["delivery_tick"] < ack["sampled_tick"] <= receipt["simulation_ticks"],
                f"navigation event {index}: delivery/fixed-update barrier absent")
        previous_sequence = ack["sequence"]
    require(receipt["navigation_wait_iterations"] >= 0, "negative navigation wait count")
    paths = [directory / name for name in
             ("runtime-saved.rscene", "runtime-roundtrip.rscene", "runtime-after-play.rscene")]
    blobs = [path.read_bytes() for path in paths]
    require(blobs[0].startswith(b"RACESCN1") and blobs[0] == blobs[1] == blobs[2],
            "independent cooked Scene byte parity failed")
    require(len(blobs[0]) == receipt["scene_bytes"], "Scene receipt size mismatch")
    images = [ppm(directory / name) for name in ("initial.ppm", "runtime.ppm")]
    require((images[0]["width"], images[0]["height"]) ==
            (receipt["initial_width"], receipt["initial_height"]), "initial readback size mismatch")
    require((images[1]["width"], images[1]["height"]) ==
            (receipt["width"], receipt["height"]), "final readback size mismatch")
    require(images[0]["sha256"] != images[1]["sha256"], "readbacks unexpectedly identical")
    return {"receipt": receipt, "images": images,
            "scene_sha256": hashlib.sha256(blobs[0]).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, help="new Evidence directory; allocated automatically when omitted")
    parser.add_argument("--scene", type=Path, help="editor-authored JSON or cooked .rscene")
    parser.add_argument("--shaders", type=Path)
    args = parser.parse_args()
    executable = args.exe.resolve(strict=True)
    output = evidence_directory("runtime-desktop", args.output)
    commands = []

    def execute(arguments, expected):
        command = [str(executable), *arguments]
        start = time.perf_counter()
        result = subprocess.run(command, text=True, encoding="utf-8", errors="replace",
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
        commands.append({"command": command, "returncode": result.returncode,
                         "elapsed_seconds": time.perf_counter() - start, "output": result.stdout})
        (output / "commands.json").write_text(json.dumps(commands, indent=2), encoding="utf-8")
        require(result.returncode == expected, f"unexpected exit {result.returncode}: {result.stdout}")

    execute(["--help"], 0)
    execute(["--frames", "0"], 1)
    common = ["--shaders", str(args.shaders.resolve(strict=True))] if args.shaders else []
    first = output / "first"
    arguments = [*common, "--smoke", str(first)]
    if args.scene:
        arguments.extend(["--scene", str(args.scene.resolve(strict=True))])
    execute(arguments, 0)
    first_result = validate(first)
    second = output / "reloaded"
    execute([*common, "--scene", str(first / "runtime-saved.rscene"), "--smoke", str(second)], 0)
    second_result = validate(second)
    require(first_result["scene_sha256"] == second_result["scene_sha256"], "new-process cooked Scene changed")
    require([(o["id_high"], o["id_low"]) for o in first_result["receipt"]["objects"]] ==
            [(o["id_high"], o["id_low"]) for o in second_result["receipt"]["objects"]],
            "new-process visible persistent IDs changed")
    result = {"schema": 1, "passed": True, "commands": commands,
              "first": first_result, "reloaded": second_result,
              "limitations": "Pixel checks prove finite-size nonblank GPU output, not visual quality or physical transport. Scripted lifecycle/camera smoke does not certify physical keyboard/mouse interaction."}
    (output / "verification.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f"Runtime smoke passed: {len(commands)} commands; two hardware runs, Scene parity, four GPU readbacks. {output}")


if __name__ == "__main__":
    main()

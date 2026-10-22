#!/usr/bin/env python3
"""Run one Linux build/test preset and retain exact local JSON evidence."""
from __future__ import annotations
from output_paths import evidence_directory

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
from source_manifest import ROOT, authored_inputs, file_sha256, git_state, manifest, write_json

EXECUTABLES = ("race_headless", "race_tests", "race_benchmarks", "race_scheduler_workloads", "race_scene_identity_tests", "race_scene_tests", "race_scene_headless", "race_scene_workloads", "race_assets_tool", "race_assets_headless", "race_assets_tests", "race_assets_fuzz", "race_render_tests", "race_render_fixture")
PRESETS = ("debug", "release", "asan", "tsan")


def read_optional(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8", errors="replace").strip()[:4096]
    except OSError:
        return None


def hardware() -> dict:
    cpu = {}
    content = read_optional(Path("/proc/cpuinfo"))
    if content:
        for line in content.splitlines():
            if ":" in line:
                key, value = (part.strip() for part in line.split(":", 1))
                if key in {"vendor_id", "model name", "cpu family", "model", "stepping", "cpu cores", "siblings"} and key not in cpu:
                    cpu[key] = value
    memory = {}
    content = read_optional(Path("/proc/meminfo"))
    if content:
        for line in content.splitlines():
            if ":" in line:
                key, value = line.split(":", 1)
                if key in {"MemTotal", "MemAvailable", "SwapTotal", "SwapFree"}:
                    memory[key] = value.strip()
    governors = {}
    for path in sorted(Path("/sys/devices/system/cpu").glob("cpu[0-9]*/cpufreq/scaling_governor")):
        governors[path.parent.parent.name] = read_optional(path)
    power = {}
    for path in sorted(Path("/sys/class/power_supply").glob("*")):
        power[path.name] = {name: read_optional(path / name) for name in ("type", "status", "online", "capacity")}
    return {"cpu": cpu or {"unavailable": True}, "logical_cpu_count": os.cpu_count(),
            "available_cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
            "memory_at_start": memory or {"unavailable": True}, "cpu_governors": governors or {"unavailable": True},
            "platform_power_profile": read_optional(Path("/sys/firmware/acpi/platform_profile")) or "unknown/unavailable",
            "power_supply": power or {"unavailable": True},
            "measurement_context": "No power mode, governor or frequency was changed. Concurrent desktop/container activity is uncontrolled; this validation is not a benchmark."}


def run_step(name: str, command: list[str], output: Path, timeout: int) -> dict:
    log = output / f"{name}.log"
    started = time.monotonic()
    result = {"name": name, "command": command, "timeout_seconds": timeout, "log": str(log.resolve()),
              "started_utc": datetime.now(timezone.utc).isoformat(), "timed_out": False}
    with log.open("wb") as stream:
        try:
            process = subprocess.Popen(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, start_new_session=True,
                                       env=dict(os.environ, RACE_EVIDENCE_RUN_DIR=str(output)))
            try:
                result["exit_code"] = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                result["timed_out"] = True
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                result["exit_code"] = process.returncode
        except OSError as error:
            result["exit_code"] = None
            result["error"] = type(error).__name__
            stream.write(f"Unable to start process: {type(error).__name__}\n".encode())
    result["elapsed_seconds"] = round(time.monotonic() - started, 6)
    result["status"] = "passed" if result["exit_code"] == 0 and not result["timed_out"] else "failed"
    result["log_sha256"] = file_sha256(log)
    print(f"{name}: {result['status']} ({result['elapsed_seconds']:.3f}s)", flush=True)
    return result


def compile_configuration(build: Path) -> dict:
    cache = read_optional(build / "CMakeCache.txt")
    # Read the complete cache, but retain only approved build/toolchain keys.
    if (build / "CMakeCache.txt").is_file():
        cache = (build / "CMakeCache.txt").read_text(errors="replace")
    selected = {}
    exact = {"CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "CMAKE_MAKE_PROGRAM", "CMAKE_GENERATOR", "RACE_SANITIZER"}
    if cache:
        for line in cache.splitlines():
            if line.startswith(("#", "//")) or "=" not in line or ":" not in line:
                continue
            declaration, value = line.split("=", 1)
            key = declaration.split(":", 1)[0]
            if key in exact or key.startswith(("CMAKE_CXX_FLAGS", "CMAKE_EXE_LINKER_FLAGS")):
                selected[key] = value
    commands_path = build / "compile_commands.json"
    commands = None
    if commands_path.is_file():
        commands = {"path": str(commands_path.resolve()), "sha256": file_sha256(commands_path),
                    "entries": json.loads(commands_path.read_text())}
    return {"cache_values": selected, "compile_commands": commands,
            "build_ninja_sha256": file_sha256(build / "build.ninja") if (build / "build.ninja").is_file() else None,
            "flags_note": "compile_commands preserves actual per-file commands; selected cache values also capture linker and sanitizer settings."}


def junit_summary(path: Path) -> dict:
    if not path.is_file():
        return {"status": "unavailable", "reason": "CTest did not write its JUnit report"}
    try:
        tree = ET.parse(path)
        cases = tree.findall(".//testcase")
        failures = sum(case.find("failure") is not None or case.find("error") is not None for case in cases)
        skipped = sum(case.find("skipped") is not None for case in cases)
        return {"status": "passed" if cases and failures == 0 and skipped == 0 else "failed",
                "test_count": len(cases), "failed": failures, "skipped": skipped,
                "names": [case.get("name", "") for case in cases], "path": str(path.resolve()), "sha256": file_sha256(path)}
    except (OSError, ET.ParseError) as error:
        return {"status": "failed", "error": type(error).__name__}


def verify(receipt_path: Path) -> int:
    receipt = json.loads(receipt_path.read_text())
    if receipt.get("schema") != 1 or receipt.get("kind") != "race-artifact-receipt":
        raise ValueError("unsupported artifact receipt schema or kind")
    actual = authored_inputs()
    current_git = git_state()
    differences = []
    if actual["sha256"] != receipt["authored_input_sha256"]:
        differences.append("authored input digest differs")
    artifacts = receipt.get("artifacts", [])
    if len(artifacts) != len(EXECUTABLES) or {item.get("name") for item in artifacts} != set(EXECUTABLES):
        differences.append("receipt does not identify all expected executables")
    for item in artifacts:
        path = Path(item["path"])
        if not path.is_file() or file_sha256(path) != item["sha256"]:
            differences.append(f"artifact differs or is missing: {item['name']}")
    embedded_identity = None
    if not differences:
        executable = next(item["path"] for item in artifacts if item["name"] == "race_headless")
        result = subprocess.run([executable, "--identity"], capture_output=True, text=True, timeout=30, check=False)
        if result.returncode != 0:
            differences.append("embedded identity command failed")
        else:
            try:
                embedded_identity = json.loads(result.stdout)
                if embedded_identity != receipt.get("embedded_identity"):
                    differences.append("embedded executable identity differs")
                if embedded_identity.get("source_commit") != receipt["source_git"]["commit"] or embedded_identity.get("authored_input_sha256") != receipt["authored_input_sha256"]:
                    differences.append("embedded executable identity does not match recorded source")
            except (ValueError, AttributeError):
                differences.append("embedded executable identity is malformed")
    if receipt.get("accepted") is not True:
        differences.append("receipt records failed or incomplete validation")
    # Evidence bytes are part of the verification record. This detects accidental
    # edits, not malicious replacement: receipts are local JSON, not signatures.
    for evidence in receipt.get("evidence_files", []):
        path = Path(evidence["path"])
        if not path.is_file() or file_sha256(path) != evidence["sha256"]:
            differences.append(f"evidence differs or is missing: {path.name}")
    answer = {"verified": not differences, "differences": differences,
              "tested_commit": receipt["source_git"]["commit"], "current_git": current_git,
              "git_revision_matches": current_git["commit"] == receipt["source_git"]["commit"],
              "embedded_identity": embedded_identity,
              "authored_input_sha256": actual["sha256"],
              "note": "A matching authored digest can survive documentation-only commits. The recorded tests belong to tested_commit and the exact executable hashes, not automatically to a newer commit."}
    print(json.dumps(answer, indent=2, sort_keys=True))
    return 0 if not differences else 1


def validate(preset: str, output: Path | None) -> int:
    output = evidence_directory("validate-" + preset, output, allow_empty=True)
    for directory in ("src", "include", "apps", "tests", "benchmarks", "scripts"):
        if output.is_relative_to(ROOT / directory):
            raise ValueError("validation evidence must be outside authored-input directories")
    if output.exists() and any(output.iterdir()):
        raise ValueError("evidence output directory is not empty; choose a new path to preserve previous records")
    output.mkdir(parents=True, exist_ok=True)
    before = manifest()
    write_json(output / "source-manifest.json", before)
    machine = hardware()
    build = ROOT / "out" / preset
    report_path = output / "ctest-results.xml"
    commands = [("configure", ["cmake", "--preset", preset], 180),
                ("build", ["cmake", "--build", "--preset", preset], 300),
                ("identity", [str(build / "race_headless"), "--identity"], 30),
                ("ctest", ["ctest", "--preset", preset, "--output-junit", str(report_path)], 300)]
    steps = []
    permitted = True
    embedded_identity = None
    for name, command, timeout in commands:
        if permitted:
            step = run_step(name, command, output, timeout)
            permitted = step["status"] == "passed"
            if name == "identity" and permitted:
                try:
                    embedded_identity = json.loads((output / "identity.log").read_text())
                    expected = {"source_commit": before["git"]["commit"],
                                "authored_input_sha256": before["authored_inputs"]["sha256"],
                                "configuration": "Release" if preset == "release" else "Debug",
                                "sanitizer": {"asan": "address", "tsan": "thread"}.get(preset, "none")}
                    if not isinstance(embedded_identity, dict) or any(embedded_identity.get(key) != value for key, value in expected.items()):
                        permitted = False
                        step["status"] = "failed"
                        step["error"] = "embedded executable identity does not match source/build configuration"
                except (OSError, ValueError):
                    permitted = False
                    step["status"] = "failed"
                    step["error"] = "embedded executable identity is malformed"
        else:
            step = {"name": name, "command": command, "status": "not_run", "reason": "prior required step failed", "exit_code": None}
        steps.append(step)
    after = {"git": git_state(), "authored_inputs": authored_inputs()}
    write_json(output / "source-after.json", after)
    unchanged = before["authored_inputs"]["sha256"] == after["authored_inputs"]["sha256"]
    commit_unchanged = before["git"]["commit"] == after["git"]["commit"]
    tests = junit_summary(report_path)
    artifacts = []
    for name in EXECUTABLES:
        path = build / name
        if path.is_file():
            artifacts.append({"name": name, "path": str(path.resolve()), "bytes": path.stat().st_size,
                              "sha256": file_sha256(path),
                              "built_with_passing_contract_suite": permitted and tests["status"] == "passed" and unchanged and commit_unchanged,
                              "executed_in_this_run":
                                  (name == "race_headless" and any(step["name"] == "identity" and step.get("exit_code") is not None for step in steps)) or
                                  (name == "race_tests" and any(test_name in {"platform", "memory", "identity", "jobs", "time", "config", "diagnostics", "lifecycle", "mock_io"} for test_name in tests.get("names", []))) or
                                  (name == "race_scheduler_workloads" and "scheduler_workloads" in tests.get("names", [])) or
                                  (name == "race_scene_identity_tests" and "scene_identity" in tests.get("names", [])) or
                                  (name == "race_scene_tests" and "scene" in tests.get("names", [])) or
                                  (name == "race_scene_headless" and "scene_headless" in tests.get("names", [])) or
                                  (name == "race_scene_workloads" and "scene_workloads" in tests.get("names", [])) or
                                  (name in {"race_assets_tool", "race_assets_headless"} and "assets_workflow" in tests.get("names", [])) or
                                  (name == "race_assets_tests" and all(n in tests.get("names", []) for n in ("assets_wire", "assets_runtime", "assets_faults"))) or
                                  (name == "race_assets_fuzz" and "assets_parser_corpus" in tests.get("names", [])) or
                                  (name == "race_render_tests" and "render_contracts" in tests.get("names", [])) or
                                  (name == "race_render_fixture" and "render_fixture" in tests.get("names", []))})
    evidence_files = [{"path": str(path.resolve()), "sha256": file_sha256(path)}
                      for path in sorted(output.iterdir()) if path.is_file()]
    accepted = permitted and tests["status"] == "passed" and unchanged and commit_unchanged and len(artifacts) == len(EXECUTABLES)
    receipt = {"schema": 1, "kind": "race-artifact-receipt", "recorded_utc": datetime.now(timezone.utc).isoformat(),
               "preset": preset, "source_root": str(ROOT), "source_git": before["git"], "source_git_after": after["git"],
               "authored_input_sha256": before["authored_inputs"]["sha256"], "authored_inputs_unchanged": unchanged,
               "source_commit_unchanged": commit_unchanged,
               "accepted": accepted, "steps": steps, "tests": tests, "artifacts": artifacts,
               "embedded_identity": embedded_identity,
               "build_configuration": compile_configuration(build), "environment": before["environment"],
               "hardware": machine, "evidence_files": evidence_files,
               "limitations": ["Linux checks only; other native/mobile platforms are untested.",
                               "This is correctness validation, not a performance baseline or a product release.",
                               "Build evidence is local JSON with SHA256 integrity checks, not a signed attestation.",
                               "No environment-variable dump or credential store is included."]}
    write_json(output / "artifact-receipt.json", receipt)
    print(json.dumps({"accepted": accepted, "receipt": str(output / "artifact-receipt.json"),
                      "commit": before["git"]["commit"], "authored_input_sha256": before["authored_inputs"]["sha256"],
                      "authored_inputs_unchanged": unchanged}, indent=2))
    return 0 if accepted else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preset", choices=PRESETS)
    parser.add_argument("--output", type=Path, help="new empty local evidence directory")
    parser.add_argument("--verify", type=Path, metavar="RECEIPT", help="verify source/artifact/evidence hashes without rebuilding")
    arguments = parser.parse_args()
    if arguments.verify and (arguments.preset or arguments.output):
        parser.error("--verify cannot be combined with --preset or --output")
    if not arguments.verify and not arguments.preset:
        parser.error("specify --preset or --verify")
    try:
        if arguments.verify:
            return verify(arguments.verify)
        return validate(arguments.preset, arguments.output)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"validation failed: {type(error).__name__}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

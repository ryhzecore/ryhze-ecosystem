"""Make and verify a local Windows developer package; no upload or installer."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile
from source_manifest import authored_inputs, git_state
from output_paths import candidate_output, candidate_path, evidence_directory

ROOT = Path(__file__).resolve().parents[1]
PROGRAMS = ("race_headless", "race_scene_headless", "race_assets_tool",
            "race_assets_headless", "race_audio_demo", "race_vfx_demo",
            "race_atmosphere_demo", "race_effects_headless")
COMMANDS = []
COMMAND_RECEIPT = None


def run(command, timeout=60):
    record = {"command": [str(x) for x in command]}
    try:
        result = subprocess.run(record["command"], capture_output=True,
                                text=True, errors="replace", timeout=timeout)
        record.update(exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr)
        if result.returncode:
            raise RuntimeError(f"{command[0]} exited {result.returncode}: {result.stderr}")
        return record
    except (OSError, subprocess.SubprocessError) as error:
        record.update(error=f"{type(error).__name__}: {error}")
        for name in ("stdout", "stderr"):
            value = getattr(error, name, None)
            if value is not None:
                record[name] = value.decode(errors="replace") if isinstance(value, bytes) else value
        raise
    finally:
        COMMANDS.append(record)
        if COMMAND_RECEIPT is not None:
            COMMAND_RECEIPT.write_text(json.dumps(COMMANDS, indent=2) + "\n", encoding="utf-8")


def output_paths(requested, evidence):
    # Resolve the parent only: resolving a leaf symlink could hide an existing
    # output and redirect a supposedly new package through that link.
    requested = candidate_path(requested)
    if requested.suffix.lower() != ".zip":
        raise ValueError("Package output must have a .zip suffix")
    output = requested.parent.resolve() / requested.name
    unpacked = evidence / "extracted"
    receipt = evidence / "validation.json"
    for path in (output, unpacked, receipt):
        if path.exists() or path.is_symlink():
            raise ValueError(f"Package output already exists; choose a new output name: {path}")
    return output, unpacked, receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, help="new ZIP below Releases/candidates/<commit-or-version>")
    parser.add_argument("--evidence", type=Path, help="new Evidence directory for extraction and startup checks")
    args = parser.parse_args()
    evidence = evidence_directory("package-windows", args.evidence)
    attempt = {"schema": 1, "kind": "race-package-attempt", "status": "running",
               "started_utc": datetime.now(timezone.utc).isoformat(),
               "build": str(args.build.resolve()), "requested_package": str(args.output) if args.output else None,
               "evidence": str(evidence)}
    attempt_path = evidence / "attempt.json"
    attempt_path.write_text(json.dumps(attempt, indent=2) + "\n", encoding="utf-8")
    try:
        package = package_attempt(args, evidence)
        attempt.update(status="passed", package=package["package"], validation_receipt=str(evidence / "validation.json"))
    except BaseException as error:
        attempt.update(status="interrupted" if isinstance(error, KeyboardInterrupt) else "failed",
                       error=f"{type(error).__name__}: {error}")
        raise
    finally:
        attempt["finished_utc"] = datetime.now(timezone.utc).isoformat()
        attempt_path.write_text(json.dumps(attempt, indent=2) + "\n", encoding="utf-8")


def package_attempt(args, evidence):
    global COMMAND_RECEIPT
    COMMANDS.clear()
    COMMAND_RECEIPT = evidence / "commands.json"
    build = args.build.resolve()
    state = git_state(ROOT)
    requested = candidate_output(state["commit"], args.output, leaf="headless-preview.zip")
    output, unpacked, receipt_path = output_paths(requested, evidence)
    executables = {program: build / (program + ".exe") for program in PROGRAMS}
    for path in executables.values():
        if not path.is_file() or path.is_symlink():
            raise ValueError(f"Missing regular executable: {path}")
    identity = json.loads(run([executables["race_scene_headless"], "--identity"])["stdout"])
    current = authored_inputs(ROOT)
    if identity["configuration"] != "Release":
        raise ValueError("Only a Release build is packaged")
    if identity["authored_input_sha256"] != current["sha256"] or identity["source_commit"] != state["commit"]:
        raise ValueError("Build identity differs from current source; rebuild first")
    # Reject stale independently built targets wherever their public CLI exposes
    # identity. The remaining demos are qualified by packaged-byte smoke checks.
    for program in ("race_headless", "race_effects_headless"):
        observed = json.loads(run([executables[program], "--identity"])["stdout"])
        for field in ("source_commit", "authored_input_sha256"):
            if observed[field] != identity[field]:
                raise ValueError(f"Executable identity differs: {program}")
        if "configuration" in observed and observed["configuration"] != "Release":
            raise ValueError(f"Executable is not a Release build: {program}")
    tool_identity = run([executables["race_assets_tool"], "--identity"])["stdout"].split()
    if tool_identity != [identity["source_commit"], identity["authored_input_sha256"]]:
        raise ValueError("Executable identity differs: race_assets_tool")
    payload = {path.name: path.read_bytes() for path in executables.values()}
    if authored_inputs(ROOT)["sha256"] != current["sha256"] or git_state(ROOT)["commit"] != state["commit"]:
        raise ValueError("Source changed during packaging; rebuild and choose a new output")
    payload["README.txt"] = (
        "RACE local Windows x64 developer increment\n"
        "Run race_headless.exe --mode game, film or authority.\n"
        "Scene/audio/VFX integration: race_effects_headless.exe --output-dir OUTPUT_DIRECTORY.\n"
        "Offline demos: race_atmosphere_demo.exe OUTPUT_DIRECTORY; "
        "race_audio_demo.exe OUTPUT.wav; race_vfx_demo.exe OUTPUT_DIRECTORY.\n"
        "These are bounded CPU/headless workflows, not the Qt editor or a shipping renderer.\n"
        "Requires the MSVC x64 runtime already installed on the validation machine. "
        "No CRT redistributable, external content, installer, signing or update mechanism is included.\n"
        "Local development artifact only; not a production release or redistribution licence.\n"
    ).encode()
    manifest = {"schema": 1, "kind": "race-windows-developer-package", "identity": identity,
                "git": state,
                "identity_checked_executables": ["race_scene_headless", "race_headless",
                                                 "race_effects_headless", "race_assets_tool"],
                "files": [{"path": name, "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest()} for name, data in sorted(payload.items())]}
    payload["manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(payload.items()):
            info = zipfile.ZipInfo(name, (2026, 10, 10, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data, compresslevel=9)
    unpacked.mkdir(exist_ok=False)
    with zipfile.ZipFile(output) as archive:
        names = archive.namelist()
        if archive.testzip() is not None or len(names) != len(payload) or set(names) != set(payload):
            raise ValueError("Archive integrity or member inventory failed")
        for name, expected in payload.items():
            if archive.read(name) != expected:
                raise ValueError(f"Packaged file integrity mismatch: {name}")
        # All members were generated above as single-leaf names, never external archives.
        archive.extractall(unpacked)
    startup = evidence / "startup"
    startup.mkdir(exist_ok=False)
    checks = [run([unpacked / "race_headless.exe", "--mode", mode])
              for mode in ("game", "film", "authority")]
    checks.append(run([unpacked / "race_scene_headless.exe", "--output", startup / "startup-scene.json"]))
    checks.append(run([unpacked / "race_effects_headless.exe", "--output-dir", startup / "startup-effects"]))
    checks.append(run([unpacked / "race_audio_demo.exe", startup / "startup-audio.wav"]))
    checks.append(run([unpacked / "race_vfx_demo.exe", startup / "startup-vfx"]))
    checks.append(run([unpacked / "race_atmosphere_demo.exe", startup / "startup-atmosphere"]))
    assets = startup / "startup-assets"
    assets.mkdir(exist_ok=False)
    fixture = startup / "startup-asset.txt"
    asset_id = f"{77:016x}{1:016x}"
    with fixture.open("x", encoding="utf-8") as stream:
        stream.write(f"RACEFIXTURE1\nid {asset_id}\nseed 1\ncount 16\nsettings 0\nenvironment 1\nhard 0\nsoft 0\n")
    checks.append(run([unpacked / "race_assets_tool.exe", "cook", assets, fixture]))
    checks.append(run([unpacked / "race_assets_tool.exe", "validate", assets]))
    checks.append(run([unpacked / "race_assets_headless.exe", assets, asset_id]))
    observed = json.loads(checks[-1]["stdout"])
    if observed["source"] != identity["source_commit"] or observed["authored_sha256"] != identity["authored_input_sha256"]:
        raise ValueError("Packaged assets workflow identity differs")
    receipt = {"package": str(output), "bytes": output.stat().st_size,
               "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
               "inventory_and_zip_integrity": "passed", "extracted": str(unpacked),
               "startup_checks": checks, "evidence": str(evidence), "qualification": "local developer package; no GPU/editor/release claim"}
    # Exclusive creation also closes the race with another writer after preflight.
    with receipt_path.open("x", encoding="utf-8") as stream:
        stream.write(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({key: value for key, value in receipt.items() if key != "startup_checks"}))
    return receipt


if __name__ == "__main__":
    main()

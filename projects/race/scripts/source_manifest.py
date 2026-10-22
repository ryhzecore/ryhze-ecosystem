#!/usr/bin/env python3
"""Bounded local source/toolchain provenance; never reads credential stores."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIRECTORIES = ("src", "include", "apps", "tests", "benchmarks", "scripts", "shaders")
SOURCE_SUFFIXES = {".cpp", ".hpp", ".py", ".json", ".cmake", ".ps1", ".hlsl"}
EDITOR_FONT_DIRECTORY = Path("apps/resources/fonts")
EDITOR_FONT_SUFFIXES = {".qrc", ".ttf", ".txt"}
ROOT_INPUTS = ("CMakeLists.txt", "CMakePresets.json")


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def authored_inputs(root: Path = ROOT) -> dict:
    paths: set[Path] = set()
    for directory in SOURCE_DIRECTORIES:
        base = root / directory
        if base.is_dir():
            for path in base.rglob("*"):
                if "__pycache__" in path.parts or not path.is_file():
                    continue
                editor_font = (path.is_relative_to(root / EDITOR_FONT_DIRECTORY)
                               and path.suffix in EDITOR_FONT_SUFFIXES)
                if path.suffix in SOURCE_SUFFIXES or path.name in {"CMakeLists.txt"} or path.name.endswith(".hpp.in") or editor_font:
                    paths.add(path)
    for name in ROOT_INPUTS:
        if (root / name).is_file():
            paths.add(root / name)
    files = []
    for path in sorted(paths, key=lambda item: item.relative_to(root).as_posix()):
        if path.is_symlink():
            # Do not let an authored symlink silently draw untracked external
            # contents into a build or pretend its identity is a regular file.
            raise ValueError(f"authored input must not be a symlink: {path.relative_to(root)}")
        files.append({"path": path.relative_to(root).as_posix(),
                      "bytes": path.stat().st_size, "sha256": file_sha256(path)})
    payload = {"schema": 1, "files": files}
    serialized = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return {"algorithm": "sha256", "canonicalization": "sorted compact JSON of schema and file records",
            "sha256": hashlib.sha256(serialized).hexdigest(), "file_count": len(files), "files": files,
            "scope": {"directories": list(SOURCE_DIRECTORIES), "suffixes": sorted(SOURCE_SUFFIXES),
                      "additional_template_suffix": ".hpp.in", "nested_cmake_inputs": "CMakeLists.txt", "root_inputs": list(ROOT_INPUTS),
                      "editor_font_inputs": {"directory": EDITOR_FONT_DIRECTORY.as_posix(),
                                             "suffixes": sorted(EDITOR_FONT_SUFFIXES)},
                      "excluded": ["documentation", "out", "evidence", "__pycache__"]}}


def bounded_command(command: list[str], root: Path = ROOT, timeout: float = 15) -> dict:
    try:
        result = subprocess.run(command, cwd=root, capture_output=True, text=True,
                                errors="replace", timeout=timeout, check=False)
        text = result.stdout + result.stderr
        return {"command": command, "exit_code": result.returncode,
                "output": text[:16384], "output_truncated": len(text) > 16384}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"command": command, "exit_code": None, "unavailable": type(error).__name__}


def git_state(root: Path = ROOT) -> dict:
    def git(*arguments: str) -> str:
        result = subprocess.run(["git", "-c", f"safe.directory={root.resolve()}", *arguments],
                                cwd=root, capture_output=True,
                                text=True, errors="replace", timeout=15, check=False)
        if result.returncode != 0:
            raise RuntimeError("Git identity query failed")
        return result.stdout.rstrip("\n")
    commit = git("rev-parse", "--verify", "HEAD")
    if len(commit) != 40 or any(char not in "0123456789abcdef" for char in commit):
        raise RuntimeError("Git did not provide a full SHA-1 commit identity")
    branch = git("branch", "--show-current") or None
    status = git("status", "--porcelain=v1", "--untracked-files=all")
    return {"commit": commit, "branch": branch, "detached": branch is None,
            "working_tree_clean": not status, "status_porcelain_v1": status,
            "status_scope": "all tracked changes and all non-ignored untracked files; ignored build/evidence output excluded"}


def environment(root: Path = ROOT) -> dict:
    release = {}
    try:
        for line in Path("/etc/os-release").read_text().splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                if key in {"ID", "VERSION_ID", "PRETTY_NAME", "BUILD_ID"}:
                    release[key] = value.strip('"')
    except OSError:
        release = {"unavailable": True}
    commands = {
        "compiler": ["clang++", "--version"], "cmake": ["cmake", "--version"],
        "ninja": ["ninja", "--version"], "git": ["git", "--version"],
        "glibc": ["getconf", "GNU_LIBC_VERSION"], "dynamic_loader": ["ldd", "--version"],
        "cxx_runtime_path": ["clang++", "-print-file-name=libstdc++.so"],
        "packages": ["dpkg-query", "-W", "-f=${Package} ${Version}\n", "libc6", "libstdc++6", "clang-18", "cmake", "ninja-build",
                     "libclang-rt-18-dev", "lib32gcc-s1", "lib32stdc++6", "libc6-i386"],
    }
    if os.name == "nt":
        release = {"ID": "windows", "VERSION_ID": platform.version(), "PRETTY_NAME": platform.platform()}
        commands = {"compiler": ["cl"], "cmake": ["cmake", "--version"],
                    "ninja": ["ninja", "--version"], "git": ["git", "--version"]}
    host_release = {}
    try:
        for line in Path("/run/host/etc/os-release").read_text().splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                if key in {"ID", "VERSION_ID", "PRETTY_NAME", "BUILD_ID"}:
                    host_release[key] = value.strip('"')
    except OSError:
        host_release = {"unavailable": True, "note": "Host release is not exposed at /run/host/etc/os-release; container OS does not identify its host distribution."}
    return {"os_release": release, "host_os_release_if_exposed": host_release, "kernel_release": platform.release(), "machine": platform.machine(),
            "python": sys.version, "tools": {name: bounded_command(command, root) for name, command in commands.items()},
            "third_party_source_dependencies": [],
            "optional_desktop_binary_dependency_lock": "scripts/desktop_dependencies.lock.json"
                if (root / "scripts/desktop_dependencies.lock.json").is_file() else None,
            "dependency_note": "Portable targets use the system C++ runtime, OS services, threads and Python tooling. Optional Windows desktop targets additionally use pinned Qt/SDL binaries and Windows SDK DXC; consult the dependency lock and the selected build configuration."}


def manifest(root: Path = ROOT) -> dict:
    return {"schema": 1, "kind": "race-authored-source-manifest",
            "recorded_utc": datetime.now(timezone.utc).isoformat(), "source_root": str(root.resolve()),
            "git": git_state(root), "authored_inputs": authored_inputs(root), "environment": environment(root),
            "identity_note": "Git commit and local status identify checkout history; authored digest identifies build inputs. Neither alone proves a tested artifact."}


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--identity", action="store_true", help="print authored-input SHA256 only; no toolchain probes")
    parser.add_argument("--output", type=Path, help="write a full JSON manifest")
    arguments = parser.parse_args()
    if arguments.identity and arguments.output:
        parser.error("--identity and --output are mutually exclusive")
    try:
        if arguments.identity:
            print(authored_inputs()["sha256"])
        elif arguments.output:
            write_json(arguments.output, manifest())
            print(str(arguments.output.resolve()))
        else:
            print(json.dumps(manifest(), indent=2, sort_keys=True))
        return 0
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f"source manifest failed: {type(error).__name__}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

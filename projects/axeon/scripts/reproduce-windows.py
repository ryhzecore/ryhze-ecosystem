#!/usr/bin/env python3
"""Independent native Windows reproduction phases. Run each under host admission.

build extracts one identified Git archive into a fresh source directory and
builds once. qualify runs its examples; compare requires two independent builds.
No Git checkout, prior attempt, tool installation or accepted package is changed.
"""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import time
import tomllib

TARGET = "x86_64-pc-windows-msvc"
BINARIES = ("axeon.exe", "axeon-process.exe", "axeon_tensor_runtime.lib")
MAX_FILE = 512 << 20
MAX_ARCHIVE = 4 << 30
MAX_FILES = 20000
MAX_SOURCE = 2 << 30
SOURCE_DIRS = {"src", "crates", "runtime", "examples", "docs", "scripts", ".cargo"}
SOURCE_NAMES = {"Cargo.toml", "Cargo.lock", "rust-toolchain.toml", "toolchain-lock.json",
                "build.rs", "README.md", "AGENTS.md", "LICENSE", "LICENSE.md", "LICENSE.txt",
                "NOTICE", "NOTICE.md", "COPYING", ".gitattributes"}
MSVC_ARGUMENT_ENV = ("CL", "_CL_", "LINK", "_LINK_")
NATIVE_ENVIRONMENT = ("PATH", "INCLUDE", "LIB", "LIBPATH", "VCTOOLSINSTALLDIR", "VCTOOLSVERSION",
                      "WINDOWSSDKDIR", "WINDOWSSDKVERSION", "WINDOWSSDKLIBVERSION",
                      "UNIVERSALCRTSDKDIR", "UCRTVERSION", "WINDOWSLIBPATH", "VSINSTALLDIR",
                      "VCINSTALLDIR", "VSCMD_ARG_TGT_ARCH")


def need(condition, message):
    if not condition:
        raise RuntimeError(message)


def native_host():
    need(os.name == "nt" and platform.machine().lower() in ("amd64", "x86_64"),
         "native Windows x64 is required; WSL is not a Windows qualification")


def safe_path(value):
    path = Path(os.path.abspath(value))
    for current in reversed((path, *path.parents)):
        try:
            info = current.lstat()
        except FileNotFoundError:
            continue
        need(not stat.S_ISLNK(info.st_mode) and not getattr(info, "st_file_attributes", 0) & 0x400,
             f"reparse/symlink paths are forbidden: {current}")
    return path


def regular(value, maximum=MAX_FILE):
    path = safe_path(value)
    info = path.stat()
    need(stat.S_ISREG(info.st_mode) and info.st_size <= maximum,
         f"expected bounded regular file ({maximum} bytes): {path}")
    return path


def sha(value, maximum=MAX_FILE):
    path = regular(value, maximum)
    before = path.stat()
    total, digest = 0, hashlib.sha256()
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        need((before.st_dev, before.st_ino) == (opened.st_dev, opened.st_ino), f"input replaced: {path}")
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            total += len(chunk)
            need(total <= maximum, f"input grew beyond limit: {path}")
            digest.update(chunk)
        after = os.fstat(stream.fileno())
    current = safe_path(path).stat()
    need((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
         (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) ==
         (current.st_dev, current.st_ino, current.st_size, current.st_mtime_ns) and total == before.st_size,
         f"input changed while hashing: {path}")
    return digest.hexdigest()


def load(value):
    path = regular(value, 32 << 20)
    def pairs(items):
        result = {}
        for key, item in items:
            need(key not in result, f"duplicate JSON key {key}")
            result[key] = item
        return result
    def invalid(value):
        raise RuntimeError(f"nonfinite JSON value {value}")
    return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=pairs, parse_constant=invalid)


def write_json(path, value):
    safe_path(path)
    temporary = path.with_name(path.name + ".writing")
    with temporary.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=True, allow_nan=False)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def fresh(value):
    path = safe_path(value)
    path.mkdir(parents=False, exist_ok=False)
    return path


def evidence_location(value):
    path = safe_path(value)
    base = safe_path("C:/Projects/Axeon/Evidence")
    need(path.is_relative_to(base), "new run outputs must be under Axeon/Evidence")
    parts = path.relative_to(base).parts
    need(len(parts) >= 4 and re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", parts[0])
         and re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", parts[1])
         and re.fullmatch(r"attempt-[0-9]{2,}", parts[2]), "use a child of a reserved dated Evidence attempt")
    return path


def relative_name(value):
    need(isinstance(value, str) and value, "relative name must be a nonempty string")
    name = PurePosixPath(value)
    need(isinstance(value, str) and value and not name.is_absolute() and "\\" not in value
         and ":" not in value and not any(ord(c) < 32 for c in value), f"unsafe relative name: {value!r}")
    need(all(part not in ("", ".", "..") and not part.endswith((".", " "))
             and not re.fullmatch(r"(?i)(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?", part)
             for part in value.split("/")), f"unsafe Windows name: {value!r}")
    return name.as_posix()


def selected(value):
    return value in SOURCE_NAMES or PurePosixPath(value).parts[0] in SOURCE_DIRS


def extract_archive(archive, expected_sha, commit, source):
    need(re.fullmatch(r"[0-9a-f]{40}", commit) is not None, "full Git source commit required")
    need(re.fullmatch(r"[0-9a-f]{64}", expected_sha) is not None, "archive SHA-256 required")
    need(sha(archive, MAX_ARCHIVE) == expected_sha, "source archive hash differs")
    source.mkdir()
    files, seen, total, count = {}, set(), 0, 0
    with tarfile.open(regular(archive, MAX_ARCHIVE), mode="r:*") as packed:
        need(packed.pax_headers.get("comment") == commit,
             "archive must be a Git-generated tar with its full commit in the global PAX comment")
        for item in packed:
            count += 1
            need(count <= MAX_FILES, "source archive entry budget exceeded")
            name = relative_name(item.name.rstrip("/") if item.isdir() else item.name)
            folded = name.casefold()
            need(folded not in seen, f"duplicate/case-aliased archive member: {name}")
            seen.add(folded)
            need(item.isdir() or item.isfile(), f"nonregular archive member: {name}")
            if item.isdir() or not selected(name):
                continue
            need(0 <= item.size <= MAX_FILE, f"source member too large: {name}")
            total += item.size
            need(total <= MAX_SOURCE, "selected source archive bytes exceed budget")
            destination = safe_path(source / name)
            destination.parent.mkdir(parents=True, exist_ok=True)
            digest, actual = hashlib.sha256(), 0
            with packed.extractfile(item) as src, destination.open("xb") as dst:
                for chunk in iter(lambda: src.read(1 << 20), b""):
                    actual += len(chunk)
                    need(actual <= item.size, "archive member grew beyond declared size")
                    dst.write(chunk)
                    digest.update(chunk)
            need(actual == item.size, f"truncated archive member: {name}")
            files[name] = digest.hexdigest()
    for required in ("Cargo.toml", "Cargo.lock", "runtime/windows_process.c", "src/main.rs",
                     "scripts/record-native-examples.py", "examples/generic-nominals/main.ax"):
        need(required in files, f"archive misses required source input: {required}")
    need(sha(archive, MAX_ARCHIVE) == expected_sha, "archive changed during extraction")
    return files


def verify_source(source, files):
    source = safe_path(source)
    need(isinstance(files, dict) and 0 < len(files) <= MAX_FILES, "invalid source file map")
    actual = set()
    for parent, dirs, names in os.walk(source, followlinks=False):
        for name in dirs:
            safe_path(Path(parent) / name)
        for name in names:
            item = Path(parent) / name
            relative = relative_name(item.relative_to(source).as_posix())
            actual.add(relative)
            need(len(actual) <= MAX_FILES and relative in files, f"unexpected source input: {relative}")
            need(sha(item) == files[relative], f"frozen source mismatch: {relative}")
    need(actual == set(files), "frozen source file map is incomplete")


def pe(path):
    with regular(path).open("rb") as stream:
        header = stream.read(64)
        need(len(header) == 64 and header[:2] == b"MZ", f"not a Windows PE: {path}")
        offset = struct.unpack_from("<I", header, 60)[0]
        need(64 <= offset <= 1 << 20, "invalid PE header offset")
        stream.seek(offset)
        signature = stream.read(26)
        need(len(signature) == 26 and signature[:6] == b"PE\0\0\x64\x86"
             and signature[24:26] == b"\x0b\x02", f"not an x64 PE32+ image: {path}")


def expected_examples(source):
    tree = ast.parse((source / "scripts/record-native-examples.py").read_text(encoding="utf-8"))
    expected = next(ast.literal_eval(node.value) for node in tree.body if isinstance(node, ast.Assign)
                    and any(isinstance(t, ast.Name) and t.id == "EXPECTED" for t in node.targets))
    need("generic-nominals/main" in expected and len(expected) >= 19, "current native oracle coverage required")
    for name, value in expected.items():
        relative_name(name)
        need(isinstance(value, tuple) and len(value) == 3 and type(value[0]) is int
             and all(isinstance(x, str) for x in value[1:]), "invalid native oracle")
    return expected


def execute(supervisor, command, cwd, env, output, checks, seconds=120, expected=None):
    label = f"{len(checks):03d}"
    entry = {"command": list(map(str, command)), "cwd": str(cwd), "supervisor": str(supervisor),
             "seconds_limit": seconds, "passed": False}
    checks.append(entry)
    started, child = time.monotonic(), None
    stdout, stderr = output / (label + ".stdout"), output / (label + ".stderr")
    with stdout.open("xb") as out, stderr.open("xb") as err:
        try:
            child = subprocess.Popen([str(supervisor), "--timeout-seconds", str(seconds), "--",
                                      *map(str, command)], cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                     stdout=out, stderr=err)
            code = child.wait(timeout=seconds + 10)
        finally:
            if child is not None and child.poll() is None:
                child.kill()
                child.wait(timeout=10)
    entry.update(exit_code=code, elapsed_seconds=time.monotonic() - started,
                 stdout_sha256=sha(stdout), stderr_sha256=sha(stderr))
    need(stdout.stat().st_size <= 16 << 20 and stderr.stat().st_size <= 16 << 20,
         "command output exceeds bounded receipt reading")
    out, err = stdout.read_bytes(), stderr.read_bytes()
    if expected is None:
        need(code == 0, f"command failed ({code}); see {stderr}")
    else:
        need((code, out, err) == expected, f"command result differs; see {stdout} and {stderr}")
    entry["passed"] = True
    return out, err


def tool_path(name):
    found = shutil.which(name)
    need(found is not None, f"required native tool missing: {name}")
    return regular(found)


def native_environment(pinned=None):
    env = {name.upper(): value for name, value in os.environ.items()}
    for name in MSVC_ARGUMENT_ENV:
        env.pop(name, None)
    if pinned is not None:
        need(isinstance(pinned, dict) and set(pinned) == set(NATIVE_ENVIRONMENT),
             "complete pinned native dependency environment required")
        for name, value in pinned.items():
            need(value is None or isinstance(value, str), f"invalid native environment value: {name}")
            if value is None:
                env.pop(name, None)
            else:
                env[name] = value
    need(env.get("INCLUDE") and env.get("LIB"), "native MSVC INCLUDE and LIB environment required")
    return env


def environment(root, source, clang, rustc):
    env = native_environment()
    for name in list(env):
        if name.startswith(("CARGO_PROFILE_", "CARGO_TARGET_")) or name in ("RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS",
                "RUSTC_WRAPPER", "RUSTC_WORKSPACE_WRAPPER", "CARGO_BUILD_TARGET", "CARGO_TARGET_DIR",
                "GIT_DIR", "GIT_WORK_TREE", "RUSTC_BOOTSTRAP"):
            env.pop(name)
    env.update(CARGO_BUILD_JOBS="1", RUST_TEST_THREADS="1", CARGO_INCREMENTAL="0",
               SOURCE_DATE_EPOCH="0", CARGO_HOME=str(root / "cargo-home"),
               CARGO_TARGET_DIR=str(root / "target"), RUSTC=str(rustc), AXEON_CLANG=str(clang),
               RUSTC_WRAPPER="", RUSTC_WORKSPACE_WRAPPER="",
               CARGO_ENCODED_RUSTFLAGS="\x1f".join((f"--remap-path-prefix={root}=/axeon",
                                                   "-C", "link-arg=/Brepro")))
    return env


def build(args):
    root = fresh(evidence_location(args.output_dir))
    receipt = {"schema_version": 1, "kind": "axeon-windows-build", "status": "running",
               "source_commit": args.source_commit, "archive_sha256": args.archive_sha256,
               "target": TARGET, "root": str(root), "checks": [], "no_shared_target_directory": True,
               "wsl_invoked_by_harness": False, "recipe_sha256": sha(__file__)}
    path = root / "build.json"
    write_json(path, receipt)
    try:
        supervisor = regular(args.supervisor)
        pe(supervisor)
        source = root / "source"
        files = extract_archive(args.archive, args.archive_sha256, args.source_commit, source)
        receipt.update(source=str(source), source_files=files, supervisor_sha256=sha(supervisor))
        source_version = tomllib.loads((source / "Cargo.toml").read_text(encoding="utf-8"))["package"]["version"]
        receipt["compiler_version"] = source_version
        tools = {name: tool_path(name + ".exe") for name in ("rustc", "cargo", "cl", "link", "lld-link")}
        tools["clang"] = regular(os.environ.get("AXEON_CLANG") or tool_path("clang.exe"))
        env = environment(root, source, tools["clang"], tools["rustc"])
        env["CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_LINKER"] = str(tools["link"])
        logs = fresh(root / "logs")
        def run(command, **kwargs):
            try:
                return execute(supervisor, command, source, env, logs, receipt["checks"], **kwargs)
            finally:
                write_json(path, receipt)
        rust = run([tools["rustc"], "-vV"])[0].decode("utf-8")
        clang = run([tools["clang"], "--version"])[0].decode("utf-8")
        cargo = run([tools["cargo"], "--version"])[0].decode("utf-8")
        need(re.search(r"(?m)^release: 1\.85\.0$", rust) and f"host: {TARGET}" in rust,
             "native Rust1.85.0 MSVC required")
        need(re.match(r"clang version 19\.1\.7(?:\s|$)", clang), "Clang19.1.7 required")
        need(cargo.startswith("cargo 1.85.0 "), "Cargo1.85.0 required")
        lock = load(source / "toolchain-lock.json")["windows_candidate"]
        need(os.environ.get("VCToolsVersion", "").rstrip("\\/") == lock["msvc"], "MSVC version differs from lock")
        need(os.environ.get("WindowsSDKVersion", "").rstrip("\\/") == lock["windows_sdk"], "Windows SDK differs from lock")
        receipt["toolchain"] = {"rust": rust, "clang": clang, "cargo": cargo,
            "tools": {name: {"path": str(value), "sha256": sha(value)} for name, value in tools.items()},
            "msvc": lock["msvc"], "windows_sdk": lock["windows_sdk"],
            "native_environment": {name: env.get(name) for name in NATIVE_ENVIRONMENT},
            "removed_argument_environment": list(MSVC_ARGUMENT_ENV),
            "rust_flags": ["--remap-path-prefix=<independent-root>=/axeon", "-C", "link-arg=/Brepro"],
            "jobs": 1, "incremental": False, "offline": True, "locked": True}
        run([tools["cargo"], "build", "--offline", "--locked", "--release", "--workspace", "--jobs", "1", "--target", TARGET], seconds=300)
        binaries = root / "target" / TARGET / "release"
        run([tools["cl"], "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/MT", "/Brepro",
             "runtime/windows_process.c", f"/Fe:{binaries / 'axeon-process.exe'}",
             f"/Fo:{root / 'windows_process.obj'}", "/link", "/Brepro"], seconds=30)
        artifacts = {f"bin/{name}": {"path": str(binaries / name), "sha256": sha(binaries / name)} for name in BINARIES}
        for name in BINARIES[:2]:
            pe(binaries / name)
        with (binaries / BINARIES[2]).open("rb") as archive:
            need(archive.read(8) == b"!<arch>\n", "runtime is not a static archive")
        verify_source(source, files)
        need(all(sha(value) == receipt["toolchain"]["tools"][name]["sha256"] for name, value in tools.items()), "tool changed during build")
        receipt.update(status="passed", artifacts=artifacts)
    except BaseException as error:
        receipt.update(status="failed", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        write_json(path, receipt)
    print(json.dumps({"status": receipt["status"], "receipt": str(path)}))


def validated_build(value):
    receipt = load(value)
    need(receipt.get("kind") == "axeon-windows-build" and receipt.get("status") == "passed"
         and receipt.get("target") == TARGET and receipt.get("no_shared_target_directory") is True
         and receipt.get("wsl_invoked_by_harness") is False, "successful native Windows build required")
    verify_source(receipt["source"], receipt["source_files"])
    need(set(receipt["artifacts"]) == {f"bin/{name}" for name in BINARIES}, "incomplete build artifacts")
    for item in receipt["artifacts"].values():
        need(sha(item["path"]) == item["sha256"], "reproduced artifact bytes differ")
    return receipt


def qualify(args):
    build_record = validated_build(args.build_receipt)
    root = fresh(evidence_location(args.output_dir))
    source = Path(build_record["source"])
    binaries = {name: Path(build_record["artifacts"][f"bin/{name}"]["path"]) for name in BINARIES}
    result = {"schema_version": 1, "kind": "axeon-windows-qualification", "status": "running",
              "root": str(root),
              "source_commit": build_record["source_commit"], "build_receipt": str(regular(args.build_receipt)),
              "build_receipt_sha256": sha(args.build_receipt), "target": TARGET, "checks": [],
              "artifacts": dict(build_record["artifacts"]), "native_outputs": {}, "recipe_sha256": sha(__file__)}
    receipt = root / "qualify.json"
    write_json(receipt, result)
    try:
        env = native_environment(build_record["toolchain"]["native_environment"])
        env.update(AXEON_CLANG=build_record["toolchain"]["tools"]["clang"]["path"],
                   AXEON_TENSOR_RUNTIME=str(binaries[BINARIES[2]]))
        need(sha(env["AXEON_CLANG"]) == build_record["toolchain"]["tools"]["clang"]["sha256"], "Clang bytes differ")
        logs = fresh(root / "logs")
        expected = expected_examples(source)
        def run(command, want):
            try:
                return execute(binaries["axeon-process.exe"], command, source, env, logs, result["checks"],
                               seconds=45, expected=want)
            finally:
                write_json(receipt, result)
        for name, (code, out, err) in expected.items():
            label = name.replace("/", "-")
            input_path = source / "examples" / (name + ".ax")
            want = (code, out.encode(), err.encode())
            run([binaries["axeon.exe"], "interpret", input_path], want)
            for operation, suffix in (("emit-object", ".obj"), ("build", ".exe")):
                artifact = root / (label + suffix)
                run([binaries["axeon.exe"], operation, input_path, "--opt", "2", "-o", artifact], (0, b"", b""))
                if suffix == ".exe":
                    pe(artifact)
                else:
                    with artifact.open("rb") as stream:
                        need(stream.read(2) == b"\x64\x86", "x64 COFF object required")
                sidecar = Path(str(artifact) + ".json")
                metadata = load(sidecar)
                need(metadata["target"] == TARGET and metadata["artifact_sha256"] == sha(artifact)
                     and metadata["source_sha256"] == sha(input_path), "native artifact metadata differs")
                for item in (artifact, sidecar):
                    result["artifacts"][item.name] = {"path": str(item), "sha256": sha(item)}
            run([root / (label + ".exe")], want)
            result["native_outputs"][name] = {"exit_code": code, "stdout": out, "stderr": err}
        validated_build(args.build_receipt)
        result.update(status="passed", optimization=2, example_count=len(expected))
    except BaseException as error:
        result.update(status="failed", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        write_json(receipt, result)
    print(json.dumps({"status": result["status"], "receipt": str(receipt)}))


def verify_qualification_execution(receipt_path, item, build_record, expected):
    root = safe_path(item["root"])
    need(root == regular(receipt_path).parent, "qualification receipt is outside its recorded run")
    source = Path(build_record["source"])
    compiler = Path(build_record["artifacts"]["bin/axeon.exe"]["path"])
    supervisor = build_record["artifacts"]["bin/axeon-process.exe"]["path"]
    for name in BINARIES:
        key = "bin/" + name
        need(item["artifacts"].get(key) == build_record["artifacts"][key],
             f"qualification binary does not match its build: {name}")
    commands = []
    for name, result in expected.items():
        label = name.replace("/", "-")
        input_path = source / "examples" / (name + ".ax")
        want = (result["exit_code"], result["stdout"].encode(), result["stderr"].encode())
        commands.append(([compiler, "interpret", input_path], want))
        for operation, suffix in (("emit-object", ".obj"), ("build", ".exe")):
            artifact = root / (label + suffix)
            commands.append(([compiler, operation, input_path, "--opt", "2", "-o", artifact], (0, b"", b"")))
            for path in (artifact, Path(str(artifact) + ".json")):
                need(item["artifacts"].get(path.name, {}).get("path") == str(path),
                     f"qualification artifact is outside its run: {path.name}")
        commands.append(([root / (label + ".exe")], want))
    checks = item.get("checks")
    need(isinstance(checks, list) and len(checks) == len(commands)
         and item.get("example_count") == len(expected), "qualification execution coverage is incomplete")
    for index, (check, (command, want)) in enumerate(zip(checks, commands)):
        need(check.get("passed") is True and check.get("command") == list(map(str, command))
             and check.get("cwd") == str(source) and check.get("supervisor") == supervisor
             and check.get("seconds_limit") == 45,
             f"qualification command differs at check {index}")
        streams = []
        for stream in ("stdout", "stderr"):
            path = regular(root / "logs" / f"{index:03d}.{stream}", 16 << 20)
            need(sha(path, 16 << 20) == check.get(stream + "_sha256"),
                 f"qualification output receipt changed: {path}")
            streams.append(path.read_bytes())
        need((check.get("exit_code"), *streams) == want,
             f"qualification result differs from the frozen oracle at check {index}")


def comparison(left_path, right_path):
    qualified, builds = [], []
    for value in (left_path, right_path):
        item = load(value)
        need(item.get("kind") == "axeon-windows-qualification" and item.get("status") == "passed"
             and item.get("target") == TARGET and item.get("optimization") == 2, "successful Windows qualification required")
        need(sha(item["build_receipt"]) == item["build_receipt_sha256"], "build receipt changed")
        build_record = validated_build(item["build_receipt"])
        need(item["source_commit"] == build_record["source_commit"], "qualification source differs")
        need(item.get("recipe_sha256") == build_record.get("recipe_sha256") == sha(__file__),
             "build, qualification and comparison must use the same reproduction recipe")
        for artifact in item["artifacts"].values():
            need(sha(artifact["path"]) == artifact["sha256"], "qualification artifact differs")
        expected = {name: {"exit_code": values[0], "stdout": values[1], "stderr": values[2]}
                    for name, values in expected_examples(Path(build_record["source"])).items()}
        need(item["native_outputs"] == expected, "independent native oracle outputs differ")
        verify_qualification_execution(value, item, build_record, expected)
        required = {f"bin/{name}" for name in BINARIES}
        required.update(name.replace("/", "-") + suffix for name in expected
                        for suffix in (".obj", ".obj.json", ".exe", ".exe.json"))
        need(set(item["artifacts"]) == required, "qualification artifact map is incomplete")
        qualified.append(item)
        builds.append(build_record)
    left, right = builds
    need(not safe_path(left["root"]).is_relative_to(safe_path(right["root"]))
         and not safe_path(right["root"]).is_relative_to(safe_path(left["root"])), "independent disjoint build roots required")
    for field in ("source_commit", "archive_sha256", "source_files", "toolchain", "compiler_version", "recipe_sha256"):
        need(left[field] == right[field], f"independent build inputs differ: {field}")
    first = {name: value["sha256"] for name, value in qualified[0]["artifacts"].items()}
    second = {name: value["sha256"] for name, value in qualified[1]["artifacts"].items()}
    need(first == second, "raw independent build artifact hashes differ; no normalization is permitted")
    return {"schema_version": 1, "kind": "axeon-windows-reproduction", "status": "passed", "target": TARGET,
            "source_commit": left["source_commit"], "archive_sha256": left["archive_sha256"],
            "compiler_version": left["compiler_version"], "independent_builds": 2,
            "shared_target_directory": False, "raw_artifacts_byte_identical": True,
            "qualification_receipts": [{"path": str(regular(value)), "sha256": sha(value)}
                                       for value in (left_path, right_path)],
            "artifact_sha256": first, "native_outputs": qualified[0]["native_outputs"],
            "toolchain": left["toolchain"], "recipe_sha256": sha(__file__)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    build_parser = modes.add_parser("build")
    for name in ("archive", "supervisor", "output-dir"):
        build_parser.add_argument("--" + name, type=Path, required=True)
    for name in ("archive-sha256", "source-commit"):
        build_parser.add_argument("--" + name, required=True)
    qualify_parser = modes.add_parser("qualify")
    for name in ("build-receipt", "output-dir"):
        qualify_parser.add_argument("--" + name, type=Path, required=True)
    compare_parser = modes.add_parser("compare")
    for name in ("left", "right", "output-dir"):
        compare_parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    native_host()
    if args.mode == "build":
        build(args)
    elif args.mode == "qualify":
        qualify(args)
    else:
        root = fresh(evidence_location(args.output_dir))
        try:
            result = comparison(args.left, args.right)
        except BaseException as error:
            write_json(root / "reproduction.json", {"status": "failed", "error": f"{type(error).__name__}: {error}"})
            raise
        write_json(root / "reproduction.json", result)
        print(json.dumps({"status": "passed", "receipt": str(root / "reproduction.json")}))


if __name__ == "__main__":
    main()

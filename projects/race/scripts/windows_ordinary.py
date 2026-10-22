"""Serial Windows ordinary suites; requires a separately reviewed pin receipt.

No pin capture, downloads, preset substitution, retries or acceptance promotion.
--configure reuses the existing cache; absent it, stale generated identity fails.
The caller supplies the reviewed pin JSON and its SHA-256. JSON fields: schema=1,
configuration, cache_values (complete parsed cache), files (absolute path/hash),
trees (required tool/SDK/dependency directory fingerprints from tree_records).
This is tooling, not a physics session supervisor or CI activation mechanism.
"""
from __future__ import annotations

import argparse
import ast
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import threading
import xml.etree.ElementTree as ET

import evidence_manifest
from output_paths import evidence_directory
from source_manifest import ROOT, authored_inputs, file_sha256, git_state
import windows_owned_job as owned

SELF = Path(__file__).resolve()
VS = Path("C:/Program Files/Microsoft Visual Studio/18/Community")
MSVC = VS / "VC/Tools/MSVC/14.51.36231"
SDK = Path("C:/Program Files (x86)/Windows Kits/10")
SDK_VERSION = "10.0.26100.0"
PYTHON = Path("C:/Users/YOUR_USER/AppData/Local/Programs/Python/Python313/python.exe")
GIT = Path("C:/Program Files/Git/cmd/git.exe")
CMAKE_HOME = VS / "Common7/IDE/CommonExtensions/Microsoft/CMake"
TOOLS = {"cmake": CMAKE_HOME / "CMake/bin/cmake.exe",
         "ctest": CMAKE_HOME / "CMake/bin/ctest.exe",
         "ninja": CMAKE_HOME / "Ninja/ninja.exe",
         "cl": MSVC / "bin/Hostx64/x64/cl.exe", "python": PYTHON, "git": GIT,
         "powershell": Path("C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")}
DEPENDENCIES = ROOT.parent / "Desktop-Dependencies-20261010"
QT = DEPENDENCIES / "Qt/6.10.1/msvc2022_64"
SDL = DEPENDENCIES / "SDL3-3.4.18"
LONG_PATH_TESTS = {"platform", "directory_watch", "mesh_watch_native",
                   "mesh_checkpoint_native", "mesh_reimport_native"}
SHA256 = re.compile(r"[0-9a-f]{64}")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def write_new(path: Path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)


def regular(path: Path) -> Path:
    require(path.is_absolute(), f"Absolute input required: {path}")
    for part in (path, *path.parents):
        info = part.lstat()
        require(not stat.S_ISLNK(info.st_mode) and
                not getattr(info, "st_file_attributes", 0) & 0x400,
                f"Redirected input refused: {part}")
    return path.resolve(strict=True)


def tree_records(path: Path) -> list:
    """Sorted complete file hashes; reject redirection, including directory links."""
    base = regular(path)
    records = []
    def inaccessible(error):
        raise error

    for directory, folders, files in os.walk(base, followlinks=False, onerror=inaccessible):
        for name in sorted([*folders, *files]):
            item = regular(Path(directory) / name)
            if item.is_file():
                records.append({"path": item.relative_to(base).as_posix(),
                                "sha256": file_sha256(item)})
    records.sort(key=lambda item: item["path"])
    require(records, f"Empty pinned input tree: {base}")
    return records


def records_sha256(records) -> str:
    data = json.dumps(records, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def required_trees(configuration: str) -> set:
    paths = {MSVC / "include", MSVC / "lib/x64", MSVC / "bin/Hostx64/x64",
             SDK / "Include" / SDK_VERSION, SDK / "Lib" / SDK_VERSION / "ucrt/x64",
             SDK / "Lib" / SDK_VERSION / "um/x64",
             SDK / "bin" / SDK_VERSION / "x64", CMAKE_HOME / "CMake",
             CMAKE_HOME / "Ninja", PYTHON.parent / "Lib", PYTHON.parent / "DLLs",
             GIT.parent.parent / "mingw64/bin"}
    if configuration == "Release":
        paths.update({QT, SDL})
    return {regular(path) for path in paths}


def parse_cache(text: str) -> dict:
    result = {}
    for line in text.splitlines():
        if line.startswith(("#", "//")) or "=" not in line or ":" not in line:
            continue
        declaration, value = line.split("=", 1)
        key = declaration.split(":", 1)[0]
        require(key not in result, f"Duplicate cache key: {key}")
        result[key] = value
    return result


def cache_contract(root: Path, build: Path, configuration: str) -> dict:
    cache = parse_cache((build / "CMakeCache.txt").read_text(encoding="utf-8"))
    desktop = configuration == "Release"
    expected = {"CMAKE_BUILD_TYPE": configuration, "CMAKE_GENERATOR": "Ninja",
                "RACE_DESKTOP": "ON" if desktop else "OFF",
                "RACE_NATIVE_REMOVAL_TESTS": "ON" if desktop else "OFF",
                "RACE_DESKTOP_ALLOCATION_PROFILE": "OFF", "RACE_SANITIZER": "none",
                "RACE_MSVC_ITERATOR_DEBUG_LEVEL": "0",
                "CMAKE_CXX_FLAGS": "/DWIN32 /D_WINDOWS /EHsc",
                "CMAKE_CXX_FLAGS_DEBUG": "/Ob0 /Od /RTC1",
                "CMAKE_CXX_FLAGS_RELEASE": "/O2 /Ob2 /DNDEBUG",
                "CMAKE_EXE_LINKER_FLAGS": "/machine:x64",
                "CMAKE_EXE_LINKER_FLAGS_DEBUG": "/debug /INCREMENTAL",
                "CMAKE_EXE_LINKER_FLAGS_RELEASE": "/INCREMENTAL:NO"}
    require(all(cache.get(k) == v for k, v in expected.items()), "Cached profile/flags differ")
    paths = {"CMAKE_HOME_DIRECTORY": root, "CMAKE_MAKE_PROGRAM": TOOLS["ninja"],
             "Python3_EXECUTABLE": PYTHON, "CMAKE_AR": TOOLS["cl"].with_name("lib.exe"),
             "CMAKE_LINKER": TOOLS["cl"].with_name("link.exe"),
             "CMAKE_RC_COMPILER": SDK / "bin" / SDK_VERSION / "x64/rc.exe",
             "CMAKE_MT": SDK / "bin" / SDK_VERSION / "x64/mt.exe",
             "CMAKE_COMMAND": TOOLS["cmake"], "CMAKE_CTEST_COMMAND": TOOLS["ctest"]}
    if desktop:
        paths.update({"Qt6_DIR": QT / "lib/cmake/Qt6", "SDL3_DIR": SDL / "cmake",
                      "RACE_DXC": SDK / "bin" / SDK_VERSION / "x64/dxc.exe"})
    for key, value in paths.items():
        require(Path(cache.get(key, "")).resolve() == value.resolve(), f"Cached path differs: {key}")
    require(cache.get("CMAKE_CXX_COMPILER") in {"cl", str(TOOLS["cl"]), TOOLS["cl"].as_posix()},
            "Cached compiler differs")
    for key in ("CMAKE_TOOLCHAIN_FILE", "CMAKE_PROJECT_INCLUDE", "CMAKE_PROJECT_INCLUDE_BEFORE",
                "CMAKE_PROJECT_TOP_LEVEL_INCLUDES", "CMAKE_CXX_COMPILER_LAUNCHER",
                "CMAKE_CXX_LINKER_LAUNCHER", "CMAKE_CROSSCOMPILING_EMULATOR"):
        require(not cache.get(key), f"Unapproved configure/command injection: {key}")
    compiler = build / "CMakeFiles/4.3.1-msvc1/CMakeCXXCompiler.cmake"
    text = compiler.read_text(encoding="utf-8")
    for setting, value in {"CMAKE_CXX_COMPILER": TOOLS["cl"].as_posix(),
                           "CMAKE_CXX_COMPILER_ID": "MSVC",
                           "CMAKE_CXX_COMPILER_VERSION": "19.51.36257.0",
                           "CMAKE_CXX_SIZEOF_DATA_PTR": "8"}.items():
        require(f'set({setting} "{value}")' in text, f"Compiler setting differs: {setting}")
    # CMake can write Ninja's 8.3 alias; the pinned-path equality above must pass first.
    cache["CMAKE_MAKE_PROGRAM"] = TOOLS["ninja"].resolve().as_posix()
    return cache


def checkpoint(root: Path, commit: str, digest: str) -> dict:
    # Fixed Git source queries capture both streams; only the pager selector is inert here.
    require(not any(k.startswith("GIT_") and k != "GIT_PAGER" and v for k, v in os.environ.items()),
            "Inherited Git routing/configuration override refused")
    resolved = shutil.which("git")
    require(resolved and Path(resolved).resolve() == GIT.resolve(), "Source query Git is not pinned Git")
    state, inputs = git_state(root), authored_inputs(root)
    require(state["working_tree_clean"] and state["commit"] == commit and
            inputs["sha256"] == digest, "Expected exact clean source/input identity")
    return {"git": state, "authored_inputs": inputs}


def pin_bindings(path: Path, expected_hash: str, configuration: str) -> tuple:
    require(SHA256.fullmatch(expected_hash), "Full pin receipt SHA-256 required")
    path = regular(path)
    require(file_sha256(path) == expected_hash, "Pin receipt bytes differ")
    pins = json.loads(path.read_text(encoding="utf-8"))
    require(pins["schema"] == 1 and pins["configuration"] == configuration,
            "Pin receipt schema/configuration differs")
    bindings = {}
    for p, digest in pins["files"].items():
        item = regular(Path(p))
        require(item not in bindings, f"Duplicate canonical file pin: {p}")
        bindings[item] = digest
    needed = {*TOOLS.values(), SELF, owned.SELF, PYTHON.parent / "python313.dll",
              PYTHON.parent / "python3.dll"}
    require({p.resolve() for p in needed}.issubset(bindings), "Missing required tool/helper pins")
    trees = {}
    for p, digest in pins["trees"].items():
        item = regular(Path(p))
        require(item not in trees, f"Duplicate canonical tree pin: {p}")
        trees[item] = digest
    require(set(trees) == required_trees(configuration), "Incomplete/extra tool/SDK input trees")
    for tree, digest in trees.items():
        records = tree_records(tree)
        require(SHA256.fullmatch(digest) and records_sha256(records) == digest,
                f"Pinned tree differs: {tree}")
        for item in records:
            item_path = tree / item["path"]
            require(item_path not in bindings or bindings[item_path] == item["sha256"],
                    f"Conflicting pin: {item_path}")
            bindings[item_path] = item["sha256"]
    bindings[path] = expected_hash
    for item, digest in bindings.items():
        require(SHA256.fullmatch(digest) and file_sha256(item) == digest, f"Pinned bytes differ: {item}")
    return bindings, pins["cache_values"]


def configuration_files(build: Path) -> dict:
    files = {build / n for n in ["CMakeCache.txt", "compile_commands.json", "build.ninja",
                                "generated/race/build_identity.hpp"]}
    files.update(build.rglob("CTestTestfile.cmake"))
    files.update((build / "CMakeFiles").glob("*.ninja"))
    files.update((build / "CMakeFiles/4.3.1-msvc1").glob("*.cmake"))
    files.update(p for p in build.glob("CTestCustom.*") if p.is_file())
    return {regular(p): file_sha256(p) for p in files}


def identity_headers(root: Path, args) -> dict:
    """Exact approved source formats, derived without executing the producer."""
    source = (root / "scripts/write_identity.py").read_text(encoding="utf-8")
    assignments = [node.value for node in ast.parse(source).body if isinstance(node, ast.Assign)
                   and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name)
                   and node.targets[0].id == "compiler"]
    require(len(assignments) == 1, "Identity producer compiler definition differs")
    compiler = ast.literal_eval(assignments[0])
    require(isinstance(compiler, str), "Identity compiler format is not literal text")
    values = {"source_commit": args.commit, "authored_input_sha256": args.authored_sha256,
              "configuration": args.configuration, "sanitizer": "none"}
    produced = "#pragma once\nnamespace race::build {\n" + compiler + "".join(
        f"inline constexpr const char* {key} = {json.dumps(value)};\n"
        for key, value in values.items()) + "}\n"
    configured = (root / "include/race/foundation/build_identity.hpp.in").read_text(encoding="utf-8")
    replacements = {"CMAKE_CXX_COMPILER_ID": "MSVC", "CMAKE_CXX_COMPILER_VERSION": "19.51.36257.0",
                    "RACE_SOURCE_COMMIT": args.commit, "RACE_SOURCE_INPUT_SHA": args.authored_sha256,
                    "CMAKE_BUILD_TYPE": args.configuration, "RACE_SANITIZER": "none"}
    for key, value in replacements.items():
        configured = configured.replace("@" + key + "@", value)
    require("@" not in configured, "Unresolved configured identity field")
    return {"configured": configured, "produced": produced}


def identity_header(path: Path, formats: dict) -> tuple:
    data = regular(path).read_bytes()
    for name, text in formats.items():
        if data in (text.encode("utf-8"), text.replace("\n", "\r\n").encode("utf-8")):
            return name, hashlib.sha256(data).hexdigest()
    raise ValueError("Generated identity fields/compiler format differ; guarded --configure may be needed")


def artifacts(build: Path) -> dict:
    files = {p for p in build.iterdir() if p.is_file() and p.suffix.lower() in {".exe", ".dll", ".lib"}}
    for folder in [build / "shaders", build / "platforms"]:
        if folder.is_dir():
            files.update(p for p in folder.rglob("*") if p.is_file())
    require(files and (build / "race_headless.exe") in files, "No built ordinary artifacts")
    return {regular(p): file_sha256(p) for p in files}


def inventory(document: dict) -> dict:
    require(document.get("kind") == "ctestInfo" and document.get("version") == {"major": 1, "minor": 0},
            "Unexpected CTest JSON schema")
    tests = document.get("tests")
    require(isinstance(tests, list) and tests, "Empty/invalid discovered inventory")
    result = {}
    for test in tests:
        name, command = test.get("name"), test.get("command")
        require(isinstance(name, str) and name and not any(c in name for c in '\0\r\n"'), "Invalid test name")
        require(name not in result, f"Duplicate test name: {name}")
        require(isinstance(command, list) and command and all(
            isinstance(s, str) and not any(c in s for c in '\0\r\n"') for s in command),
            f"Invalid/unsupported command: {name}")
        require(Path(command[0]).is_absolute(), f"Nonabsolute test executable: {name}")
        properties = {}
        for prop in test.get("properties", []):
            key = prop["name"]
            require(key not in properties, f"Duplicate test property: {name}/{key}")
            properties[key] = prop["value"]
        require(properties.get("DISABLED", False) in (False, "FALSE", "OFF", "0"),
                f"Disabled test: {name}")
        require(not any(k in properties for k in ("SKIP_RETURN_CODE", "SKIP_REGULAR_EXPRESSION", "WILL_FAIL")),
                f"Test result reinterpretation refused: {name}")
        directory = properties.get("WORKING_DIRECTORY")
        require(isinstance(directory, str) and Path(directory).is_absolute() and
                not any(c in directory for c in '\r\n'), f"Missing working directory: {name}")
        result[name] = {"command": command, "directory": directory, "properties": properties}
    require(LONG_PATH_TESTS.issubset(result), "Required long-path regressions missing")
    return result


def bind_commands(tests: dict, bindings: dict, root: Path, build: Path):
    """Bind actual outer wrapper and every absolute input, including inner tools."""
    files = {p.resolve() for p in bindings}
    for name, test in tests.items():
        command = test["command"]
        prefix = ["-B", str(root / "scripts/run_evidence.py"), "--run", name,
                  "--cmake-launcher", "", "--cmake-emulator", "", "--"]
        require(len(command) > 10 and Path(command[0]).resolve() == PYTHON.resolve() and
                [s.replace("\\", "/") for s in command[1:10]] ==
                [s.replace("\\", "/") for s in prefix], f"Unapproved test wrapper: {name}")
        require(Path(command[10]).is_absolute() and Path(command[10]).resolve() in files,
                f"Unbound inner executable: {name}")
        for argument in command:
            path = Path(argument)
            if not path.is_absolute():
                continue
            path = path.resolve()
            directory = path in {root.resolve(), build.resolve(), (build / "shaders").resolve()}
            require(path in files or directory, f"Unbound/other-profile command input: {name}: {argument}")
        require(Path(test["directory"]).resolve() == build.resolve(), f"Other-profile working directory: {name}")


def validate_completion(tests: dict, junit: str, log: str) -> dict:
    require("<!DOCTYPE" not in junit and "<!ENTITY" not in junit, "JUnit entities refused")
    root = ET.fromstring(junit)
    require(root.tag == "testsuite", "Expected single CTest JUnit testsuite")
    cases = root.findall("testcase")
    require(len(cases) == len(tests) and root.get("tests") == str(len(tests)), "JUnit inventory count differs")
    for key in ("failures", "errors", "disabled", "skipped"):
        require(root.get(key, "0") == "0", f"JUnit {key} is nonzero")
    seen = set()
    for case in cases:
        name = case.get("name")
        require(name in tests and name not in seen, "JUnit extra/duplicate/substituted test")
        require(case.get("status") == "run" and not any(case.find(k) is not None
                for k in ("failure", "error", "skipped")), f"Test did not pass: {name}")
        seen.add(name)
    require(seen == set(tests), "JUnit missing test")
    pattern = re.compile(r"^(\d+)/(\d+) Test: ([^\r\n]+)\r?\nCommand: ([^\r\n]*)\r?\nDirectory: ([^\r\n]*)", re.M)
    entries = list(pattern.finditer(log))
    require(len(entries) == len(tests), "Command-log inventory differs")
    logged, indices = set(), set()
    for entry in entries:
        index, total, name, command, directory = entry.groups()
        require(name in tests and name not in logged and int(index) not in indices and
                int(total) == len(tests), "Command-log name/index differs")
        expected = " ".join('"' + arg + '"' for arg in tests[name]["command"])
        require(command == expected and directory == tests[name]["directory"], f"Executed command differs: {name}")
        logged.add(name)
        indices.add(int(index))
    require(indices == set(range(1, len(tests) + 1)) and logged == seen, "Incomplete command-log coverage")
    require(log.startswith("Start testing:") and re.search(r"^End testing:", log, re.M), "Incomplete CTest log")
    return {"completed": len(seen), "names": sorted(seen), "command_binding": "exact LastTest header/argv/directory"}


def child_environment(output: Path) -> dict:
    environment = dict(os.environ)
    overrides = {"CL", "_CL_", "LINK", "_LINK_", "CMAKE_TOOLCHAIN_FILE", "PYTHONPATH", "PYTHONHOME",
                 "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML2_IMPORT_PATH",
                 "CMAKE_PREFIX_PATH", "CMAKE_INCLUDE_PATH", "CMAKE_LIBRARY_PATH", "CMAKE_PROGRAM_PATH"}
    overrides.update(k for k in environment if k.startswith("CTEST_") and k != "CTEST_PARALLEL_LEVEL")
    for key in overrides:
        require(not environment.get(key), f"Inherited override refused: {key}")
    directories = [TOOLS["cl"].parent, TOOLS["cmake"].parent, TOOLS["ninja"].parent,
                   PYTHON.parent, GIT.parent, SDK / "bin" / SDK_VERSION / "x64",
                   Path("C:/Windows/System32"), Path("C:/Windows")]
    environment.update(PATH=os.pathsep.join(map(str, directories)),
                       INCLUDE=";".join(map(str, [MSVC / "include", *[
                           SDK / "Include" / SDK_VERSION / p for p in ("ucrt", "shared", "um", "winrt", "cppwinrt")]])),
                       LIB=";".join(map(str, [MSVC / "lib/x64", *[
                           SDK / "Lib" / SDK_VERSION / p / "x64" for p in ("ucrt", "um")]])),
                       LIBPATH=str(MSVC / "lib/x64"), CMAKE_BUILD_PARALLEL_LEVEL="1",
                       CTEST_PARALLEL_LEVEL="1", RACE_EVIDENCE_RUN_DIR=str(output),
                       PYTHONDONTWRITEBYTECODE="1")
    return environment


class StageStopped(Exception):
    def __init__(self, result):
        self.result = result
        self.code = result["exit_code"] if result["exit_code"] else 125
        super().__init__(f'Stage {result["status"]}: {result["exit_code"]}')


def stage(label, command, *, root, output, bindings, before, after, record,
          environment, cancel=None, timeout=1200, capture=None):
    """All native stage paths use the reviewed helper; failure always stops caller."""
    before_state = before()
    write_new(output / (label + "-before.json"), before_state)
    result = owned.run_owned(command, cwd=root, output=output / label, timeout_seconds=timeout,
                             bindings=bindings, environment=environment, cancel=cancel)
    record["stages"].append({"label": label, "receipt": str(output / label / "receipt.json"),
                             "status": result["status"], "exit_code": result["exit_code"],
                             "quiescent": result["quiescent"]})
    # Capture logs even when post-source checks fail; preserve original failures.
    post_error = None
    try:
        after_state = after()
        write_new(output / (label + "-after.json"), after_state)
        if label != "configure":
            require(after_state == before_state, "Stage source/pin/configuration set drift")
    except Exception as error:
        post_error = error
    try:
        if capture is not None and result["quiescent"]:
            capture()
    except Exception as error:
        post_error = post_error or error
    if post_error is not None:
        record["stages"][-1]["postcheck_error"] = repr(post_error)
        if result["status"] == "exited" and result["exit_code"] == 0:
            raise post_error
    if (result["status"] != "exited" or result["exit_code"] != 0 or
            not result["quiescent"] or not result.get("bindings_unchanged", False)):
        raise StageStopped(result)
    require("postcheck_error" not in record["stages"][-1], "Post-stage identity failure")
    return (output / label / "stdout.log").read_text(encoding="utf-8-sig")


def qualify(args, root, build, output, bindings, common) -> dict:
    if args.configure:
        stage("configure", [str(TOOLS["cmake"]), "-S", str(root), "-B", str(build)],
              bindings=bindings, **common)
    header = regular(build / "generated/race/build_identity.hpp")
    formats = identity_headers(root, args)
    initial_format, initial_hash = identity_header(header, formats)
    config = configuration_files(build)
    require(config[header] == initial_hash and header not in bindings,
            "Generated identity snapshot differs or was pinned as an input")
    stable = dict(bindings)
    stable.update({p: h for p, h in config.items() if p != header})

    def build_inputs(probe):
        state = probe()
        files = dict(state["configuration_files"])
        files.pop(str(header))  # Only this known build output may change during build.
        return {**state, "configuration_files": files}

    build_common = {**common, "before": lambda: build_inputs(common["before"]),
                    "after": lambda: build_inputs(common["after"])}
    stage("build", [str(TOOLS["cmake"]), "--build", str(build), "--parallel", "1"],
          bindings=stable, **build_common)
    final_format, final_hash = identity_header(header, {"produced": formats["produced"]})
    config[header] = final_hash
    require(configuration_files(build) == config, "Post-build configuration set drifted")
    stable[header] = final_hash
    write_new(output / "identity-header-transition.json", {
        "path": str(header), "before_format": initial_format, "before_sha256": initial_hash,
        "after_format": final_format, "after_sha256": final_hash, "build_quiescent": True})
    built = artifacts(build)
    stable.update(built)
    write_new(output / "artifacts.json", {str(p): h for p, h in built.items()})
    identity = json.loads(stage("identity", [str(build / "race_headless.exe"), "--identity"],
                                bindings=stable, **common))
    require(identity == {"source_commit": args.commit, "authored_input_sha256": args.authored_sha256,
                         "configuration": args.configuration, "sanitizer": "none"}, "Built identity stale")
    command = [str(TOOLS["ctest"]), "--test-dir", str(build), "--parallel", "1"]
    discovered = inventory(json.loads(stage("discover", [*command, "--show-only=json-v1"],
                                             bindings=stable, **common)))
    bind_commands(discovered, stable, root, build)
    write_new(output / "inventory.json", discovered)
    log_path = build / "Testing/Temporary/LastTest.log"
    old_stamp = log_path.stat().st_mtime_ns if log_path.exists() else None

    def capture():
        require(log_path.stat().st_mtime_ns != old_stamp, "Stale CTest command log")
        with (output / "LastTest.log").open("xb") as stream:
            stream.write(log_path.read_bytes())

    stage("test", [*command, "--output-on-failure", "--output-log", str(output / "ctest.log"),
                   "--output-junit", str(output / "ctest.xml")], bindings=stable, capture=capture, **common)
    completed = validate_completion(discovered, (output / "ctest.xml").read_text(encoding="utf-8-sig"),
                                    (output / "LastTest.log").read_text(encoding="utf-8-sig"))
    rediscovered = inventory(json.loads(stage("rediscover", [*command, "--show-only=json-v1"],
                                               bindings=stable, **common)))
    require(rediscovered == discovered, "Inventory/commands drifted during tests")
    require(artifacts(build) == built and configuration_files(build) == config,
            "Artifact/configuration set drifted")
    return completed


def finalize_receipt(output: Path, record: dict):
    record["finished_utc"] = datetime.now(timezone.utc).isoformat()
    record["output"] = str(output)
    record["stage_outcome"] = {key: record[key] for key in ("passed", "status", "exit_code", "error")
                               if key in record}
    record["manifest"] = {"complete": False}
    record["files"] = {}
    try:
        record["driver_sha256"] = file_sha256(SELF)
        record["supervisor_sha256"] = file_sha256(owned.SELF)
        record["manifest_helper_sha256"] = file_sha256(evidence_manifest.SELF)
        record["files"] = evidence_manifest.complete_files(output)
        record["manifest"] = {"complete": True, "file_count": len(record["files"]),
                              "excluded": ["receipt.json"]}
    except (Exception, KeyboardInterrupt) as error:
        record.update(passed=False, exit_code=125, status="evidence-incomplete")
        record["files"] = {}
        record["manifest"] = {"complete": False, "error": f"{type(error).__name__}: {error}"}
    try:
        evidence_manifest.write_receipt(output, record)
    except (Exception, KeyboardInterrupt) as error:
        record.update(passed=False, exit_code=125, status="receipt-write-failed")
        raise OSError(f"{error}; original stage outcome={record['stage_outcome']}; "
                      f"manifest={record['manifest']}") from error


def run(args, cancel: threading.Event | None = None) -> dict:
    output = evidence_directory("windows-ordinary-" + args.configuration.lower(), args.output)
    root = ROOT
    build = root / "out" / ("content-recipes-" + args.configuration.lower())
    record = {"schema": 1, "kind": "race-windows-ordinary-suite", "passed": False,
              "exit_code": 125, "status": "error", "stages": [], "configuration": args.configuration,
              "expected_commit": args.commit, "expected_authored_sha256": args.authored_sha256,
              "started_utc": datetime.now(timezone.utc).isoformat(),
              "scope": "Full configured ordinary inventory only; separate Qt/SDL journeys, GPU smoke, extracted packages, isolated Jolt admission and phase acceptance remain open."}
    try:
        require(os.name == "nt" and Path(sys.executable).resolve() == PYTHON.resolve() and
                sys.version_info[:3] == (3, 13, 15), "Pinned Windows Python required")
        require(re.fullmatch(r"[0-9a-f]{40}", args.commit) and SHA256.fullmatch(args.authored_sha256),
                "Full expected commit/input identities required")
        pinned, cache_values = pin_bindings(args.pins, args.pins_sha256, args.configuration)
        bindings = dict(pinned)
        environment = child_environment(output)

        def source_pins():
            source = checkpoint(root, args.commit, args.authored_sha256)
            fresh, expected = pin_bindings(args.pins, args.pins_sha256, args.configuration)
            require(fresh == pinned and expected == cache_values, "Tool/SDK input set changed")
            require(cache_contract(root, build, args.configuration) == cache_values, "Cache flags drift")
            return {"source": source, "pins_sha256": args.pins_sha256,
                    "configuration_files": {str(p): h for p, h in configuration_files(build).items()}}

        original = source_pins()
        write_new(output / "admission.json", original)
        bindings.update({root / item["path"]: item["sha256"] for item in original["source"]["authored_inputs"]["files"]})
        common = dict(root=root, output=output, before=source_pins, after=source_pins,
                      record=record, environment=environment, cancel=cancel)
        record["completion"] = qualify(args, root, build, output, bindings, common)
        record.update(passed=True, exit_code=0, status="passed")
    except StageStopped as error:
        record.update(exit_code=error.code, status=error.result["status"], error=str(error))
    except (Exception, KeyboardInterrupt) as error:
        record.update(error=f"{type(error).__name__}: {error}")
    finally:
        finalize_receipt(output, record)
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--configuration", choices=["Debug", "Release"], required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--authored-sha256", required=True)
    parser.add_argument("--pins", type=Path, required=True)
    parser.add_argument("--pins-sha256", required=True)
    parser.add_argument("--configure", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        record = run(args)
        print(json.dumps({"passed": record["passed"], "status": record["status"],
                          "exit_code": record["exit_code"], "output": record["output"]}))
        return record["exit_code"]
    except (OSError, ValueError) as error:
        print(f"Ordinary execution or receipt finalization failed: {error}", file=sys.stderr)
        return 125


if __name__ == "__main__":
    raise SystemExit(main())

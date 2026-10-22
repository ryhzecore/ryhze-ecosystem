"""Focused inert ordinary-driver contracts. Never runs CMake, CTest or engine code."""
import argparse
from contextlib import ExitStack
import copy
import json
import os
from pathlib import Path
import re
import sys
import threading
from types import SimpleNamespace
from unittest.mock import patch
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import windows_ordinary as driver
import source_manifest
from output_paths import evidence_directory
from source_manifest import file_sha256
from verify_desktop_surfaces import native_guard

CHILD = "import sys,time\nif sys.argv[1]=='sleep':time.sleep(5)\nelse:sys.exit(int(sys.argv[1]))\n"


def discovered(root=None, build=None):
    tests = []
    for name in [*sorted(driver.LONG_PATH_TESTS), "ordinary-other"]:
        command = [sys.executable, "-B", "inert fixture 雪.py", "", name]
        if root is not None:
            command = [sys.executable, "-B", str(root / "scripts/run_evidence.py"), "--run", name,
                       "--cmake-launcher", "", "--cmake-emulator", "", "--", str(build / "race_headless.exe")]
        tests.append({"name": name, "command": command,
                      "properties": [{"name": "WORKING_DIRECTORY", "value": str(build or Path.cwd())}]})
    return {"kind": "ctestInfo", "version": {"major": 1, "minor": 0}, "tests": tests}


def completion(document):
    root = ET.Element("testsuite", tests=str(len(document["tests"])), failures="0", errors="0",
                      skipped="0", disabled="0")
    log = ["Start testing: inert fixture", "----------------------------------------------------------"]
    for index, test in enumerate(document["tests"], 1):
        ET.SubElement(root, "testcase", name=test["name"], status="run", time="0")
        command = " ".join('"' + arg + '"' for arg in test["command"])
        log.extend([f'{index}/{len(document["tests"])} Test: {test["name"]}', "Command: " + command,
                    "Directory: " + test["properties"][0]["value"], "Output:", "Test Passed."])
    log.append("End testing: inert fixture")
    return ET.tostring(root, encoding="unicode"), "\n".join(log) + "\n"


def parser_cases(check, rejects):
    good = discovered()
    tests = driver.inventory(good)
    junit, log = completion(good)
    check("complete inventory and argv/directory correspondence", driver.validate_completion(tests, junit, log)["completed"] == len(tests))
    mutations = {
        "empty": lambda d: d.update(tests=[]),
        "duplicate": lambda d: d["tests"].append(copy.deepcopy(d["tests"][0])),
        "missing command": lambda d: d["tests"][0].pop("command"),
        "empty command": lambda d: d["tests"][0].update(command=[]),
        "nonabsolute executable": lambda d: d["tests"][0]["command"].__setitem__(0, "python.exe"),
        "unsupported quote": lambda d: d["tests"][0]["command"].append('embedded"quote'),
        "disabled bool": lambda d: d["tests"][0]["properties"].append({"name": "DISABLED", "value": True}),
        "disabled string": lambda d: d["tests"][0]["properties"].append({"name": "DISABLED", "value": "ON"}),
        "skip return": lambda d: d["tests"][0]["properties"].append({"name": "SKIP_RETURN_CODE", "value": 75}),
        "expected failure": lambda d: d["tests"][0]["properties"].append({"name": "WILL_FAIL", "value": True}),
        "missing directory": lambda d: d["tests"][0].update(properties=[]),
        "duplicate property": lambda d: d["tests"][0]["properties"].append(copy.deepcopy(d["tests"][0]["properties"][0])),
        "missing long path": lambda d: d["tests"].pop(0),
        "schema": lambda d: d.update(kind="wrong"),
    }
    for label, mutate in mutations.items():
        value = copy.deepcopy(good)
        mutate(value)
        rejects("inventory " + label, lambda: driver.inventory(value))
    for tag in ("failure", "error", "skipped"):
        xml = ET.fromstring(junit)
        ET.SubElement(xml[0], tag)
        rejects("empty JUnit " + tag, lambda: driver.validate_completion(tests, ET.tostring(xml, encoding="unicode"), log))
    for status in ("notrun", "disabled", "fail", "skipped", ""):
        xml = ET.fromstring(junit)
        xml[0].set("status", status)
        rejects("JUnit status " + status, lambda: driver.validate_completion(tests, ET.tostring(xml, encoding="unicode"), log))
    for label, edit in {
        "missing": lambda x: x.remove(x[-1]),
        "extra": lambda x: ET.SubElement(x, "testcase", name="extra", status="run"),
        "duplicate": lambda x: x[-1].set("name", x[0].get("name")),
        "substituted": lambda x: x[-1].set("name", "substitution"),
        "count": lambda x: x.set("tests", "99"),
        "root failure": lambda x: x.set("failures", "1"),
    }.items():
        xml = ET.fromstring(junit)
        edit(xml)
        rejects("JUnit " + label, lambda: driver.validate_completion(tests, ET.tostring(xml, encoding="unicode"), log))
    for label, modified in {
        "empty argument removed": log.replace(' "" ', ' '),
        "command replaced": log.replace('"-B"', '"-I"'),
        "directory changed": log.replace("Directory: ", "Directory: changed-"),
        "total changed": log.replace("/6 Test:", "/7 Test:"),
        "duplicate header": log + log,
        "no footer": log.replace("End testing:", "No end:"),
        "unbound command spoof in output": log.replace('Command: ', 'Output command: '),
    }.items():
        rejects("log " + label, lambda: driver.validate_completion(tests, junit, modified))


def input_cases(out, check, rejects):
    tree = out / "input-tree"
    tree.mkdir()
    (tree / "a").write_bytes(b"one")
    first = driver.records_sha256(driver.tree_records(tree))
    check("tree deterministic", first == driver.records_sha256(driver.tree_records(tree)))
    (tree / "a").write_bytes(b"two")
    check("tree detects byte drift", first != driver.records_sha256(driver.tree_records(tree)))
    (tree / "b").write_bytes(b"extra")
    check("tree binds entire file set", len(driver.tree_records(tree)) == 2)
    rejects("duplicate cache keys", lambda: driver.parse_cache("K:STRING=one\nK:STRING=two"))
    with patch.dict(os.environ, {"CL": "/override"}):
        rejects("inherited flags rejected", lambda: driver.child_environment(out))
    with patch.dict(os.environ, {"CTEST_EXTRA_ARGS": "--exclude-regex .*"}):
        rejects("inherited test filter rejected", lambda: driver.child_environment(out))
    with patch.dict(os.environ, {"GIT_DIR": str(out)}):
        rejects("Git repository redirection rejected", lambda: driver.checkpoint(out, "a" * 40, "b" * 64))
    with patch.object(driver.shutil, "which", return_value=str(out / "other-git.exe")):
        rejects("ambient Git substitution rejected", lambda: driver.checkpoint(out, "a" * 40, "b" * 64))
    def inaccessible(*_, onerror, **__):
        onerror(PermissionError("inert unreadable directory"))
    with patch.object(driver.os, "walk", side_effect=inaccessible):
        rejects("incomplete SDK enumeration fails", lambda: driver.tree_records(tree))
    environment = driver.child_environment(out)
    check("serial environment", environment["CMAKE_BUILD_PARALLEL_LEVEL"] == environment["CTEST_PARALLEL_LEVEL"] == "1")
    commit, digest = "a" * 40, "b" * 64
    with patch.object(driver, "git_state", return_value={"working_tree_clean": False, "commit": commit}), \
            patch.object(driver, "authored_inputs", return_value={"sha256": digest}):
        rejects("dirty source rejected", lambda: driver.checkpoint(out, commit, digest))
    with patch.object(driver, "git_state", return_value={"working_tree_clean": True, "commit": commit}), \
            patch.object(driver, "authored_inputs", return_value={"sha256": "c" * 64}):
        rejects("source digest drift rejected", lambda: driver.checkpoint(out, commit, digest))
    for configuration in ("Debug", "Release"):
        build = out / ("cache-" + configuration.lower())
        build.mkdir()
        actual = driver.ROOT / "out" / ("content-recipes-" + configuration.lower())
        (build / "CMakeCache.txt").write_bytes((actual / "CMakeCache.txt").read_bytes())
        compiler = build / "CMakeFiles/4.3.1-msvc1/CMakeCXXCompiler.cmake"
        compiler.parent.mkdir(parents=True)
        compiler.write_bytes((actual / "CMakeFiles/4.3.1-msvc1/CMakeCXXCompiler.cmake").read_bytes())
        value = driver.cache_contract(driver.ROOT, build, configuration)
        check(configuration + " actual retained profile accepted", value["RACE_DESKTOP"] == ("ON" if configuration == "Release" else "OFF"))
        text = (build / "CMakeCache.txt").read_text()
        for tool in ("CMAKE_LINKER", "CMAKE_AR", "CMAKE_RC_COMPILER", "CMAKE_MT", "CMAKE_COMMAND", "CMAKE_CTEST_COMMAND"):
            import re
            changed = re.sub(r"(?m)^" + tool + r":[^=]+=.*$", tool + ":FILEPATH=C:/unbound.exe", text)
            check(configuration + " tool mutation is present " + tool, changed != text)
            (build / "CMakeCache.txt").write_text(changed)
            rejects(configuration + " tool substitution " + tool, lambda: driver.cache_contract(driver.ROOT, build, configuration))
        (build / "CMakeCache.txt").write_text(text.replace("RACE_MSVC_ITERATOR_DEBUG_LEVEL:STRING=0", "RACE_MSVC_ITERATOR_DEBUG_LEVEL:STRING=2"))
        rejects(configuration + " flag substitution rejected", lambda: driver.cache_contract(driver.ROOT, build, configuration))


def stage_cases(out, check, rejects, receipt):
    child = out / "inert fixture.py"
    child.write_text(CHILD, encoding="utf-8")
    bindings = {p: file_sha256(p) for p in [driver.owned.SELF, driver.SELF, Path(__file__), Path(sys.executable), child]}

    def exercise(label, arg="0", seconds=5, cancel=None, pins=None, post=None):
        record = {"stages": []}
        try:
            driver.stage(label, [sys.executable, "-B", str(child), arg], root=out, output=out,
                         bindings=pins or bindings, before=lambda: {"inert": True},
                         after=post or (lambda: {"inert": True}), record=record,
                         environment=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"), cancel=cancel, timeout=seconds)
            result = None
        except driver.StageStopped as error:
            result = error
        receipt["native_stage_cases"].append({"label": label, "stages": record["stages"]})
        if result and result.result["status"] == "held" and not result.result["guard"].get("fixture"):
            raise RuntimeError("real admission held")
        return result

    check("real inert success through driver/helper", exercise("inert-success") is None)
    failed = exercise("inert-exit7", "7")
    check("original child exit7 retained", failed.code == 7 and failed.result["quiescent"])
    timed = exercise("inert-timeout", "sleep", seconds=.2)
    check("timeout124 and owned quiescence", timed.code == 124 and timed.result["status"] == "timeout" and timed.result["quiescent"])
    cancelled = threading.Event()
    cancelled.set()
    value = exercise("inert-cancel", cancel=cancelled)
    check("cancel130 retained", value.code == 130 and value.result["quiescent"])
    with patch.object(driver.owned, "native_guard", return_value={"passed": False, "fixture": True}):
        value = exercise("inert-held")
    check("held75 never success/no child", value.code == 75 and not value.result["child_created"])
    bad = dict(bindings)
    bad[Path(sys.executable)] = "0" * 64
    value = exercise("inert-tool-drift", pins=bad)
    check("tool drift cannot launch", value.code == 125 and not value.result["child_created"])
    value = exercise("inert-failure-and-postdrift", "19", post=lambda: {"inert": False})
    check("postcheck never masks original exit19", value.code == 19)
    rejects("post-success drift fails", lambda: exercise("inert-success-postdrift", post=lambda: {"inert": False}))
    for label, change in [("nonquiescent", {"quiescent": False}), ("binding-drift", {"bindings_unchanged": False})]:
        result = dict(status="exited", exit_code=0, quiescent=True, bindings_unchanged=True)
        result.update(change)
        with patch.object(driver.owned, "run_owned", return_value=result):
            value = exercise("synthetic-" + label)
        check("zero exit cannot hide " + label, value.code == 125)


def git_environment_cases(out, check, rejects):
    commit, digest = "a" * 40, "b" * 64
    calls = []
    replies = {("rev-parse", "--verify", "HEAD"): commit,
               ("branch", "--show-current"): "inert-branch",
               ("status", "--porcelain=v1", "--untracked-files=all"): ""}
    prefix = ["git", "-c", f"safe.directory={out.resolve()}"]
    def captured(command, **kwargs):
        query = tuple(command[3:])
        if command[:3] != prefix or query not in replies:
            raise ValueError("unexpected scoped Git fixture argv")
        calls.append(command)
        check("Git source query captures both streams " + query[0],
              command[0] == "git" and kwargs.get("capture_output") is True
              and not kwargs.get("shell", False) and "env" not in kwargs)
        return SimpleNamespace(returncode=0, stdout=replies[query] + "\n")
    # Replace the Python mapping for inert observation; do not clear/change the OS environment.
    synthetic = {"GIT_PAGER": "inert fixture selector, never a command"}
    with patch.object(driver.os, "environ", synthetic), \
            patch.object(driver.shutil, "which", return_value=str(driver.GIT)), \
            patch.object(driver, "authored_inputs", return_value={"sha256": digest}), \
            patch.object(source_manifest.subprocess, "run", side_effect=captured) as runner:
        state = driver.checkpoint(out, commit, digest)
        check("GIT_PAGER tolerated through captured source-query path",
              state["git"]["commit"] == commit and len(calls) == 3)
        check("pager selector never becomes subprocess argv", all(c[0] == "git" for c in calls))
        for name in ["GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_OBJECT_DIRECTORY",
                     "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_INDEX_FILE", "GIT_CONFIG_COUNT",
                     "GIT_CONFIG_KEY_0", "GIT_CONFIG_VALUE_0", "GIT_CONFIG_GLOBAL",
                     "GIT_CONFIG_SYSTEM", "GIT_CONFIG_PARAMETERS", "GIT_UNKNOWN_OVERRIDE"]:
            with patch.object(driver.os, "environ", {**synthetic, name: "inert override"}):
                before = runner.call_count
                rejects(name + " still refused alongside GIT_PAGER",
                        lambda: driver.checkpoint(out, commit, digest))
                check(name + " refused before any Git launch", runner.call_count == before)
    query = ["rev-parse", "--verify", "HEAD"]
    invalid = {
        "missing scoped option": ["git", *query],
        "missing scoped value": ["git", "-c", *query],
        "wrong executable": ["other-git", *prefix[1:], *query],
        "wrong option flag": ["git", "--config", prefix[2], *query],
        "wrong scoped root": ["git", "-c", f"safe.directory={out.parent.resolve()}", *query],
        "wildcard scoped root": ["git", "-c", "safe.directory=*", *query],
        "extra scoped option": [*prefix, "-c", prefix[2], *query],
        "trailing scoped option": [*prefix, *query, "-c", prefix[2]],
        "unknown query": [*prefix, "status", "--short"],
        "empty argv": [],
    }
    before = len(calls)
    for label, command in invalid.items():
        rejects("Git mock rejects " + label, lambda: captured(command, capture_output=True))
    check("rejected mock argv never reaches a fixed reply", len(calls) == before)


def ninja_cache_cases(out, check, rejects):
    build = out / "ninja-cache-fixture"
    build.mkdir()
    actual = driver.ROOT / "out/content-recipes-debug"
    text = (actual / "CMakeCache.txt").read_text(encoding="utf-8")
    compiler = build / "CMakeFiles/4.3.1-msvc1/CMakeCXXCompiler.cmake"
    compiler.parent.mkdir(parents=True)
    compiler.write_bytes((actual / "CMakeFiles/4.3.1-msvc1/CMakeCXXCompiler.cmake").read_bytes())
    pinned = driver.TOOLS["ninja"].resolve()
    short = Path("C:/INERT~1/NINJA~1/ninja.exe")
    resolve = Path.resolve
    def equivalent(path, *args, **kwargs):
        return pinned if path == short else resolve(path, *args, **kwargs)
    def contract(value, extra="retained literal"):
        changed, count = re.subn(r"(?m)^CMAKE_MAKE_PROGRAM:[^=]+=.*$",
                                 "CMAKE_MAKE_PROGRAM:FILEPATH=" + value, text)
        check("Ninja fixture changes exact cache field", count == 1)
        changed += "\nINERT_RETAINED_VALUE:STRING=" + extra + "\n"
        (build / "CMakeCache.txt").write_text(changed, encoding="utf-8")
        result = driver.cache_contract(driver.ROOT, build, "Debug")
        raw = driver.parse_cache(changed)
        check("every non-Ninja cache value stays exact", {k: v for k, v in result.items()
              if k != "CMAKE_MAKE_PROGRAM"} == {k: v for k, v in raw.items() if k != "CMAKE_MAKE_PROGRAM"})
        return result
    # Model only one equivalent alias; no synthetic executable is accessed or launched.
    with patch.object(Path, "resolve", new=equivalent):
        long_result = contract(driver.TOOLS["ninja"].as_posix())
        short_result = contract(short.as_posix())
        check("same pinned Ninja long/short paths produce identical cache contracts",
              long_result == short_result and short_result["CMAKE_MAKE_PROGRAM"] == pinned.as_posix())
        changed = contract(short.as_posix(), extra="changed literal")
        check("unrelated cache drift still changes full contract", changed != short_result)
        rejects("different unpinned Ninja refused before canonicalization",
                lambda: contract((out / "other-ninja.exe").as_posix()))


def pin_cases(out, check, rejects):
    tree = out / "pinned-sdk-fixture"
    tree.mkdir()
    (tree / "inert-header.h").write_bytes(b"#define INERT 1\n")
    powershell = out / "inert-powershell.exe"
    powershell.write_bytes(b"INERT DATA: POWERSHELL PIN STAND-IN, NEVER EXECUTED")
    check("PowerShell required path matches both generated Windows profiles",
          driver.TOOLS["powershell"] == Path("C:\\windows/System32/WindowsPowerShell/v1.0/powershell.exe"))
    def invoke(label, value, expected=None, configuration="Debug"):
        path = out / (label + "-pins.json")
        driver.write_new(path, value)
        return driver.pin_bindings(path, expected or file_sha256(path), configuration)
    # No PowerShell file query, pin capture or launch: its required-file role uses inert bytes.
    with patch.dict(driver.TOOLS, powershell=powershell), patch.object(
            driver, "required_trees", return_value={tree.resolve()}):
        files = {*driver.TOOLS.values(), driver.SELF, driver.owned.SELF,
                 driver.PYTHON.parent / "python3.dll", driver.PYTHON.parent / "python313.dll"}
        good = {"schema": 1, "configuration": "Debug", "cache_values": {"fixture": True},
                "files": {str(p): file_sha256(p) for p in files},
                "trees": {str(tree): driver.records_sha256(driver.tree_records(tree))}}
        pinned, cache = invoke("valid-fixture", good)
        check("explicit reviewed file/tree pins accepted", tree / "inert-header.h" in pinned and cache == {"fixture": True})
        release = {**good, "configuration": "Release"}
        release_pins, _ = invoke("valid-release-fixture", release, configuration="Release")
        check("PowerShell required file bound for Debug and Release", powershell in pinned and powershell in release_pins)
        for label, mutate in {
            "missing-tool": lambda p: p["files"].pop(str(driver.TOOLS["ctest"])),
            "missing-powershell": lambda p: p["files"].pop(str(powershell)),
            "substituted-powershell": lambda p: p["files"].update({
                str(tree / "inert-header.h"): p["files"].pop(str(powershell))}),
            "tool-drift": lambda p: p["files"].update({str(driver.PYTHON): "0" * 64}),
            "missing-sdk-tree": lambda p: p.update(trees={}),
            "sdk-bytes-drift": lambda p: p["trees"].update({str(tree): "0" * 64}),
            "wrong-configuration": lambda p: p.update(configuration="Release"),
        }.items():
            value = copy.deepcopy(good)
            mutate(value)
            rejects("pins reject " + label, lambda: invoke(label, value))
        rejects("pin receipt itself is bound", lambda: invoke("receipt-drift", good, expected="0" * 64))


def pipeline_child(args, build, formats, calls, failure, mutate_inventory, mutation):
    header = build / "generated/race/build_identity.hpp"
    root = build.parent.parent
    def fake(command, *, output, bindings, **_):
        output.mkdir()
        stage = output.name
        calls.append({"stage": stage, "command": command, "header_bound": header in bindings})
        result = dict(status="exited", exit_code=0, quiescent=True, bindings_unchanged=True)
        if failure and stage == failure[0]:
            result.update(status=failure[1], exit_code=failure[2])
        data = ""
        if stage == "configure":
            header.write_text(formats["configured"])
        if stage == "build":
            text = formats["produced"]
            if mutation == "wrong-identity":
                text = text.replace(args.commit, "d" * 40)
            if mutation == "wrong-compiler":
                text = text.replace("_MSC_FULL_VER", "_MSC_VER")
            header.write_text(text)
            if mutation == "other-config":
                (build / "build.ninja").write_text("changed other config")
        if stage == "identity":
            data = json.dumps(dict(source_commit=args.commit, authored_input_sha256=args.authored_sha256,
                                   configuration="Debug", sanitizer="none"))
            if mutation == "later-header":
                header.write_text(formats["configured"])
        if stage in {"discover", "rediscover"}:
            doc = discovered(root, build)
            if stage == "rediscover" and mutate_inventory:
                doc["tests"][-1]["command"].append("changed")
            data = json.dumps(doc)
        if stage == "test":
            xml, log = completion(discovered(root, build))
            (args.output / "ctest.xml").write_text(xml)
            log_path = build / "Testing/Temporary/LastTest.log"
            log_path.parent.mkdir(parents=True)
            log_path.write_text(log)
        (output / "stdout.log").write_text(data, encoding="utf-8")
        result["bindings_unchanged"] = all(file_sha256(p) == h for p, h in bindings.items())
        return result
    return fake


def pipeline_case(out, label, check, *, failure=None, configure=False, stale=False,
                  mutate_inventory=False, mutation=None, initial="configured"):
    """Synthetic adapters and inert files; no native pipeline is launched."""
    folder = out / label
    folder.mkdir()
    driver.write_new(folder / "fixture-context.json", {"kind": "synthetic driver adapters", "real_engine_pipeline": False})
    root = folder / "source"
    build = root / "out/content-recipes-debug"
    header = build / "generated/race/build_identity.hpp"
    header.parent.mkdir(parents=True)
    args = SimpleNamespace(configuration="Debug", output=folder / "result", commit="a" * 40,
                           authored_sha256="b" * 64, pins=folder / "unused-pins.json",
                           pins_sha256="c" * 64, configure=configure)
    for relative in ["scripts/write_identity.py", "include/race/foundation/build_identity.hpp.in"]:
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes((driver.SELF.parent.parent / relative).read_bytes())
    formats = driver.identity_headers(root, args)
    header.write_text("stale" if stale else formats[initial])
    for name in ["CMakeCache.txt", "compile_commands.json", "build.ninja"]:
        (build / name).write_bytes(b"inert config data")
    target = build / "race_headless.exe"
    target.write_bytes(b"INERT DATA: NOT AN EXECUTABLE")
    pinned = folder / "pinned-input"
    pinned.write_bytes(b"inert")
    wrapper = root / "scripts/run_evidence.py"
    wrapper.write_bytes(b"# inert data, never executed\n")
    pins = {p: file_sha256(p) for p in [pinned, wrapper, Path(sys.executable)]}
    calls = []
    fake = pipeline_child(args, build, formats, calls, failure, mutate_inventory, mutation)

    source = {"git": {"commit": args.commit, "working_tree_clean": True}, "authored_inputs": {"files": []}}
    with ExitStack() as stack:
        for name, value in [("ROOT", folder / "source")]:
            stack.enter_context(patch.object(driver, name, value))
        stack.enter_context(patch.object(driver, "pin_bindings", return_value=(pins, {"fixture": True})))
        stack.enter_context(patch.object(driver, "cache_contract", return_value={"fixture": True}))
        stack.enter_context(patch.object(driver, "checkpoint", return_value=source))
        stack.enter_context(patch.object(driver, "artifacts", return_value={target: file_sha256(target)}))
        stack.enter_context(patch.object(driver.owned, "run_owned", side_effect=fake))
        result = driver.run(args)
    expected = ["build", "identity", "discover", "test", "rediscover"]
    if configure:
        expected.insert(0, "configure")
    if failure:
        expected = expected[:expected.index(failure[0]) + 1]
    elif stale and not configure:
        expected = []
    elif mutation:
        expected = expected[:expected.index("identity" if mutation == "later-header" else "build") + 1]
    check(label + " exact stage ordering/no later launch", [c["stage"] for c in calls] == expected)
    successful = not failure and not (stale and not configure) and not mutate_inventory and not mutation
    check(label + " truthful overall status", result["passed"] == successful)
    transition = args.output / "identity-header-transition.json"
    build_accepted = bool(calls) and not failure and mutation not in {"wrong-identity", "wrong-compiler", "other-config"}
    if failure and failure[0] != "build":
        build_accepted = True
    check(label + " rebind only after validated build", transition.is_file() == build_accepted)
    check(label + " header unbound only during build", all(c["header_bound"] == (c["stage"] not in {"configure", "build"}) for c in calls))
    if failure:
        check(label + " original exit retained", result["exit_code"] == failure[2])
        if failure[0] == "test":
            check(label + " failed command log preserved", (args.output / "LastTest.log").is_file())
    check(label + " serial build/CTest commands", all("--parallel" not in c["command"] or c["command"][c["command"].index("--parallel") + 1] == "1" for c in calls))
    return args


def command_cases(out, check, rejects):
    root, build = out / "source-command-fixture", out / "source-command-fixture/out/debug"
    build.mkdir(parents=True)
    wrapper = root / "scripts/run_evidence.py"
    wrapper.parent.mkdir()
    wrapper.write_bytes(b"# inert")
    binary = build / "race_headless.exe"
    binary.write_bytes(b"inert data")
    tests = driver.inventory(discovered(root, build))
    pins = {p: file_sha256(p) for p in [wrapper, binary, Path(sys.executable)]}
    driver.bind_commands(tests, pins, root, build)
    check("discovered commands bind matching source/profile", True)
    for label, index, value in [("other profile", 10, str(root / "out/release/race_headless.exe")),
                                ("other source", 2, str(out / "other-source.py")),
                                ("other interpreter", 0, str(out / "other-python.exe"))]:
        changed = copy.deepcopy(tests)
        next(iter(changed.values()))["command"][index] = value
        rejects("command rejects " + label, lambda: driver.bind_commands(changed, pins, root, build))
    powershell = out / "inert-powershell-command.exe"
    powershell.write_bytes(b"INERT DATA, NEVER EXECUTED")
    tests["native_tool_lifetime"] = copy.deepcopy(next(iter(tests.values())))
    tests["native_tool_lifetime"]["command"][4] = "native_tool_lifetime"
    tests["native_tool_lifetime"]["command"][10:] = [str(powershell), "-NoProfile", "-File", str(wrapper)]
    rejects("PowerShell command requires reviewed file pin", lambda: driver.bind_commands(tests, pins, root, build))
    pins[powershell] = file_sha256(powershell)
    driver.bind_commands(tests, pins, root, build)
    check("PowerShell native_tool_lifetime role binds inert reviewed path", True)
    tests["native_tool_lifetime"]["command"][10] = str(out / "substituted-powershell.exe")
    rejects("PowerShell command substitution refused", lambda: driver.bind_commands(tests, pins, root, build))


def identity_cases(out, check, rejects):
    args = SimpleNamespace(commit="a" * 40, authored_sha256="b" * 64, configuration="Debug")
    formats = driver.identity_headers(driver.SELF.parent.parent, args)
    compiler = ('#if defined(__clang__)\n'
                'inline constexpr const char* compiler_version = __clang_version__;\n'
                '#elif defined(_MSC_VER)\n#define RACE_STRINGIFY_IMPL(x) #x\n'
                '#define RACE_STRINGIFY(x) RACE_STRINGIFY_IMPL(x)\n'
                'inline constexpr const char* compiler_version = "MSVC " RACE_STRINGIFY(_MSC_FULL_VER);\n'
                '#else\ninline constexpr const char* compiler_version = "unknown";\n#endif\n')
    expected = '#pragma once\nnamespace race::build {\n' + compiler + (
        f'inline constexpr const char* source_commit = "{args.commit}";\n'
        f'inline constexpr const char* authored_input_sha256 = "{args.authored_sha256}";\n'
        'inline constexpr const char* configuration = "Debug";\n'
        'inline constexpr const char* sanitizer = "none";\n}\n')
    check("produced format matches independently transcribed pinned producer", formats["produced"] == expected)
    header = out / "identity-format-fixture.hpp"
    for name, text in formats.items():
        for ending in ["\n", "\r\n"]:
            header.write_bytes(text.replace("\n", ending).encode())
            accepted, digest = driver.identity_header(header, formats)
            check(name + " exact fields/compiler and newline bytes accepted " + repr(ending),
                  accepted == name and digest == file_sha256(header))
    for label, old, new in [("commit", args.commit, "c" * 40), ("digest", args.authored_sha256, "d" * 64),
                             ("configuration", '"Debug"', '"Release"'), ("sanitizer", '"none"', '"asan"'),
                             ("compiler", "_MSC_FULL_VER", "_MSC_VER")]:
        header.write_bytes(expected.replace(old, new).encode())
        rejects("identity rejects wrong " + label, lambda: driver.identity_header(header, formats))
    header.write_bytes((expected + "// extra content\n").encode())
    rejects("identity rejects extra content", lambda: driver.identity_header(header, formats))
    header.write_bytes(formats["configured"].encode())
    rejects("post-build identity must use producer form", lambda: driver.identity_header(header, {"produced": expected}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    out = evidence_directory("p04-ordinary-driver-fixtures", args.output)
    receipt = {"kind": "inert-contract-fixtures", "status": "running", "checks": [], "native_stage_cases": [],
               "real_engine_pipeline": False, "driver_sha256": file_sha256(driver.SELF),
               "tests_sha256": file_sha256(Path(__file__)), "supervisor_sha256": file_sha256(driver.owned.SELF)}

    def check(name, value):
        receipt["checks"].append({"check": name, "passed": bool(value)})
        if not value:
            raise AssertionError(name)

    def rejects(name, action):
        try:
            action()
        except (ValueError, OSError):
            check(name, True)
        else:
            check(name, False)

    try:
        receipt["guard"] = native_guard(out)
        if not receipt["guard"]["passed"]:
            raise RuntimeError("real admission held")
        parser_cases(check, rejects)
        input_cases(out, check, rejects)
        git_environment_cases(out, check, rejects)
        ninja_cache_cases(out, check, rejects)
        pin_cases(out, check, rejects)
        command_cases(out, check, rejects)
        identity_cases(out, check, rejects)
        stage_cases(out, check, rejects, receipt)
        used = pipeline_case(out, "synthetic-complete", check)
        rejects("used output refused without overwrite", lambda: driver.run(used))
        pipeline_case(out, "synthetic-held", check, failure=("build", "held", 75))
        pipeline_case(out, "synthetic-child-failure", check, failure=("discover", "exited", 37))
        pipeline_case(out, "synthetic-test-failure", check, failure=("test", "exited", 8))
        pipeline_case(out, "synthetic-stale", check, stale=True)
        pipeline_case(out, "synthetic-configure", check, configure=True, stale=True)
        pipeline_case(out, "synthetic-inventory-drift", check, mutate_inventory=True)
        pipeline_case(out, "synthetic-idempotent-header", check, initial="produced")
        for mutation in ["wrong-identity", "wrong-compiler", "other-config", "later-header"]:
            pipeline_case(out, "synthetic-" + mutation, check, mutation=mutation)
        receipt["status"] = "passed"
    except BaseException as error:
        receipt.update(status="held" if "real admission held" in str(error) else "failed", error=repr(error))
    finally:
        receipt["unchanged"] = all(file_sha256(p) == receipt[k] for p, k in [
            (driver.SELF, "driver_sha256"), (Path(__file__), "tests_sha256"), (driver.owned.SELF, "supervisor_sha256")])
        with (out / "tests.json").open("x", encoding="utf-8") as stream:
            json.dump(receipt, stream, indent=2)
    print(json.dumps({"status": receipt["status"], "checks": len(receipt["checks"]),
                      "error": receipt.get("error"), "receipt": str(out / "tests.json")}))
    return 0 if receipt["status"] == "passed" else 75 if receipt["status"] == "held" else 1


if __name__ == "__main__":
    raise SystemExit(main())

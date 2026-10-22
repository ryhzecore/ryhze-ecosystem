#!/usr/bin/env python3
"""Run prebuilt Cargo test suites serially; never invoke Cargo or compile Rust.

Run this orchestrator directly in the prepared native Windows environment, not
inside one aggregate Job Object for the whole workload. Each listing/test gets
its own unchanged axeon-process.exe job and fresh host admission. Example:

python -B scripts/record-windows-tests.py --prebuild-receipt RUN/result.json \
  --source-manifest CAPTURE/source-manifest.json --target-dir TARGET \
  --profile debug --compiler TARGET/debug/axeon.exe \
  --runtime TARGET/debug/axeon_tensor_runtime.lib --supervisor HELPER.exe \
  --output-dir C:/Projects/Axeon/Evidence/YYYY-MM-DD/windows-tests/attempt-01/run

The prebuild command must contain test --workspace --no-run --offline --locked
--jobs 1 --message-format=json. --suite PACKAGE::KIND::NAME limits scope explicitly.
--split-suite ID runs every listed test of that suite in a separate guarded job;
--exact-test ID=NAME explicitly selects individual tests (requires --split-suite).
There is no automatic splitting, retry, resource-limit increase, or wait loop.
--resume-from RESULT reuses only verified successful execution units with the
same source, binaries, recipe and explicit selection. Use a new output directory.
Exit75 is a resource hold; exit1 a failure; exit0 means the recorded scope passed,
not that the complete platform/release acceptance gates passed.
"""
import argparse
import ctypes
from ctypes import wintypes
import datetime
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import time

API = runpy.run_path(str(Path(__file__).with_name("reproduce-windows.py")))
need, safe, regular, sha, load, write_json = (
    API[name] for name in ("need", "safe_path", "regular", "sha", "load", "write_json"))
MAX_LOG = 64 << 20
MAX_TESTS = 10000
MAX_SUITES = 256
MIN_PHYSICAL = 2 << 30
MIN_COMMIT = 4 << 30
MIN_DISK = 4 << 30


class ResourceHold(RuntimeError):
    pass


class MemoryStatus(ctypes.Structure):
    _fields_ = [("dwLength", wintypes.DWORD), ("dwMemoryLoad", wintypes.DWORD),
                ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def admission(disk_root):
    status = MemoryStatus()
    status.dwLength = ctypes.sizeof(status)
    query = ctypes.WinDLL("kernel32", use_last_error=True).GlobalMemoryStatusEx
    query.argtypes, query.restype = [ctypes.POINTER(MemoryStatus)], wintypes.BOOL
    if not query(ctypes.byref(status)):
        raise ctypes.WinError(ctypes.get_last_error())
    disk = shutil.disk_usage(disk_root).free
    return {"utc": utc(), "free_physical_bytes": status.ullAvailPhys,
            "free_commit_bytes": status.ullAvailPageFile, "free_disk_bytes": disk,
            "minimum_physical_bytes": MIN_PHYSICAL, "minimum_commit_bytes": MIN_COMMIT,
            "minimum_disk_bytes": MIN_DISK,
            "ready": status.ullAvailPhys >= MIN_PHYSICAL and status.ullAvailPageFile >= MIN_COMMIT and disk >= MIN_DISK}


def frozen_manifest(path):
    manifest = load(path)
    need(re.fullmatch(r"[0-9a-f]{40}", manifest.get("commit", "")), "full frozen source commit required")
    need(re.fullmatch(r"[0-9a-f]{64}", manifest.get("archive_sha256", "")), "frozen source archive identity required")
    source = safe(manifest["source"])
    need(source.is_dir() and not (source / ".git").exists(), "a frozen non-Git source snapshot is required")
    entries = manifest["files"]
    need(isinstance(entries, list) and 0 < len(entries) <= API["MAX_FILES"], "invalid frozen source file inventory")
    files, folded = {}, set()
    for entry in entries:
        name = API["relative_name"](entry["path"])
        need(name.casefold() not in folded, "duplicate or case-aliased frozen input")
        need(re.fullmatch(r"[0-9a-f]{64}", entry.get("sha256", "")), "invalid frozen input hash")
        files[name] = entry["sha256"]
        folded.add(name.casefold())
    API["verify_source"](source, files)
    return manifest, source, files


def option(command, name):
    found = []
    for index, value in enumerate(command):
        if value == name:
            need(index + 1 < len(command), f"missing value after {name}")
            found.append(command[index + 1])
        elif value.startswith(name + "="):
            found.append(value.split("=", 1)[1])
    need(len(found) <= 1, f"duplicate prebuild option {name}")
    return found[0] if found else None


def artifacts(args, source, files):
    receipt = load(args.prebuild_receipt)
    command = receipt.get("command", [])
    need(receipt.get("exit_code") == 0 and receipt.get("wsl_invoked_by_harness") is False
         and receipt.get("jobs") == 1 and receipt.get("test_threads") == 1,
         "successful serial native prebuild receipt required")
    need(isinstance(command, list) and len(command) >= 2 and command[1] == "test"
         and Path(command[0]).name.lower() in ("cargo", "cargo.exe"), "Cargo test prebuild required")
    need(all(value in command for value in ("--workspace", "--no-run", "--offline", "--locked"))
         and option(command, "--jobs") == "1" and option(command, "--message-format") == "json",
         "prebuild must be a locked offline workspace no-run build with jobs1 and JSON output")
    filters = ("--exclude", "--package", "-p", "--lib", "--bin", "--bins", "--test", "--tests", "--doc", "--bench", "--benches", "--example", "--examples")
    need("--" not in command and not any(value == flag or value.startswith(flag + "=") for value in command for flag in filters),
         "prebuild target/package filtering cannot establish the workspace test inventory")
    need(("--release" in command) == (args.profile == "release") and option(command, "--profile") is None,
         "prebuild profile differs")
    need(safe(receipt["cwd"]) == source, "prebuild source directory differs from frozen manifest")
    target = safe(args.target_dir)
    need(target.is_dir() and not target.is_relative_to(source), "target directory must be separate from frozen source")
    declared_target = option(command, "--target-dir")
    if declared_target is not None:
        declared = Path(declared_target)
        need(safe(declared if declared.is_absolute() else source / declared) == target, "prebuild target directory differs")
    triple = option(command, "--target")
    need(triple in (None, API["TARGET"]), "prebuild target is not native Windows x64")
    profile_dir = target / API["TARGET"] / args.profile if triple else target / args.profile
    for name in ("stdout.log", "stderr.log"):
        log = regular(args.prebuild_receipt.parent / name, MAX_LOG)
        saved = receipt.get(name, {})
        need(saved.get("bytes") == log.stat().st_size and saved.get("sha256") == sha(log, MAX_LOG),
             f"prebuild {name} differs from receipt")
    suites, compiler_seen, finished = {}, False, False
    with (args.prebuild_receipt.parent / "stdout.log").open("r", encoding="utf-8") as stream:
        for line in stream:
            need(len(line) <= 2 << 20, "Cargo JSON line budget exceeded")
            item = json.loads(line)
            if item.get("reason") == "build-finished":
                need(not finished and item.get("success") is True, "prebuild did not finish successfully")
                finished = True
            if item.get("reason") != "compiler-artifact":
                continue
            manifest_path, src_path = safe(item["manifest_path"]), safe(item["target"]["src_path"])
            need(manifest_path.is_relative_to(source) and src_path.is_relative_to(source)
                 and manifest_path.relative_to(source).as_posix() in files and src_path.relative_to(source).as_posix() in files,
                 "Cargo artifact refers outside the frozen source inventory")
            executable = item.get("executable")
            if executable is None:
                continue
            executable = regular(executable)
            need(executable.is_relative_to(profile_dir) and executable.suffix.lower() == ".exe", "test binary outside selected target/profile")
            API["pe"](executable)
            if item.get("profile", {}).get("test") is not True:
                if item["target"]["name"] == "axeon" and "bin" in item["target"]["kind"]:
                    need(executable == regular(args.compiler), "prebuild compiler artifact differs")
                    compiler_seen = True
                continue
            need(executable.parent == profile_dir / "deps", "test binary must be a Cargo deps artifact")
            kinds = item["target"]["kind"]
            need(kinds and len(kinds) == len(set(kinds)) and set(kinds) <= {"lib", "bin", "test", "staticlib", "rlib"},
                 "unsupported test target kind")
            kind = "+".join(sorted(kinds))
            package = manifest_path.parent.relative_to(source).as_posix()
            package = "root" if package == "." else package.replace("/", "-")
            suite_id = f"{package}::{kind}::{item['target']['name']}"
            need(suite_id not in suites and len(suites) < MAX_SUITES, "duplicate or excessive test suites")
            suites[suite_id] = {"id": suite_id, "executable": str(executable), "sha256": sha(executable),
                                "source_path": str(src_path), "manifest_path": str(manifest_path),
                                "package_id": item["package_id"], "target": item["target"]}
    need(finished and suites and compiler_seen, "prebuild is missing completed suites or the non-test compiler artifact")
    need(regular(args.runtime) == profile_dir / "axeon_tensor_runtime.lib", "runtime is outside selected target/profile")
    with regular(args.runtime).open("rb") as stream:
        need(stream.read(8) == b"!<arch>\n", "runtime is not a static archive")
    return dict(sorted(suites.items()))


def listed(raw):
    names, summary = [], None
    for line in raw.decode("utf-8").splitlines():
        match = re.fullmatch(r"(.+): test", line)
        if match:
            names.append(match[1])
        elif re.fullmatch(r"[0-9]+ tests?, [0-9]+ benchmarks?", line):
            need(summary is None, "duplicate test list summary")
            summary = tuple(map(int, re.findall(r"[0-9]+", line)))
        else:
            need(not line, f"unexpected test-list output: {line[:160]}")
    need(summary == (len(names), 0) and len(names) <= MAX_TESTS and len(set(names)) == len(names),
         "test list is incomplete, duplicated, or includes unsupported benchmarks")
    return sorted(names)


def observed(raw, names, total):
    values, summary = {}, None
    for line in raw.decode("utf-8").splitlines():
        match = re.fullmatch(r"test (.+) \.\.\. (ok|FAILED|ignored)(?:, .*)?", line)
        if match:
            need(match[1] not in values, "duplicate test result")
            values[match[1]] = match[2]
        match = re.fullmatch(r"test result: (?:ok|FAILED)\. ([0-9]+) passed; ([0-9]+) failed; ([0-9]+) ignored; ([0-9]+) measured; ([0-9]+) filtered out; finished in .+", line)
        if match:
            need(summary is None, "duplicate test result summary")
            summary = tuple(map(int, match.groups()))
    need(set(values) == set(names) and summary is not None, "observed test names differ from selected manifest")
    expected = (sum(x == "ok" for x in values.values()), sum(x == "FAILED" for x in values.values()),
                sum(x == "ignored" for x in values.values()), 0, total - len(names))
    need(summary == expected, "test summary differs from observed names/counts")
    return {"passed": summary[0], "failed": summary[1], "ignored": summary[2],
            "measured": summary[3], "filtered_out": summary[4], "results": values}


def run_child(helper, command, cwd, env, root, entry, seconds, save):
    entry.update(command=list(map(str, command)), status="admission", admission=admission(root))
    save()
    if not entry["admission"]["ready"]:
        entry["status"] = "resource_hold"
        save()
        raise ResourceHold("host RAM, commit or disk admission failed; no child started")
    child, start = None, time.perf_counter()
    try:
        with (root / "stdout.log").open("xb") as out, (root / "stderr.log").open("xb") as err:
            child = subprocess.Popen([str(helper), "--timeout-seconds", str(seconds), "--", *map(str, command)],
                                     cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=out, stderr=err)
            entry.update(status="running", supervisor_pid=child.pid)
            save()
            try:
                entry["exit_code"] = child.wait(timeout=seconds + 15)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=10)
                entry.update(exit_code=124, outer_deadline=True)
    finally:
        if child is not None and child.poll() is None:
            child.kill()
            child.wait(timeout=10)
        entry["elapsed_seconds"] = time.perf_counter() - start
        for name in ("stdout.log", "stderr.log"):
            path = root / name
            if path.exists():
                entry[name] = {"path": str(path), "bytes": path.stat().st_size, "sha256": sha(path)}
        save()
    entry["status"] = "executed"
    save()
    need(regular(root / "stdout.log", MAX_LOG).stat().st_size <= MAX_LOG, "test output exceeds parse budget")
    return (root / "stdout.log").read_bytes()


def resume_units(path, identity, selection):
    if path is None:
        return {}
    old = load(path)
    need(old.get("kind") == "axeon-windows-serial-tests" and old.get("identity") == identity
         and old.get("selection") == selection, "resume source, binaries, recipe or selection differs")
    result = {}
    for unit in old.get("units", []):
        if unit.get("status") != "passed":
            continue
        for name in ("stdout.log", "stderr.log"):
            record = unit[name]
            need(sha(record["path"]) == record["sha256"] and regular(record["path"]).stat().st_size == record["bytes"],
                 "completed resume unit logs changed")
        parsed = observed(regular(unit["stdout.log"]["path"], MAX_LOG).read_bytes(), unit["expected_names"], unit["suite_test_count"])
        need(parsed == unit["observed"] and unit.get("exit_code") == 0 and parsed["failed"] == 0,
             "completed resume unit result differs")
        need(unit["key"] not in result, "duplicate resume unit")
        result[unit["key"]] = unit
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ("prebuild-receipt", "source-manifest", "target-dir", "compiler", "runtime", "supervisor", "output-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--profile", choices=("debug", "release"), required=True)
    parser.add_argument("--suite", action="append", default=[])
    parser.add_argument("--split-suite", action="append", default=[])
    parser.add_argument("--exact-test", action="append", default=[])
    parser.add_argument("--include-ignored", action="store_true")
    parser.add_argument("--seconds", type=int, default=300)
    parser.add_argument("--resume-from", type=Path)
    args = parser.parse_args()
    API["native_host"]()
    need(1 <= args.seconds <= 3600, "wall timeout must be between1 and3600seconds; CPU limit remains180seconds")
    root = API["fresh"](API["evidence_location"](args.output_dir))
    receipt_path = root / "tests.json"
    result = {"schema_version": 1, "kind": "axeon-windows-serial-tests", "status": "verifying", "utc": utc(),
              "target": API["TARGET"], "profile": args.profile, "jobs": 1, "test_threads": 1,
              "wsl_invoked_by_harness": False, "compiled_by_harness": False, "acceptance_claim": False,
              "inventory_scope": "Cargo JSON prebuilt test executables; doctests and non-test acceptance gates are separate",
              "limits": {"job_committed_bytes": 2 << 30, "job_cpu_seconds": 180, "active_processes": 256,
                         "wall_seconds_per_command": args.seconds}, "listings": [], "units": [], "continuation": {}}
    def save():
        write_json(receipt_path, result)
    save()
    exit_code = 1
    try:
        manifest, source, files = frozen_manifest(args.source_manifest)
        suites = artifacts(args, source, files)
        helper, compiler, runtime = map(regular, (args.supervisor, args.compiler, args.runtime))
        API["pe"](helper)
        identity = {"source_commit": manifest["commit"], "source_manifest_sha256": sha(args.source_manifest),
                    "archive_sha256": manifest["archive_sha256"], "prebuild_receipt_sha256": sha(args.prebuild_receipt),
                    "compiler_sha256": sha(compiler), "runtime_sha256": sha(runtime), "supervisor_sha256": sha(helper),
                    "recipe_sha256": sha(__file__), "shared_recipe_sha256": sha(Path(__file__).with_name("reproduce-windows.py")),
                    "test_binary_sha256": {key: item["sha256"] for key, item in suites.items()}}
        selected = sorted(set(args.suite or suites))
        split = sorted(set(args.split_suite))
        need(set(selected) <= set(suites) and set(split) <= set(selected), "unknown/unselected suite ID")
        exact = {}
        for value in args.exact_test:
            suite_id, separator, name = value.partition("=")
            need(separator and suite_id in split and name, "--exact-test requires ID=NAME and an explicitly split suite")
            need(name not in exact.setdefault(suite_id, []), "duplicate exact test selection")
            exact[suite_id].append(name)
        selection = {"suites": selected, "split_suites": split, "exact_tests": {key: sorted(value) for key, value in exact.items()},
                     "include_ignored": args.include_ignored, "seconds": args.seconds}
        result.update(identity, identity=identity, selection=selection, artifact_inventory=suites,
                      artifact_hash_timing="verified at execution, Cargo JSON does not contain build-time hashes",
                      source=str(source), target_dir=str(safe(args.target_dir)), compiler=str(compiler), runtime=str(runtime),
                      supervisor=str(helper), source_manifest=str(regular(args.source_manifest)),
                      prebuild_receipt=str(regular(args.prebuild_receipt)), status="listing")
        if args.resume_from:
            result["resume_from"] = {"path": str(regular(args.resume_from)), "sha256": sha(args.resume_from)}
        resumed = resume_units(args.resume_from, identity, selection)
        env = os.environ.copy()
        env.update(CARGO_BUILD_JOBS="1", RUST_TEST_THREADS="1", CARGO_INCREMENTAL="0", AXEON_PROCESS_TEST=str(helper),
                   AXEON_TENSOR_RUNTIME=str(runtime))
        def unchanged():
            need(sha(helper) == identity["supervisor_sha256"] and sha(compiler) == identity["compiler_sha256"]
                 and sha(runtime) == identity["runtime_sha256"], "compiler/runtime/helper bytes changed")
            need(sha(args.source_manifest) == identity["source_manifest_sha256"]
                 and sha(args.prebuild_receipt) == identity["prebuild_receipt_sha256"], "source/prebuild receipt changed")
        plan = []
        for index, (suite_id, suite) in enumerate(suites.items()):
            # List the complete prebuild inventory even when execution scope is narrower.
            executable = regular(suite["executable"])
            need(sha(executable) == suite["sha256"], "test binary changed before listing")
            unchanged()
            entry = {"suite": suite_id, "status": "pending"}
            result["listings"].append(entry)
            result["continuation"] = {"phase": "list", "suite": suite_id}
            directory = API["fresh"](root / f"list-{index:03d}")
            raw = run_child(helper, [executable, "--list", "--format", "pretty", "--color", "never"],
                            Path(suite["manifest_path"]).parent, env, directory, entry, min(args.seconds, 45), save)
            need(entry["exit_code"] == 0, f"test listing failed ({entry['exit_code']}); see {directory}")
            names = listed(raw)
            need(entry["stderr.log"]["bytes"] == 0, "unexpected stderr while listing tests")
            entry.update(status="passed", expected_names=names, test_count=len(names))
            if suite_id not in selected:
                continue
            requested = sorted(exact.get(suite_id, names))
            need(set(requested) <= set(names), "exact test selection is not in this suite's manifest")
            groups = [[name] for name in requested] if suite_id in split else [names]
            if not groups:
                groups = [[]]
            for group in groups:
                key = suite_id + ("=" + group[0] if suite_id in split and group else "")
                plan.append({"key": key, "suite": suite_id, "expected_names": group, "suite_test_count": len(names),
                             "exact": suite_id in split and bool(group), "binary_sha256": suite["sha256"]})
            save()
        need(sum(x["test_count"] for x in result["listings"]) <= MAX_TESTS, "workspace test inventory budget exceeded")
        result["plan"] = plan
        result["status"] = "running"
        for index, unit in enumerate(plan):
            result["continuation"] = {"phase": "test", "unit": unit["key"], "remaining_units": [x["key"] for x in plan[index:]]}
            old = resumed.get(unit["key"])
            if old is not None:
                need(all(old[field] == value for field, value in unit.items()), "resume execution plan changed")
                result["units"].append({**old, "reused_from": str(args.resume_from)})
                save()
                continue
            suite = suites[unit["suite"]]
            unchanged()
            need(sha(suite["executable"]) == unit["binary_sha256"], "test binary changed before execution")
            entry = {**unit, "status": "pending"}
            result["units"].append(entry)
            directory = API["fresh"](root / f"test-{index:03d}")
            command = [suite["executable"], "--test-threads", "1", "--format", "pretty", "--color", "never"]
            if unit["exact"]:
                command.extend(["--exact", unit["expected_names"][0]])
            if args.include_ignored:
                command.append("--include-ignored")
            raw = run_child(helper, command, Path(suite["manifest_path"]).parent, env, directory, entry, args.seconds, save)
            counts = observed(raw, unit["expected_names"], unit["suite_test_count"])
            entry.update(status="failed", observed=counts)
            need(entry["exit_code"] == 0 and counts["failed"] == 0, "test failures recorded")
            entry["status"] = "passed"
            save()
        unchanged()
        API["verify_source"](source, files)
        totals = {name: sum(item["observed"][name] for item in result["units"]) for name in ("passed", "failed", "ignored")}
        available = sum(item["test_count"] for item in result["listings"])
        complete = totals["passed"] == available and totals["ignored"] == 0 and set(selected) == set(suites)
        result.update(status="passed" if complete else "partial", totals=totals, available_tests=available,
                      executed_scope_passed=True, complete_prebuilt_inventory_passed=complete,
                      continuation={"phase": "complete", "remaining_units": []})
        exit_code = 0
    except ResourceHold as error:
        result.update(status="resource_hold", error=str(error), resume_hint="Use --resume-from this tests.json in a fresh Evidence directory; selection and inputs must match")
        exit_code = 75
    except BaseException as error:
        result.update(status="failed", error=f"{type(error).__name__}: {error}",
                      resume_hint="Preserve this attempt; resolve the failure and use a fresh run. Unchanged successful units may be reused with --resume-from")
        exit_code = 130 if isinstance(error, KeyboardInterrupt) else 1
    finally:
        result["finished_utc"] = utc()
        save()
    print(json.dumps({"status": result["status"], "receipt": str(receipt_path), "totals": result.get("totals"), "exit_code": exit_code}))
    return exit_code


if __name__ == "__main__":
    sys.exit(main())

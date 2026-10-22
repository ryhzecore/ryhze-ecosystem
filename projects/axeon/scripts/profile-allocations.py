#!/usr/bin/env python3
"""Validate and measure the opt-in Axeon Rust allocation profiler on Linux.

New process per warmup/sample; allocation counts, not instrumented speed claims.
Only the Python standard library and sibling profile-compiler.py are required.
No builds, installations, native compiler calls, or network operations occur.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import stat
import sys


SHARED_PATH = Path(__file__).with_name("profile-compiler.py")
_spec = importlib.util.spec_from_file_location("axeon_compiler_profile_shared", SHARED_PATH)
shared = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(shared)

VERSION = "axeon-allocation-profile-1"
MAX_REPORT_BYTES = 128 * 1024
MAX_ARTIFACT_BYTES = 64 * 1024 * 1024
FUEL = 1_000_000
PROJECT_PHASES = ("load", "check", "verify-ir", "emit-ir", "drop-ir-output", "interpret",
                  "drop-execution", "drop-program", "drop-project")
FORMAT_PHASES = ("read-source", "format", "drop-format-output", "drop-source")
PHASE_PLANS = {
    ("project", "success"): (PROJECT_PHASES, ()),
    ("project", "load_error"): (("load", "drop-load-error"), PROJECT_PHASES[1:]),
    ("project", "check_error"): (("load", "check", "drop-check-error", "drop-project"), PROJECT_PHASES[2:-1]),
    ("project", "verification_error"): (
        ("load", "check", "verify-ir", "drop-verification-error", "drop-program", "drop-project"),
        ("emit-ir", "drop-ir-output", "interpret", "drop-execution")),
    ("format", "success"): (FORMAT_PHASES, ()),
    ("format", "format_error"): (FORMAT_PHASES, ()),
    ("format", "read_error"): (("read-source", "drop-read-error"), FORMAT_PHASES[1:]),
}
EVENTS = (
    "alloc_attempts", "alloc_successes", "alloc_failures",
    "zeroed_attempts", "zeroed_successes", "zeroed_failures",
    "realloc_attempts", "realloc_successes", "realloc_failures", "dealloc_calls",
    "allocation_requested_bytes", "allocation_successful_bytes", "realloc_requested_bytes",
    "realloc_successful_old_bytes", "realloc_successful_new_bytes", "deallocated_bytes",
)
STATE = ("live_requested_bytes_begin", "live_requested_bytes_end", "live_blocks_begin",
         "live_blocks_end", "peak_live_requested_bytes", "peak_increase_over_begin_bytes")
WORKLOADS = ("arithmetic-project", "aggregate-modules-project", "format-success", "format-budget-rejection")
MODULE_SOURCES = {
    "modules/main.ax": b"""import shapes; import bridge;
fn main()->i64{
 let a:[i64;2]=[20,22];let b:[i64;2]=bridge::numbers(a);
 let r:shapes::R=shapes::make(b[0]+b[1]);let s:shapes::R=bridge::record_value(r);
 let u:shapes::U=shapes::U::Value{n:s.n,real:s.real};let v:shapes::U=bridge::union_value(u);
 match v{shapes::U::Value{n:n,real:x}=>{print(x);print(n);return n;},shapes::U::Empty{}=>{return -1;},}
}
""",
    "modules/shapes.ax": b"""export record R{n:i64,real:f64}
export union U{Value{n:i64,real:f64},Empty{}}
export fn make(n:i64)->R{return R{n:n,real:-0.0};}
""",
    "modules/bridge.ax": b"""import shapes;
export fn numbers(a:[i64;2])->[i64;2]{return a;}
export fn record_value(r:shapes::R)->shapes::R{return r;}
export fn union_value(u:shapes::U)->shapes::U{return u;}
""",
}


class AllocationError(RuntimeError):
    pass


def demand(condition, message):
    if not condition:
        raise AllocationError(message)


def regular(path, limit):
    metadata = Path(path).lstat()
    demand(stat.S_ISREG(metadata.st_mode), f"not a regular non-symlink file: {path}")
    demand(metadata.st_size <= limit, f"file exceeds evidence limit: {path}")
    return metadata.st_size


def fingerprint(path, limit=MAX_ARTIFACT_BYTES):
    size = regular(path, limit)
    return {"bytes": size, "sha256": shared.digest(path)}


def unchanged(frozen):
    for path, expected in frozen.items():
        demand(fingerprint(path) == expected, f"recorded input, executable, or output changed: {path}")


def integer(value, name, signed=False):
    demand(type(value) is int and (signed or value >= 0), f"{name} is not a valid integer")
    demand(-(1 << 64) <= value <= (1 << 64) - 1, f"{name} exceeds counter range")
    return value


def validate_phase(phase):
    demand(isinstance(phase, dict), "phase is not an object")
    demand(set(phase) == {"name", "retained_at_end", "accounting_valid", "events",
                         *STATE, "net_live_requested_bytes"}, "unknown or missing phase field")
    demand(type(phase["name"]) is str and type(phase["retained_at_end"]) is str,
           "phase name/retained objects are not text")
    demand(phase["accounting_valid"] is True, "profiler invalidated allocation accounting")
    e = phase["events"]
    demand(isinstance(e, dict) and set(e) == set(EVENTS), "unknown or missing counter")
    for name, value in e.items():
        integer(value, name)
    for name in STATE:
        integer(phase[name], name)
    integer(phase["net_live_requested_bytes"], "net_live_requested_bytes", signed=True)
    for kind in ("alloc", "zeroed", "realloc"):
        demand(e[f"{kind}_attempts"] == e[f"{kind}_successes"] + e[f"{kind}_failures"],
               f"{kind} attempts do not equal successes plus failures")
    begin, end = phase["live_requested_bytes_begin"], phase["live_requested_bytes_end"]
    expected = (begin + e["allocation_successful_bytes"] + e["realloc_successful_new_bytes"]
                - e["realloc_successful_old_bytes"] - e["deallocated_bytes"])
    demand(end == expected, "live requested-byte balance is inconsistent")
    demand(phase["live_blocks_end"] == phase["live_blocks_begin"] + e["alloc_successes"]
           + e["zeroed_successes"] - e["dealloc_calls"], "live block balance is inconsistent")
    peak = phase["peak_live_requested_bytes"]
    demand(peak >= max(begin, end), "peak is below an endpoint")
    demand(peak <= begin + e["allocation_successful_bytes"] + e["realloc_successful_new_bytes"],
           "peak exceeds all available requested allocation bytes")
    demand(phase["peak_increase_over_begin_bytes"] == peak - begin, "peak increase is inconsistent")
    demand(phase["net_live_requested_bytes"] == end - begin, "net live-byte change is inconsistent")
    demand(e["allocation_requested_bytes"] >= e["allocation_successful_bytes"],
           "successful allocation bytes exceed requests")
    demand(e["realloc_requested_bytes"] >= e["realloc_successful_new_bytes"],
           "successful realloc bytes exceed requests")
    for calls, total in ((e["alloc_attempts"] + e["zeroed_attempts"], "allocation_requested_bytes"),
                         (e["alloc_successes"] + e["zeroed_successes"], "allocation_successful_bytes"),
                         (e["realloc_attempts"], "realloc_requested_bytes"),
                         (e["realloc_successes"], "realloc_successful_old_bytes"),
                         (e["realloc_successes"], "realloc_successful_new_bytes"),
                         (e["dealloc_calls"], "deallocated_bytes")):
        demand((calls == 0 and e[total] == 0) or (calls > 0 and e[total] >= calls),
               f"{total} does not correspond to nonzero-size calls")


def load_json(path):
    regular(path, MAX_REPORT_BYTES)
    def unique(pairs):
        result = {}
        for key, value in pairs:
            demand(key not in result, "duplicate JSON key")
            result[key] = value
        return result
    return json.loads(Path(path).read_text(encoding="utf-8"), object_pairs_hook=unique,
                      parse_constant=lambda value: (_ for _ in ()).throw(AllocationError(f"nonfinite JSON {value}")))


def validate_result(directory, process, expectation):
    """Independently verify complete counters, actual artifacts and exact outcome."""
    demand(process["timed_out"] is False, "profiler child timed out")
    demand(process["returncode"] == expectation["exit"], "unexpected profiler exit status")
    demand(process["stdout"]["bytes"] == 0 and process["stderr"]["bytes"] == 0,
           "profiler wrote unexpected process output")
    directory = Path(directory)
    demand(directory.is_dir() and not directory.is_symlink(), "missing or linked sample directory")
    before = {}
    for entry in directory.iterdir():
        demand(len(before) < 5, "sample has unexpected extra files")
        before[entry] = fingerprint(entry, MAX_REPORT_BYTES if entry.name == "report.json" else MAX_ARTIFACT_BYTES)
    demand(directory / "report.json" in before, "missing report.json")
    report = load_json(directory / "report.json")
    demand(report.get("schema_version") == 1 and type(report.get("schema_version")) is int, "unsupported schema")
    demand(report.get("profiler_version") == "0.1.0", "unsupported profiler version")
    demand(report.get("measurement") == "observed-rust-global-allocator-calls", "wrong measurement scope")
    demand(report.get("status") == "complete" and report.get("active_phase") is None,
           "incomplete or invalid allocation report")
    demand(report.get("failure") is None, "allocation report records a tool failure")
    demand(report.get("mode") == expectation["mode"] and report.get("outcome") == expectation["outcome"],
           "allocation report outcome/mode mismatch")
    demand(report.get("fuel") == FUEL and report.get("max_fuel") == 10_000_000, "fuel configuration mismatch")
    if expectation["mode"] == "project":
        scope = report.get("scope")
        demand(isinstance(scope, dict) and scope.get("check_includes_verification") is True
               and scope.get("verify_ir_repeats_check_verification") is True
               and scope.get("phases_are_disjoint_pipeline") is False,
               "explicit verification must disclose duplicate check work, not disjoint pipeline totals")
    phases = report.get("phases")
    plan = PHASE_PLANS.get((expectation["mode"], expectation["outcome"]))
    demand(plan is not None, "unsupported expected phase outcome")
    expected_phases, expected_skipped = plan
    demand(isinstance(phases, list) and [p.get("name") for p in phases if isinstance(p, dict)] == list(expected_phases)
           and len(phases) == len(expected_phases), "missing, extra, or unordered phase")
    for phase in phases:
        validate_phase(phase)
    demand(report.get("skipped_phases") == list(expected_skipped), "incorrect skipped phases for outcome")
    demand(report.get("diagnostic_codes") == expectation["codes"], "wrong diagnostic codes")
    demand(report.get("result") == expectation["result"] and report.get("trap") is None, "wrong execution result/trap")
    if expectation["result"] is not None:
        integer(report["result"], "result", signed=True)
        demand(-(1 << 63) <= report["result"] < (1 << 63), "result is outside i64")
    sources = report.get("sources")
    demand(isinstance(sources, list) and len(sources) == len(expectation["sources"]), "source inventory mismatch")
    seen_sources = {}
    for source in sources:
        demand(isinstance(source, dict) and set(source) == {"logical_path", "bytes"}, "malformed source entry")
        name = source["logical_path"]
        demand(type(name) is str and name not in seen_sources, "duplicate source entry")
        seen_sources[name] = integer(source["bytes"], "source bytes")
    demand(seen_sources == expectation["sources"], "frozen source names/sizes mismatch")
    artifacts = report.get("artifacts")
    demand(isinstance(artifacts, list), "missing artifact inventory")
    actual = {}
    for artifact in artifacts:
        demand(isinstance(artifact, dict) and set(artifact) == {"path", "bytes"}, "malformed artifact entry")
        name = artifact["path"]
        demand(type(name) is str and name in expectation["artifacts"] and name not in actual,
               "unexpected, duplicate or unsafe artifact path")
        path = directory / name
        demand(path in before, "declared artifact is missing")
        demand(integer(artifact["bytes"], "artifact bytes") == before[path]["bytes"], "artifact byte size mismatch")
        wanted = expectation["artifacts"][name]
        if "sha256" in wanted:
            demand(before[path]["sha256"] == wanted["sha256"], "artifact differs from independent ordinary-compiler result")
        if "contains" in wanted:
            demand(wanted["contains"].encode("ascii") in path.read_bytes(), "diagnostic artifact lacks expected error")
        actual[name] = before[path]
    demand(set(actual) == set(expectation["artifacts"]), "artifact inventory is incomplete")
    demand({p.name for p in before} == {"report.json", *actual}, "undeclared sample files")
    # Check report and output bytes again after parsing and verification.
    unchanged(before)
    return report, before


def save_report(directory, report):
    temporary = Path(directory) / "allocations.json.tmp"
    with temporary.open("x", encoding="utf-8") as output:
        json.dump(report, output, indent=2, allow_nan=False)
        output.write("\n")
    temporary.replace(Path(directory) / "allocations.json")


def summarize_samples(samples):
    summaries = {}
    for workload in WORKLOADS:
        selected = [s for s in samples if s["workload"] == workload and s["round_kind"] == "measured" and s["valid"]]
        demand(selected, f"no accepted measured samples for {workload}")
        names = [p["name"] for p in selected[0]["allocation_report"]["phases"]]
        summaries[workload] = {}
        for name in names:
            phases = [next(p for p in sample["allocation_report"]["phases"] if p["name"] == name) for sample in selected]
            summaries[workload][name] = {metric: shared.summarize([p[metric] for p in phases]) for metric in STATE}
            summaries[workload][name]["events"] = {
                metric: shared.summarize([p["events"][metric] for p in phases]) for metric in EVENTS}
            # The shared summarizer intentionally accepts only nonnegative
            # values. Preserve signed lifetime changes explicitly as raw data.
            summaries[workload][name]["net_live_requested_bytes_raw"] = [p["net_live_requested_bytes"] for p in phases]
    return summaries


def profile(compiler, profiler, directory, trials, label, timeout_seconds):
    sources, expected_arithmetic = shared.generated_sources()
    sources.update(MODULE_SOURCES)
    inputs = directory / "inputs"
    inputs.mkdir()
    (inputs / "modules").mkdir()
    for name, source in sources.items():
        with (inputs / name).open("xb") as output:
            output.write(source)
    samples_dir, preparation_dir = directory / "samples", directory / "preparation"
    samples_dir.mkdir()
    preparation_dir.mkdir()
    frozen = {path: fingerprint(path) for path in [compiler, profiler, Path(__file__).resolve(), SHARED_PATH.resolve(),
                                                  *(inputs / name for name in sources)]}
    environment = os.environ.copy()
    environment.update({"LC_ALL": "C", "LANG": "C", "TZ": "UTC", "SOURCE_DATE_EPOCH": "0"})
    report = {
        "schema_version": 1, "workload_version": VERSION, "status": "running", "label": label,
        "captured_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "compiler": {"path": str(compiler), **frozen[compiler]},
        "profiler": {"path": str(profiler), **frozen[profiler]},
        "harness": {"path": str(Path(__file__).resolve()), **frozen[Path(__file__).resolve()]},
        "shared_harness": {"path": str(SHARED_PATH.resolve()), **frozen[SHARED_PATH.resolve()]},
        "inputs": {name: frozen[inputs / name] for name in sources},
        "host_before": shared.host_configuration(),
        "configuration": {"trials": trials, "warmups": 1, "fuel": FUEL, "timeout_seconds": timeout_seconds,
                          "workload_order": list(WORKLOADS), "order": "one warmup round then fixed interleaved measured rounds",
                          "environment": {k: environment[k] for k in ("LC_ALL", "LANG", "TZ", "SOURCE_DATE_EPOCH")}},
        "scope": {
            "counts": "Observed System-forwarded Rust global allocator calls in the separately instrumented optimized executable; optimizer-elided allocations are not counted.",
            "phases": "Load includes actual frozen IO, lexing, grammar passes and graph work. Check includes boundary verification. Separately labeled verify-ir repeats that verification on checked IR; it is duplicate work, not a disjoint compilation stage. Never sum phases as compilation totals. LLVM emission, reference interpretation, formatting and explicit drops use the real public compiler APIs.",
            "lifetime": "No live-counter resets. Phase baselines include previously retained objects and unmeasured report bookkeeping. Nonzero final baseline is not a leak claim.",
            "peak": "Requested live Rust block sizes at completed-call boundaries, including entry baseline; not allocator capacity/metadata, internal realloc transient peak, stack or physical memory.",
            "process": "A new process for every warmup and sample. Raw wait4 CPU/RSS/wall are retained for diagnostics only; instrumentation timings are never used for speed comparisons.",
            "rss": "Shared harness wait4 RSS has the inherited Python pre-exec floor; RSS is not allocation count or simultaneous process-tree usage.",
            "validation": "Exact ordinary-compiler LLVM and formatter bytes; independent arithmetic/module expected stdout and i64 result; full counter/phase/lifetime equations and frozen file hashes each invocation.",
            "rejection": "The identical complete ~900 KiB valid source must reject formatted output above its budget with E_FORMAT_LIMIT; this is an intentional exit1 measurement, not accepted-program throughput.",
            "limits": "No allocation-site attribution, compiled native-program allocations, device allocations, startup-before-Rust accounting, cold-cache or thermal-control claim. Counts alone establish no speedup or allocation bottleneck.",
        },
        "preparation": [], "samples": [], "summaries": {},
    }

    def ordinary(name, arguments, expected_exit=0, expected_stdout=None, artifact=None, contains=None):
        unchanged(frozen)
        prefix = preparation_dir / name
        process = shared.run_child([compiler, *arguments], directory, prefix.with_suffix(".stdout"),
                                   prefix.with_suffix(".stderr"), timeout_seconds, environment)
        process.update({"name": name, "valid": False})
        report["preparation"].append(process)
        shared.require_result(process, expected_exit,
                              hashlib.sha256(expected_stdout).hexdigest() if expected_stdout is not None else None,
                              stderr_empty=expected_exit == 0, stderr_contains=contains)
        for stream in ("stdout", "stderr"):
            path = Path(process[stream]["path"])
            observed = fingerprint(path)
            demand(observed == {key: process[stream][key] for key in ("bytes", "sha256")},
                   "ordinary-compiler process output changed before validation")
            frozen[path] = observed
        if artifact is not None:
            frozen[artifact] = fingerprint(artifact)
            process["artifact"] = {"path": str(artifact), **frozen[artifact]}
        unchanged(frozen)
        process["valid"] = True
        save_report(directory, report)
        return process

    try:
        save_report(directory, report)
        version = ordinary("compiler-version", ["--version"])
        identity = Path(version["stdout"]["path"]).read_text()
        demand(identity.startswith("axeon ") and len(identity.splitlines()) == 1, "ordinary compiler identity is invalid")
        report["compiler"]["version_output"] = identity
        arithmetic = inputs / "arithmetic.ax"
        module = inputs / "modules/main.ax"
        expected_module = b"f64:0x8000000000000000\n42\nresult: 42\n"
        references = {}
        for name, source, expected in (("arithmetic-project", arithmetic, expected_arithmetic),
                                        ("aggregate-modules-project", module, expected_module)):
            ordinary(name + "-check", ["check", source], expected_stdout=b"")
            ordinary(name + "-interpret", ["interpret", source], expected_stdout=expected)
            ir = preparation_dir / (name + ".ll")
            ordinary(name + "-emit-ir", ["emit-ir", source, "-o", ir], expected_stdout=b"", artifact=ir)
            lines = expected.rsplit(b"result: ", 1)
            result = int(lines[1].strip())
            wanted_sources = ({"arithmetic.ax": len(sources["arithmetic.ax"])} if name == "arithmetic-project"
                              else {Path(p).name: len(b) for p, b in MODULE_SOURCES.items()})
            references[name] = {"mode": "project", "outcome": "success", "exit": 0, "codes": [],
                                "result": result, "sources": wanted_sources,
                                "artifacts": {"emitted.ll": {"sha256": frozen[ir]["sha256"]},
                                              "stdout.txt": {"sha256": hashlib.sha256(lines[0]).hexdigest()}}}
        formatted = ordinary("format-success", ["format", arithmetic])
        references["format-success"] = {"mode": "format", "outcome": "success", "exit": 0, "codes": [],
            "result": None, "sources": {"arithmetic.ax": len(sources["arithmetic.ax"])},
            "artifacts": {"formatted.ax": {"sha256": formatted["stdout"]["sha256"]}}}
        budget = inputs / "format-budget.ax"
        ordinary("format-budget-valid-source", ["check", budget], expected_stdout=b"")
        ordinary("format-budget-rejection", ["format", budget], expected_exit=1,
                 expected_stdout=b"", contains=b"error[E_FORMAT_LIMIT]")
        references["format-budget-rejection"] = {"mode": "format", "outcome": "format_error", "exit": 1,
            "codes": ["E_FORMAT_LIMIT"], "result": None, "sources": {"format-budget.ax": len(sources["format-budget.ax"])},
            "artifacts": {"diagnostics.txt": {"contains": "error[E_FORMAT_LIMIT]"}}}
        report["expected_results"] = references
        for round_number in range(trials + 1):
            kind = "warmup" if round_number == 0 else "measured"
            for workload in WORKLOADS:
                unchanged(frozen)
                prefix = samples_dir / f"{workload}-{kind}-{round_number:03}"
                expectation = references[workload]
                source = module if workload == "aggregate-modules-project" else budget if workload == "format-budget-rejection" else arithmetic
                arguments = [profiler, expectation["mode"], source, "--output-dir", prefix]
                if expectation["mode"] == "project":
                    arguments.extend(["--fuel", str(FUEL)])
                process = shared.run_child(arguments, directory, prefix.with_suffix(".stdout"),
                                           prefix.with_suffix(".stderr"), timeout_seconds, environment)
                sample = {"workload": workload, "round_kind": kind, "trial": round_number,
                          "valid": False, "process": process, "directory": str(prefix)}
                report["samples"].append(sample)
                # Persist the raw attempt before validation, including failed samples.
                save_report(directory, report)
                allocation, outputs = validate_result(prefix, process, expectation)
                sample["allocation_report"] = allocation
                sample["outputs"] = {path.name: value for path, value in outputs.items()}
                frozen.update(outputs)
                for stream in ("stdout", "stderr"):
                    path = Path(process[stream]["path"])
                    observed = fingerprint(path)
                    demand(observed == {key: process[stream][key] for key in ("bytes", "sha256")},
                           "profiler process output changed before validation")
                    frozen[path] = observed
                unchanged(frozen)
                sample["valid"] = True
                save_report(directory, report)
        unchanged(frozen)
        report["summaries"] = summarize_samples(report["samples"])
        report["status"] = "passed"
        code = 0
    except (AllocationError, shared.ProfileError, OSError, ValueError, KeyError, TypeError) as error:
        report.update({"status": "failed", "failure": str(error)})
        code = 1
    except KeyboardInterrupt:
        report.update({"status": "interrupted", "failure": "harness interrupted; owned child terminated"})
        code = 130
    report["host_after"] = shared.host_configuration()
    save_report(directory, report)
    print(json.dumps({"status": report["status"], "report": str(directory / "allocations.json"),
                      "failure": report.get("failure")}, indent=2))
    return code


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--profiler", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--trials", default=7, type=int)
    parser.add_argument("--label", default="unspecified")
    parser.add_argument("--timeout-seconds", type=float, default=120)
    args = parser.parse_args(argv)
    if sys.platform != "linux" or not hasattr(os, "wait4"):
        parser.error("Linux os.wait4 is required")
    if not 1 <= args.trials <= 100:
        parser.error("trials must be in 1..100")
    if not math.isfinite(args.timeout_seconds) or args.timeout_seconds <= 0:
        parser.error("timeout must be finite and positive")
    try:
        compiler, profiler = args.compiler.resolve(strict=True), args.profiler.resolve(strict=True)
        for path in (compiler, profiler):
            regular(path, MAX_ARTIFACT_BYTES)
            if not os.access(path, os.X_OK):
                parser.error(f"not an executable file: {path}")
        directory = args.output_dir.absolute()
        directory.mkdir()  # Exclusive; no reuse or cleanup of evidence.
    except (OSError, AllocationError) as error:
        parser.error(str(error))
    return profile(compiler, profiler, directory, args.trials, args.label, args.timeout_seconds)


if __name__ == "__main__":
    raise SystemExit(main())

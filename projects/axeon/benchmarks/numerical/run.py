#!/usr/bin/env python3
"""Two-stage, bounded numerical benchmark. No timing without explicit --execute."""
import argparse
import csv
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import re
import runpy
import statistics
import sys
import time

from kernels import bits

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
_spec = importlib.util.spec_from_file_location("axeon_integer_benchmark", HERE.parent / "run.py")
base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(base)

WORKLOADS = ("scalar", "matrix")
INPUTS = (1000, 4000)
SMALL_INPUTS = (0, 1, 2, 31, 97, 257)
PROFILE_N = 1000
OPTIMIZATIONS = (0, 2, 3)
TRIALS = 3
NUMPY = "NumPy matmul (tolerance)"
ABS_TOL = 2e-12
REL_TOL = 2e-12
FLAGS = ["-target", "x86_64-pc-linux-gnu", "-Werror", "-Wno-override-module",
         "-fno-fast-math", "-ffp-contract=off"]
THREAD_ENV = {name: "1" for name in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS",
                                       "BLIS_NUM_THREADS", "VECLIB_MAXIMUM_THREADS", "NUMEXPR_NUM_THREADS")}


def languages(workload):
    return ("Axeon", "C++", "Python") + ((NUMPY,) if workload == "matrix" else ())


def workload_hashes():
    files = [HERE / name for name in ["scalar.ax", "matrix.ax", "scalar.cpp", "matrix.cpp",
                                      "checked.hpp", "kernels.py"]]
    files += [HERE.parent / "driver.c", ROOT / "runtime/runtime.c", ROOT / "runtime/runtime.h"]
    return {str(path.relative_to(ROOT)): base.file_hash(path) for path in files}


def parse_library_output(stdout, n, reference):
    data = json.loads(stdout)
    # Reuse the common timing/RSS shape checks without imposing an exact-bit
    # contract on a library whose reduction order is intentionally unconstrained.
    protocol = dict(data, checksums=[reference["checksum"]] * 9)
    base.parse_process_output(json.dumps(protocol), n, reference["checksum"])
    checksums, states = data.get("checksums"), data.get("states")
    if not isinstance(checksums, list) or len(checksums) != 9 or any(type(x) is not int for x in checksums):
        raise ValueError("NumPy must report nine signed integer bit checksums")
    if not isinstance(states, list) or len(states) != 9:
        raise ValueError("NumPy must report every final vector")
    expected = reference["state"]
    if len(expected) != 4 or any(not math.isfinite(x) for x in expected):
        raise ValueError("matrix reference must contain four finite components")
    max_abs, max_relative, max_fraction = 0.0, 0.0, 0.0
    tolerances = [ABS_TOL + REL_TOL * abs(x) for x in expected]
    for observed, checksum in zip(states, checksums):
        if not isinstance(observed, list) or len(observed) != 4:
            raise ValueError("NumPy final vector must have four components")
        ordered_sum = 0.0
        for actual, wanted, tolerance in zip(observed, expected, tolerances):
            if type(actual) not in (int, float) or not math.isfinite(actual):
                raise ValueError("NumPy result must remain finite")
            difference = abs(actual - wanted)
            if difference > tolerance:
                raise ValueError(f"NumPy state error {difference} exceeds tolerance {tolerance}")
            max_abs = max(max_abs, difference)
            if wanted != 0.0:
                max_relative = max(max_relative, difference / abs(wanted))
            max_fraction = max(max_fraction, difference / tolerance)
            ordered_sum = ordered_sum + actual
        if bits(ordered_sum) != checksum:
            raise ValueError("NumPy checksum does not describe its reported vector")
    data["numerical_validation"] = {
        "mode": "componentwise tolerance; not exact language-semantic equivalence",
        "absolute_tolerance": ABS_TOL, "relative_tolerance": REL_TOL,
        "component_tolerances": tolerances, "max_abs_error": max_abs,
        "max_relative_error_nonzero_reference": max_relative,
        "max_error_fraction_of_tolerance": max_fraction,
        "all_checksums_happen_to_match_exactly": checksums == [reference["checksum"]] * 9,
        "reference_state": expected,
    }
    return data


def kernel_body(text, symbol, llvm=False):
    if llvm:
        pattern = rf"^define [^\n]*@{re.escape(symbol)}\([^\n]*\)[^\n]*\{{\n.*?^\}}"
    else:
        pattern = rf"^{re.escape(symbol)}:.*?^\s*\.size\s+{re.escape(symbol)}\b"
    match = re.search(pattern, text, re.M | re.S)
    if not match:
        raise ValueError(f"missing kernel body for {symbol}")
    return match.group()


def operator_counts(ir, assembly, symbol):
    llvm_body = kernel_body(ir, symbol, llvm=True)
    asm_body = kernel_body(assembly, symbol)
    forbidden = re.findall(r"\b(?:fadd|fsub|fmul|fdiv)\s+(?:fast|reassoc|nnan|ninf|nsz|arcp|contract|afn)\b", llvm_body)
    if forbidden or re.search(r"@llvm\.(?:fma|fmuladd)\.", llvm_body):
        raise ValueError("strict benchmark IR contains fast-math or contraction operations")
    instructions = re.findall(r"^\s+([a-z][a-z0-9.]*)\s", asm_body, re.M)
    fused = [opcode for opcode in instructions if re.match(r"v(?:fmadd|fmsub|fnmadd|fnmsub)", opcode)]
    if fused:
        raise ValueError(f"native strict kernel contains fused arithmetic: {fused}")
    float_ops = {name: len(re.findall(rf"\b{name}\b", llvm_body))
                 for name in ("fadd", "fsub", "fmul", "fdiv", "sitofp", "alloca", "load", "store", "getelementptr")}
    machine_ops = {}
    for opcode in instructions:
        if re.match(r"v?(?:add|sub|mul|div)(?:sd|ss|pd|ps)$", opcode):
            machine_ops[opcode] = machine_ops.get(opcode, 0) + 1
    return {"llvm_counts": float_ops, "assembly_fp_opcode_counts": machine_ops,
            "assembly_instruction_lines": len(instructions), "fma_instructions": fused,
            "forbidden_fp_flags": forbidden}


def validate_profile(path):
    data = json.loads(path.read_text())
    if data.get("status") != "passed" or data.get("stage") != "profile":
        raise ValueError("a completed profile-stage receipt is required")
    required = {(workload, language, opt) for workload in WORKLOADS
                for language in ("Axeon", "C++") for opt in OPTIMIZATIONS}
    observed = {(row.get("workload"), row.get("language"), row.get("opt"))
                for row in data.get("profiles", [])}
    if observed != required:
        raise ValueError("profile receipt does not cover both kernels and O0/O2/O3")
    if data.get("workload_hashes") != workload_hashes():
        raise ValueError("workload/runtime changed after profiling; reprofile before measuring")
    return data


def compile_stage(runner, compiler, clang, output, opts, result):
    phases = result["compilation_phases"]

    def phase(command, label):
        record = runner.run(command, label)
        phases.append(record)
        return record

    runtime = output / "runtime.o"
    phase([clang, *FLAGS, "-O3", "-std=c11", "-c", ROOT / "runtime/runtime.c", "-o", runtime],
          "shared_runtime_compile")
    phase([sys.executable, "-m", "py_compile", HERE / "kernels.py"], "python_bytecode_compile")
    binaries = {}
    for workload in WORKLOADS:
        directory = output / workload
        directory.mkdir()
        original, adapted = directory / "original.ll", directory / "kernel.ll"
        phase([compiler, "emit-ir", HERE / f"{workload}.ax", "-o", original],
              f"{workload}_axeon_frontend_emit_ir")
        adapted.write_text(base.adapt_llvm(original.read_text()))
        for opt in opts:
            flags = [*FLAGS, f"-O{opt}"]
            for language, slug, symbol in [("Axeon", "axeon", "axeon_bench_kernel"),
                                            ("C++", "cpp", "cpp_bench_kernel")]:
                obj = directory / f"{slug}-O{opt}.o"
                asm = directory / f"{slug}-O{opt}.s"
                optimized_ir = directory / f"{slug}-O{opt}.ll"
                driver = directory / f"driver-{slug}-O{opt}.o"
                executable = directory / f"{slug}-O{opt}"
                source_flags = ["-std=c++17"] if language == "C++" else []
                source = HERE / f"{workload}.cpp" if language == "C++" else adapted
                phase([clang, *flags, *source_flags, "-c", source, "-o", obj],
                      f"{workload}_{slug}_O{opt}_frontend_codegen" if language == "C++"
                      else f"{workload}_{slug}_O{opt}_llvm_codegen")
                phase([clang, *flags, "-std=c11", f"-DBENCH_KERNEL={symbol}", "-c",
                       HERE.parent / "driver.c", "-o", driver], f"{workload}_{slug}_O{opt}_driver_compile")
                phase([clang, *flags, obj, driver, runtime, "-o", executable],
                      f"{workload}_{slug}_O{opt}_link")
                runner.run([clang, *flags, *source_flags, "-S", source, "-o", asm],
                           f"{workload}_{slug}_O{opt}_assembly_audit")
                runner.run([clang, *flags, *source_flags, "-S", "-emit-llvm", source, "-o", optimized_ir],
                           f"{workload}_{slug}_O{opt}_ir_audit")
                evidence = base.assembly_evidence(asm, symbol)
                evidence.update(operator_counts(optimized_ir.read_text(), asm.read_text(), symbol))
                evidence.update(workload=workload, language=language, opt=opt,
                                ir_file=str(optimized_ir), ir_sha256=base.file_hash(optimized_ir))
                result["operator_profiles"].append(evidence)
                binaries[workload, language, opt] = executable
    return binaries


def process_command(workload, language, n, binaries, opt=3):
    if language in ("Axeon", "C++"):
        return [binaries[workload, language, opt], n]
    command = [sys.executable, HERE / "kernels.py", workload, n]
    if language == NUMPY:
        command.append("--numpy")
    return command


def validate_process(record, workload, language, n, reference, **extra):
    if language == NUMPY:
        data = parse_library_output(record["stdout"], n, reference)
    else:
        data = base.parse_process_output(record["stdout"], n, reference["checksum"])
        data["numerical_validation"] = {"mode": "exact signed binary64 checksum bits"}
    total = data["cold_ns"] + data["warmup_ns"] + sum(data["warm_ns"])
    data.update(workload=workload, language=language, peak_rss_kib=record["peak_rss_kib"],
                process_wall_ns=record["wall_ns"], outside_kernel_ns=record["wall_ns"] - total,
                user_cpu_seconds=record["user_cpu_seconds"], system_cpu_seconds=record["system_cpu_seconds"],
                **extra)
    return data


def check_small_inputs(runner, compiler, binaries, references, opts, output, result):
    for workload in WORKLOADS:
        source = (HERE / f"{workload}.ax").read_text()
        if source.count("return kernel(1);") != 1:
            raise ValueError("expected exactly one replaceable interpreter entry point")
        for n in SMALL_INPUTS:
            small = output / workload / f"correctness-{n}.ax"
            small.write_text(source.replace("return kernel(1);", f"return kernel({n});"))
            expected = references[workload][n]
            record = runner.run([compiler, "interpret", small, "--fuel", "1000000"],
                                f"{workload}_interpreter_correctness_{n}")
            if record["stdout"].strip() != f"result: {expected['checksum']}":
                raise ValueError(f"{workload} interpreter mismatch at N={n}: {record['stdout']}")
            for opt in opts:
                for language in ("Axeon", "C++"):
                    label = f"{workload}_{language}_O{opt}_correctness_{n}"
                    record = runner.run(process_command(workload, language, n, binaries, opt), label)
                    validate_process(record, workload, language, n, expected, opt=opt)
            for language in languages(workload)[2:]:
                record = runner.run(process_command(workload, language, n, binaries),
                                    f"{workload}_{language}_correctness_{n}")
                data = validate_process(record, workload, language, n, expected)
                result["small_correctness_checks"].append({"workload": workload, "language": language,
                    "n": n, "validation": data["numerical_validation"]})


def summarize(result):
    for workload in WORKLOADS:
        for n in INPUTS:
            axeon_groups = [p["warm_ns"] for p in result["processes"]
                            if (p["workload"], p["n"], p["language"]) == (workload, n, "Axeon")]
            axeon_median = statistics.median(v for group in axeon_groups for v in group)
            for index, language in enumerate(languages(workload)):
                processes = [p for p in result["processes"]
                             if (p["workload"], p["n"], p["language"]) == (workload, n, language)]
                groups = [p["warm_ns"] for p in processes]
                warm = base.summary([v for group in groups for v in group])
                row = {"workload": workload, "n": n, "language": language, "opt": 3 if language in ("Axeon", "C++") else None,
                       "warm": warm, "warm_ci95_ns": base.cluster_bootstrap(groups, n + index),
                       "cold": base.summary([p["cold_ns"] for p in processes]),
                       "proc_image_peak_rss_kib": base.summary([p["proc_image_peak_rss_kib"] for p in processes]),
                       "process_peak_rss_kib": base.summary([p["peak_rss_kib"] for p in processes]),
                       "outside_kernel_ns": base.summary([p["outside_kernel_ns"] for p in processes]),
                       "time_divided_by_axeon": warm["median"] / axeon_median,
                       "speedup_ci95": [1.0, 1.0] if language == "Axeon" else base.speedup_bootstrap(groups, axeon_groups, n + index),
                       "validation_mode": processes[0]["numerical_validation"]["mode"], "correct": True,
                       "expected_checksum": result["references"][workload][n]["checksum"]}
                if language == NUMPY:
                    row["numerical_validation"] = {
                        "max_abs_error": max(p["numerical_validation"]["max_abs_error"] for p in processes),
                        "max_relative_error_nonzero_reference": max(p["numerical_validation"]["max_relative_error_nonzero_reference"] for p in processes),
                        "max_error_fraction_of_tolerance": max(p["numerical_validation"]["max_error_fraction_of_tolerance"] for p in processes),
                        "absolute_tolerance": ABS_TOL, "relative_tolerance": REL_TOL,
                        "component_tolerances": processes[0]["numerical_validation"]["component_tolerances"],
                    }
                result["summaries"].append(row)


def write_reports(result, output):
    filename = "profiles.json" if result["stage"] == "profile" else "results.json"
    (output / filename).write_text(json.dumps(result, indent=2) + "\n")
    processes = result["profiles"] if result["stage"] == "profile" else result["processes"]
    with (output / "samples.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["workload", "language", "opt", "n", "trial", "phase", "sample", "nanoseconds", "checksum",
                         "validation", "max_state_absolute_error", "proc_image_peak_rss_kib", "wait4_peak_rss_kib",
                         "process_wall_ns", "outside_kernel_ns"])
        for process in processes:
            phases = [("first", 0, process["cold_ns"]), ("warmup", 0, process["warmup_ns"])]
            phases += [("warm", index, value) for index, value in enumerate(process["warm_ns"])]
            validation = process["numerical_validation"]
            for call, (phase, sample, duration) in enumerate(phases):
                writer.writerow([process["workload"], process["language"], process.get("opt"), process["n"],
                    process.get("trial", 0), phase, sample, duration, process["checksums"][call],
                    validation["mode"], validation.get("max_abs_error"), process["proc_image_peak_rss_kib"],
                    process["peak_rss_kib"], process["process_wall_ns"], process["outside_kernel_ns"]])
    with (output / "compilation.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["phase", "wall_ns", "user_cpu_seconds", "system_cpu_seconds", "peak_rss_kib", "command"])
        for record in result["compilation_phases"]:
            writer.writerow([record["label"], record["wall_ns"], record["user_cpu_seconds"], record["system_cpu_seconds"],
                             record["peak_rss_kib"], json.dumps(record["command"])])
    if result["stage"] == "measure":
        with (output / "summary.csv").open("w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(["workload", "language", "n", "opt", "warm_median_ns", "warm_q1_ns", "warm_q3_ns",
                             "warm_min_ns", "warm_max_ns", "warm_ci95_low_ns", "warm_ci95_high_ns", "cold_median_ns",
                             "time_divided_by_axeon", "vmhwm_median_kib", "outside_kernel_median_ns", "validation",
                             "max_state_absolute_error", "absolute_tolerance", "relative_tolerance", "correct"])
            for row in result["summaries"]:
                tolerance = row.get("numerical_validation", {})
                writer.writerow([row["workload"], row["language"], row["n"], row["opt"], row["warm"]["median"],
                    row["warm"]["q1"], row["warm"]["q3"], row["warm"]["min"], row["warm"]["max"],
                    *row["warm_ci95_ns"], row["cold"]["median"], row["time_divided_by_axeon"],
                    row["proc_image_peak_rss_kib"]["median"], row["outside_kernel_ns"]["median"], row["validation_mode"],
                    tolerance.get("max_abs_error"), tolerance.get("absolute_tolerance"), tolerance.get("relative_tolerance"), True])


def execute(args):
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        raise ValueError(f"output directory must be absent or empty: {output}")
    profile = validate_profile(args.profile_results.resolve()) if args.stage == "measure" else None
    output.mkdir(parents=True, exist_ok=True)
    affinity = sorted(os.sched_getaffinity(0))
    cpu = affinity[0] if args.cpu is None else args.cpu
    if cpu not in affinity:
        raise ValueError(f"CPU {cpu} is outside allowed affinity")
    os.environ.update(THREAD_ENV)
    runner = base.Runner(output, cpu)
    compiler, clang = args.compiler.resolve(), args.clang.resolve()
    result = {"schema_version": 1, "stage": args.stage, "status": "running", "inputs": list(INPUTS),
              "profile_n": PROFILE_N, "profile_optimizations": list(OPTIMIZATIONS), "headline_optimization": 3,
              "trials": TRIALS if args.stage == "measure" else 1, "warmups_per_process": 1,
              "warm_samples_per_process": 7, "profiles": [], "processes": [], "summaries": [],
              "compilation_phases": [], "operator_profiles": [], "small_correctness_checks": [],
              "workload_hashes": workload_hashes(), "compiler_sha256": base.file_hash(compiler),
              "host": {"platform": platform.platform(), "python": sys.version, "python_executable": sys.executable,
                       "allowed_cpus": affinity, "pinned_cpu": cpu, "cpuinfo": Path("/proc/cpuinfo").read_text(),
                       "numerical_threading_environment": THREAD_ENV,
                       "limits": {"subprocess_wall_seconds": 30, "virtual_bytes": 2*1024**3,
                                  "overall_wall_seconds": 180, "overall_child_cpu_seconds": 180}},
              "notes": ["Headlines use fixed O3 for both native languages; profiles do not select a favorable winner.",
                        "Strict ordered baselines must match signed binary64 checksum bits exactly.",
                        "NumPy matrix comparison uses componentwise tolerance and is not exact language-semantic equivalence.",
                        "NumPy is a numerical-library baseline; Axeon has no numerical-library or tensor API.",
                        "NumPy performs tiny 4x4 matmul/multiply/add/copy calls per iteration; includes Python dispatch and per-kernel allocation/setup, not large-BLAS throughput.",
                        "First call is in a fresh process, without flushed hardware/filesystem caches.",
                        "VmHWM is post-exec process peak memory, including driver/runtime/imports; not incremental kernel memory.",
                        "wait4 RSS includes inherited pre-exec process history and is retained separately.",
                        "Outside-kernel time includes launch/imports/reporting/scheduling and is not pure startup.",
                        "Three process clusters provide limited uncertainty estimates; no outliers are removed.",
                        "Benchmark-only LLVM symbol export is not a supported language FFI."]}
    if profile:
        result["profile_provenance"] = {"path": str(args.profile_results.resolve()),
            "sha256": base.file_hash(args.profile_results), "compiler_sha256": profile["compiler_sha256"],
            "note": "A reviewed CLI default change may produce a different compiler binary; workload/runtime hashes must match."}
    try:
        result["versions"] = {"axeon": runner.run([compiler, "--version"], "axeon_version")["stdout"],
                              "clang": runner.run([clang, "--version"], "clang_version")["stdout"]}
        if "19.1.7" not in result["versions"]["clang"].split():
            raise ValueError("Clang 19.1.7 is required")
        numpy_probe = """import json
import numpy as np
try:
    from threadpoolctl import threadpool_info
except ImportError:
    pools = None
else:
    pools = threadpool_info()
    if any(pool.get('num_threads') != 1 for pool in pools):
        raise RuntimeError('numerical library thread count is not one')
print(json.dumps({'version': np.__version__, 'runtime_threadpools': pools,
                  'threadpoolctl_available': pools is not None,
                  'runtime_threadpool_verification_available': bool(pools)}))
np.show_config()
"""
        numpy = runner.run([sys.executable, "-c", numpy_probe], "numpy_configuration")
        result["numpy_configuration"] = numpy["stdout"]
        result["numpy_runtime"] = json.loads(numpy["stdout"].splitlines()[0])
        references = {}
        for workload in WORKLOADS:
            references[workload] = {}
            for n in sorted(set(SMALL_INPUTS + INPUTS + (PROFILE_N,))):
                record = runner.run([sys.executable, HERE / "kernels.py", workload, n, "--reference"],
                                    f"{workload}_independent_reference_{n}")
                references[workload][n] = json.loads(record["stdout"])
        result["references"] = references
        opts = OPTIMIZATIONS if args.stage == "profile" else (3,)
        binaries = compile_stage(runner, compiler, clang, output, opts, result)
        check_small_inputs(runner, compiler, binaries, references, opts, output, result)
        if args.stage == "profile":
            for workload in WORKLOADS:
                for opt in OPTIMIZATIONS:
                    for language in ("Axeon", "C++"):
                        record = runner.run(process_command(workload, language, PROFILE_N, binaries, opt),
                                            f"profile_{workload}_{language}_O{opt}")
                        data = validate_process(record, workload, language, PROFILE_N,
                                                references[workload][PROFILE_N], opt=opt)
                        data["warm"] = base.summary(data["warm_ns"])
                        result["profiles"].append(data)
        else:
            for trial in range(TRIALS):
                for workload in WORKLOADS if trial % 2 == 0 else reversed(WORKLOADS):
                    for n in INPUTS if trial % 2 == 0 else reversed(INPUTS):
                        candidates = languages(workload)
                        offset = (trial + INPUTS.index(n)) % len(candidates)
                        for language in candidates[offset:] + candidates[:offset]:
                            record = runner.run(process_command(workload, language, n, binaries),
                                                f"measure_{workload}_{language}_{n}_{trial}")
                            data = validate_process(record, workload, language, n, references[workload][n],
                                                    trial=trial, opt=3 if language in ("Axeon", "C++") else None)
                            result["processes"].append(data)
                            (output / "partial.json").write_text(json.dumps(result, indent=2) + "\n")
            summarize(result)
        result["source_hashes"] = {str(path.relative_to(ROOT)): base.file_hash(path) for path in
            [HERE / "run.py", HERE / "plot_results.py", HERE.parent / "run.py", *HERE.glob("*.ax"),
             *HERE.glob("*.cpp"), HERE / "checked.hpp", HERE / "kernels.py", HERE.parent / "driver.c"]}
        result.update(status="passed", commands=runner.records, total_child_cpu_seconds=runner.cpu_seconds,
                      total_wall_seconds=180 - (runner.deadline - time.monotonic()))
        write_reports(result, output)
        if args.stage == "measure":
            runner.run([sys.executable, HERE / "plot_results.py", output / "results.json", output / "numerical.png"], "plot_results")
            result.update(commands=runner.records, total_child_cpu_seconds=runner.cpu_seconds,
                          total_wall_seconds=180 - (runner.deadline - time.monotonic()))
            write_reports(result, output)
        print(json.dumps({"status": "passed", "stage": args.stage, "output": str(output),
                          "profiles": result["profiles"] if args.stage == "profile" else [],
                          "summaries": result["summaries"]}, indent=2))
    except Exception as error:
        result.update(status="failed", error=str(error), commands=runner.records)
        (output / "failure.json").write_text(json.dumps(result, indent=2) + "\n")
        completed = output / ("profiles.json" if args.stage == "profile" else "results.json")
        if completed.exists():
            completed.write_text(json.dumps(result, indent=2) + "\n")
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=["profile", "measure"])
    parser.add_argument("--execute", action="store_true", help="requires completed correctness gate and explicit execution approval")
    parser.add_argument("--compiler", type=Path, default=ROOT / "target/release/axeon")
    parser.add_argument("--clang", type=Path, default=Path("/workspace/tools/sysroot/usr/lib/llvm-19/bin/clang"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--profile-results", type=Path)
    args = parser.parse_args()
    if not args.execute:
        parser.error("no timing without --execute; complete correctness gate and obtain execution approval first")
    if args.stage == "measure" and args.profile_results is None:
        parser.error("measure requires --profile-results from the approved profile stage")
    if args.output is None:
        try:
            layout = runpy.run_path(str(ROOT / "scripts/project_layout.py"))
            args.output = layout["reserve_evidence_output"]("numerical-benchmark-" + args.stage)
        except (OSError, ValueError) as error:
            parser.error(str(error).replace("--output-dir", "--output"))
    execute(args)


if __name__ == "__main__":
    main()

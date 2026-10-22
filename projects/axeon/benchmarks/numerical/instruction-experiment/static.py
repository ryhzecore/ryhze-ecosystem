#!/usr/bin/env python3
"""Same-source SSE2/AVX2 compilation and exact-bit checks; performs no timings."""
import collections
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import resource
import subprocess
import sys

HERE = Path(__file__).resolve().parent
NUMERICAL = HERE.parent
ROOT = NUMERICAL.parent.parent
CLANG = Path("/workspace/tools/sysroot/usr/lib/llvm-19/bin/clang")
RUSTC = ROOT / ".tools/rust/bin/rustc"
FLAGS = ["-target", "x86_64-pc-linux-gnu", "-Werror", "-Wno-override-module",
         "-fno-fast-math", "-ffp-contract=off"]
INPUTS = [0, 1, 2, 31, 97, 257, 1000, 4000]
ENV = dict(os.environ, OMP_NUM_THREADS="1", OPENBLAS_NUM_THREADS="1", MKL_NUM_THREADS="1")
RECEIPT = {"status": "running", "mode": "static and correctness only; no timings",
           "commands": [], "profiles": [], "checks": [], "inputs": INPUTS,
           "flags": FLAGS, "optimization_levels": [2, 3],
           "resource_limits": {"process_virtual_memory_bytes": 2 * 1024**3,
                               "process_cpu_seconds": 30, "command_timeout_seconds": 35}}


def limited():
    resource.setrlimit(resource.RLIMIT_AS, (2 * 1024**3, 2 * 1024**3))
    resource.setrlimit(resource.RLIMIT_CPU, (30, 30))


def run(command):
    command = [str(x) for x in command]
    result = subprocess.run(command, cwd=ROOT, env=ENV, capture_output=True, text=True,
                            timeout=35, preexec_fn=limited)
    RECEIPT["commands"].append({"argv": command, "returncode": result.returncode,
                               "stdout": result.stdout, "stderr": result.stderr})
    if result.returncode:
        raise RuntimeError(f"command failed: {command}: {result.stderr}")
    return result.stdout


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def hashes():
    files = [NUMERICAL / name for name in ("matrix.ax", "scalar.ax", "matrix.cpp", "scalar.cpp",
                                          "checked.hpp", "kernels.py")]
    files += [ROOT / "runtime/runtime.c", ROOT / "runtime/runtime.h"]
    for workload in ("matrix", "scalar"):
        files += [NUMERICAL / "profiles" / workload / name for name in ("kernel.ll", "original.ll")]
    return {str(path.relative_to(ROOT)): sha(path) for path in files}


def body(text, symbol, llvm=False):
    expression = (rf"^define [^\n]*@{symbol}\([^\n]*\)[^\n]*\{{\n.*?^\}}" if llvm else
                  rf"^{symbol}:.*?^\s*\.size\s+{symbol}\b")
    result = re.search(expression, text, re.M | re.S)
    if not result:
        raise ValueError(f"missing symbol {symbol}")
    return result.group()


def instruction_lines(assembly):
    return [line.split("#", 1)[0].strip() for line in assembly.splitlines()
            if re.match(r"^\s+[a-z][a-z0-9.]*(?:\s|$)", line)]


def hot_loop(assembly):
    """Extract the single innermost recurrence used by these frozen kernels."""
    after_header = assembly.split("=>This Inner Loop Header: Depth=1", 1)
    if len(after_header) != 2:
        raise ValueError("expected one innermost loop header")
    lines = []
    for line in instruction_lines(after_header[1]):
        lines.append(line)
        if re.match(r"j[a-z]+\s", line):
            return lines
    raise ValueError("expected a terminating conditional loop branch")


def profile(ir, assembly, symbol):
    llvm = body(ir, symbol, True)
    asm = body(assembly, symbol)
    forbidden = re.findall(r"\b(?:fadd|fsub|fmul|fdiv)\s+(?:fast|reassoc|nnan|ninf|nsz|arcp|contract|afn)\b", llvm)
    instructions = instruction_lines(asm)
    fused = [line for line in instructions if re.match(r"v(?:fmadd|fmsub|fnmadd|fnmsub)", line)]
    if forbidden or fused or re.search(r"@llvm\.(?:fma|fmuladd)\.", llvm):
        raise ValueError("unexpected unsafe floating-point contraction or flags")
    counts = collections.Counter(line.split()[0] for line in instructions)
    loop = hot_loop(asm)
    loop_counts = collections.Counter(line.split()[0] for line in loop)
    return {"llvm_counts": {name: len(re.findall(rf"\b{name}\b", llvm))
                            for name in ("fadd", "fsub", "fmul", "fdiv", "alloca", "load", "store", "getelementptr")},
            "assembly_instruction_lines": len(instructions), "assembly_opcode_counts": dict(counts),
            "forbidden_fp_flags": forbidden, "fma_instructions": fused,
            "ymm_instruction_lines": [line for line in instructions if "%ymm" in line],
            "hot_loop": {"instructions": loop, "instruction_lines": len(loop),
                         "opcode_counts": dict(loop_counts),
                         "memory_operand_lines": [line for line in loop if "(" in line]}}


def main():
    output = HERE / "artifacts"
    output.mkdir(exist_ok=True)
    RECEIPT["source_hashes_before"] = hashes()
    RECEIPT["clang_version"] = run([CLANG, "--version"])
    RECEIPT["rustc_version"] = run([RUSTC, "--version"])
    probe = output / "capability"
    run([RUSTC, "--edition=2021", "-C", "target-cpu=x86-64", HERE / "capability.rs", "-o", probe])
    # This executable is compiled for baseline x86-64, without AVX/AVX2 target flags.
    RECEIPT["capability_probe"] = json.loads(run([probe]))
    if not RECEIPT["capability_probe"]["avx2"]:
        raise ValueError("AVX2 cannot execute on this CPU/OS")
    spec = importlib.util.spec_from_file_location("numerical_reference", NUMERICAL / "kernels.py")
    reference = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reference)
    runtime = output / "runtime.o"
    run([CLANG, *FLAGS, "-O3", "-std=c11", "-c", ROOT / "runtime/runtime.c", "-o", runtime])
    for workload in ("scalar", "matrix"):
        for language, symbol in (("axeon", "axeon_bench_kernel"), ("cpp", "cpp_bench_kernel")):
            source = (NUMERICAL / "profiles" / workload / "kernel.ll" if language == "axeon" else
                      NUMERICAL / f"{workload}.cpp")
            language_flags = ["-std=c++17"] if language == "cpp" else []
            driver = output / f"driver-{language}.o"
            run([CLANG, *FLAGS, "-O2", "-std=c11", f"-DBENCH_KERNEL={symbol}", "-c",
                 HERE / "check-driver.c", "-o", driver])
            for opt in (2, 3):
                for cpu in ("baseline", "avx2"):
                    extra = ["-mavx2"] if cpu == "avx2" else []
                    flags = [*FLAGS, f"-O{opt}", *extra]
                    stem = output / f"{workload}-{language}-O{opt}-{cpu}"
                    obj, asm, ir = [stem.with_suffix(suffix) for suffix in (".o", ".s", ".ll")]
                    run([CLANG, *flags, *language_flags, "-c", source, "-o", obj])
                    run([CLANG, *flags, *language_flags, "-S", source, "-o", asm])
                    run([CLANG, *flags, *language_flags, "-S", "-emit-llvm", source, "-o", ir])
                    run([CLANG, *flags, obj, driver, runtime, "-o", stem])
                    record = profile(ir.read_text(), asm.read_text(), symbol)
                    record.update(workload=workload, language=language, opt=opt, cpu=cpu,
                                  source=str(source.relative_to(ROOT)), source_sha256=sha(source),
                                  assembly=str(asm.relative_to(ROOT)), assembly_sha256=sha(asm),
                                  ir=str(ir.relative_to(ROOT)), ir_sha256=sha(ir), binary_sha256=sha(stem))
                    stem.with_suffix(".loop.txt").write_text("\n".join(record["hot_loop"]["instructions"]) + "\n")
                    RECEIPT["profiles"].append(record)
                    for n in INPUTS:
                        expected = reference.independent_reference(workload, n)["checksum"]
                        observed = int(run([stem, n]).strip())
                        if observed != expected:
                            raise ValueError(f"bit mismatch: {workload} {language} {opt} {cpu} {n}")
                        RECEIPT["checks"].append({"workload": workload, "language": language,
                                                  "opt": opt, "cpu": cpu, "n": n,
                                                  "expected": expected, "observed": observed})
    RECEIPT["cross_language_comparison"] = {}
    for workload in ("scalar", "matrix"):
        for opt in (2, 3):
            for cpu in ("baseline", "avx2"):
                rows = [row for row in RECEIPT["profiles"]
                        if (row["workload"], row["opt"], row["cpu"]) == (workload, opt, cpu)]
                instructions = rows[0]["hot_loop"]["instructions"]
                RECEIPT["cross_language_comparison"][f"{workload}-O{opt}-{cpu}"] = {
                    "axeon_cpp_hot_loop_identical": instructions == rows[1]["hot_loop"]["instructions"],
                    "hot_loop_instructions": len(instructions),
                    "hot_loop_sha256": hashlib.sha256(("\n".join(instructions) + "\n").encode()).hexdigest()}
    RECEIPT["frozen_baseline_assembly_matches"] = {
        str(Path(row["assembly"]).relative_to(HERE.relative_to(ROOT))):
        sha(ROOT / row["assembly"]) == sha(NUMERICAL / "profiles" / row["workload"] /
                                          f"{row['language']}-O{row['opt']}.s")
        for row in RECEIPT["profiles"] if row["cpu"] == "baseline"}
    RECEIPT["source_hashes_after"] = hashes()
    if RECEIPT["source_hashes_before"] != RECEIPT["source_hashes_after"]:
        raise ValueError("frozen inputs changed during experiment")
    RECEIPT["status"] = "passed"


if __name__ == "__main__":
    try:
        main()
    finally:
        (HERE / "static.json").write_text(json.dumps(RECEIPT, indent=2) + "\n")
    print(json.dumps({"status": RECEIPT["status"], "profiles": len(RECEIPT["profiles"]),
                      "exact_checks": len(RECEIPT["checks"]), "capability": RECEIPT["capability_probe"]}))

#!/usr/bin/env python3
"""Check actual Axeon tensor programs against the preserved independent oracles."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
ORACLES = ROOT / "numerics-tests"
sys.path.insert(0, str(ORACLES))
import generate_fixtures  # noqa: E402
import run as numerical_gate  # noqa: E402

ERRORS = {name: code for code, name in enumerate([
    "RankLimit", "ExtentLimit", "ElementLimit", "ShapeMismatch", "RankMismatch",
    "Bounds", "InvalidPermutation", "InvalidStep", "BudgetLimit", "AllocationFailure",
    "WorkLimit", "NonFiniteInput", "EmptyInput", "NumericalFailure", "NestedScope",
], 1)}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fingerprints():
    paths = [*ROOT.joinpath("src").rglob("*.rs"), *ROOT.joinpath("runtime").glob("*"),
             *ROOT.joinpath("crates").rglob("*.rs"), *ROOT.joinpath("crates").rglob("Cargo.toml"),
             ROOT / "Cargo.toml", ROOT / "Cargo.lock", ROOT / "rust-toolchain.toml",
             ROOT / "toolchain-lock.json", Path(__file__), ORACLES / "fixtures.json",
             *ORACLES.glob("*.py")]
    return {str(p.relative_to(ROOT)): sha(p) for p in sorted(set(paths)) if p.is_file()}


def encoded(bits):
    signed = bits if bits < 1 << 63 else bits - (1 << 64)
    return f"f64_from_bits({signed})"


def shape_literal(shape):
    return "[" + ", ".join(map(str, shape)) + "]"


def generate(fixture):
    """Keep exact original storage bits and a single actual source view transform."""
    helpers, body, operands = [], [], []
    failure_call = None
    for name in ("a", "b"):
        if name not in fixture:
            continue
        tensor = fixture[name]
        values, shape = tensor["bits"], tensor["shape"]
        dims = shape_literal(shape)
        if 0 < len(values) <= 32:
            body += [f"let data_{name}: [f64; {len(values)}] = [" + ", ".join(map(encoded, values)) + "];",
                     f"let mut {name}: tensor = try tensor_from_array({dims}, data_{name});"]
        else:
            body.append(f"let mut {name}: tensor = try tensor_zeros({dims});")
            if any(values):
                # Each scalar helper remains comfortably below the 2,048-node
                # function limit. This constructs inputs before the operation;
                # it does not replace or simplify the tensor kernel being tested.
                dispatch = []
                for start in range(0, len(values), 64):
                    chunk = values[start:start + 64]
                    fn = f"input_{name}_{start}"
                    cases = "\n".join(f"if index == {start + i} {{ return {encoded(value)}; }}" for i, value in enumerate(chunk))
                    helpers.append(f"fn {fn}(index: i64) -> f64 {{\n{cases}\nreturn 0.0;\n}}")
                    dispatch.append(f"if index < {start + len(chunk)} {{ return {fn}(index); }}")
                helpers.append(f"fn input_{name}(index: i64) -> f64 {{\n" + "\n".join(dispatch) + "\nreturn 0.0;\n}")
                body += [f"let mut init_{name}: i64 = 0;",
                         f"while init_{name} < {len(values)} {{ try tensor_set({name}, init_{name}, input_{name}(init_{name})); init_{name} = init_{name} + 1; }}"]
        operand = name
        if tensor["transforms"]:
            transform = tensor["transforms"][0]
            if transform[0] == "p":
                call = f"tensor_transpose(&{name}, {shape_literal(transform[1:])})"
            else:
                call = f"tensor_slice(&{name}, " + ", ".join(map(str, transform[1:])) + ")"
            operand = f"view_{name}"
            body.append(f"let {operand}: &tensor = try {call};")
            if fixture["id"].startswith("bad_"):
                failure_call = call
        operands.append(operand)
    operation = f"tensor_{fixture['op']}(" + ", ".join(operands) + ")"
    scalar = fixture["op"] in ("sum", "logsumexp")
    if scalar:
        body += [f"let answer: f64 = try {operation};", "print(0); print(0); print(1); print(answer);"]
    else:
        body += [f"let answer: tensor = try {operation};", "print(0); print(tensor_rank(answer));",
                 "let mut axis: i64 = 0; while axis < tensor_rank(answer) { print(try tensor_dim(answer, axis)); axis = axis + 1; }",
                 "print(tensor_len(answer));",
                 "let mut index: i64 = 0; while index < tensor_len(answer) { print(try tensor_get(answer, index)); index = index + 1; }"]
    source = "\n\n".join(helpers) + "\nfn main() -> i64 {\n  tensor_scope(16777216) {\n    " + "\n    ".join(body) + "\n  } catch (error: tensor_error) {\n    print(1); print(tensor_error_code(error)); print(tensor_error_start(error)); print(tensor_error_end(error));\n  }\n  return 0;\n}\n"
    call = failure_call or operation
    offset = source.index(call)
    return source, [len(source[:offset].encode()), len(source[:offset + len(call)].encode())]


def decode(source, output, expected_span):
    lines = output.splitlines()
    if not lines or lines[-1] != "result: 0":
        raise AssertionError("missing exact source-program completion")
    if lines[0] == "1":
        if len(lines) != 5:
            raise AssertionError("malformed or partially printed error response")
        code, start, end = map(int, lines[1:4])
        if [start, end] != expected_span or not 0 <= start < end <= len(source.encode()):
            raise AssertionError(f"wrong tensor error span: {[start, end]} != {expected_span}")
        names = {value: key for key, value in ERRORS.items()}
        return {"kind": "error", "error": names[code]}
    if lines[0] != "0":
        raise AssertionError("invalid source output tag")
    rank = int(lines[1])
    if not 0 <= rank <= 8:
        raise AssertionError("invalid output rank")
    shape = list(map(int, lines[2:2 + rank]))
    count = int(lines[2 + rank])
    words = lines[3 + rank:-1]
    if len(words) != count or any(not re.fullmatch(r"f64:0x[0-9a-f]{16}", value) for value in words):
        raise AssertionError("incomplete/malformed full tensor output")
    return {"kind": "values", "shape": shape, "bits": [word[6:] for word in words]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, default=ROOT / ("target/release/axeon.exe" if os.name == "nt" else "target/release/axeon"))
    args = parser.parse_args()
    output = args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        raise SystemExit("Choose a fresh output directory; previous evidence is preserved.")
    output.mkdir(parents=True, exist_ok=True)
    compiler = args.compiler.resolve()
    supplied_clang = "/workspace/tools/sysroot/usr/lib/llvm-19/bin/clang"
    selected_clang = os.environ.get("AXEON_CLANG") or (supplied_clang if Path(supplied_clang).is_file() else "clang-19")
    clang = Path(shutil.which(selected_clang) or selected_clang).resolve()
    runtime = compiler.parent / ("axeon_tensor_runtime.lib" if os.name == "nt" else "libaxeon_tensor_runtime.a")
    if os.environ.get("AXEON_TENSOR_RUNTIME"):
        runtime = Path(os.environ["AXEON_TENSOR_RUNTIME"]).resolve()
    receipt = {"schema_version": 1, "status": "running", "commands": [], "runs": [],
               "ir_audits": [], "excluded_fixtures": [],
               "scope": "Actual Axeon source; reference interpreter and linked native O0/O2/O3, each checked against preserved independent full-vector oracles; no timings/benchmark claim.",
               "host": {"platform": platform.platform(), "python": sys.version},
               "limits": {"child_memory_bytes": 2 << 30, "child_seconds": 30,
                          "aggregate_wall_seconds": 180, "aggregate_child_cpu_seconds": 180,
                          "interpreter_fuel": 20_000_000, "maximum_output_elements": 4096},
               "compiler_sha256": None, "runtime_archive_sha256": None}
    receipt_path = output / "source-tensor-oracle.json"
    runner = None
    try:
        receipt["compiler_sha256"] = sha(compiler)
        receipt["runtime_archive_sha256"] = sha(runtime)
        if os.name == "nt":
            receipt["compiler_supervisor_sha256"] = sha(compiler.parent / "axeon-process.exe")
        runner = numerical_gate.Runner(output, receipt)
        receipt["source_hashes"] = fingerprints()
        receipt["compiler_version"] = runner.call("compiler-version", [compiler, "--version"])
        receipt["clang_sha256"] = sha(clang)
        receipt["clang_version"] = runner.call("clang-version", [clang, "--version"])
        if "axeon 0.13.0; subset 13;" not in receipt["compiler_version"]:
            raise AssertionError("source tensor gate requires compiler0.13.0/subset13")
        frozen = json.loads((ORACLES / "fixtures.json").read_text())
        if frozen["fixtures"] != generate_fixtures.fixtures():
            raise AssertionError("independent fixture regeneration differs")
        receipt["accuracy_budget_version"] = frozen["accuracy_budget_version"]
        runner.call("oracle-unit-tests", [sys.executable, "-m", "unittest", "discover", "-s", ORACLES, "-p", "test_*.py", "-v"])
        for fixture in frozen["fixtures"]:
            reason = None
            if any(len(fixture[key]["transforms"]) > 1 for key in ("a", "b") if key in fixture):
                reason = "Source subset permits one direct-owner view transform; composed view is not exposed."
            expected = fixture["expected"]
            if len(expected.get("bits", expected.get("reference_bits", []))) > 4096:
                reason = "Full output exceeds this source-gate bound; the complete 65,536-element library result remains covered by preserved0.4 evidence."
            if reason:
                receipt["excluded_fixtures"].append({"id": fixture["id"], "reason": reason})
                continue
            source, error_span = generate(fixture)
            source_path = output / (fixture["id"] + ".ax")
            source_path.write_bytes(source.encode("utf-8"))
            ir_path = output / (fixture["id"] + ".ll")
            runner.call(fixture["id"] + "-emit-ir", [compiler, "emit-ir", source_path, "-o", ir_path])
            if (output / (fixture["id"] + "-emit-ir.stderr")).read_bytes():
                raise AssertionError("unexpected IR-emission diagnostics")
            receipt["ir_audits"].append(numerical_gate.audit_ir(ir_path))
            observations = []
            complete_outputs = []
            for mode in ("interpret", "O0", "O2", "O3"):
                command = [compiler, "interpret", source_path, "--fuel", "20000000"] if mode == "interpret" else [compiler, "run", source_path, "--opt", mode[1:]]
                text = runner.call(fixture["id"] + "-" + mode, command)
                if (output / (fixture["id"] + "-" + mode + ".stderr")).read_bytes():
                    raise AssertionError("unexpected source-program stderr")
                actual = decode(source, text, error_span)
                result = numerical_gate.compare(fixture, actual)
                result.update({"execution": mode, "source": source_path.name, "source_sha256": sha(source_path)})
                receipt["runs"].append(result)
                observations.append(actual)
                complete_outputs.append((output / (fixture["id"] + "-" + mode + ".stdout")).read_bytes())
            if any(actual != observations[0] for actual in observations[1:]):
                raise AssertionError(f"{fixture['id']}: native/reference exact output parity changed")
            if any(value != complete_outputs[0] for value in complete_outputs[1:]):
                raise AssertionError(f"{fixture['id']}: complete stdout parity changed")
        if fingerprints() != receipt["source_hashes"] or sha(compiler) != receipt["compiler_sha256"] or sha(runtime) != receipt["runtime_archive_sha256"]:
            raise AssertionError("source/compiler/runtime changed during the recorded gate")
        receipt["fixture_count"] = len(receipt["runs"]) // 4
        receipt["execution_count"] = len(receipt["runs"])
        receipt["element_observations"] = sum(item.get("elements", 0) for item in receipt["runs"])
        runner.check_budget()
        receipt["status"] = "passed"
    except Exception as error:
        held = os.name == "nt" and runner is not None and isinstance(error, runner.api["ResourceHold"])
        receipt["status"] = "resource_hold" if held else "failed"
        receipt["failure"] = str(error)
        if held:
            raise SystemExit(75) from error
        raise
    finally:
        receipt["wall_seconds"] = time.monotonic() - runner.started if runner else None
        receipt["child_cpu_seconds"] = runner.child_cpu() - runner.cpu_started if runner else None
        receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({key: receipt[key] for key in ("status", "fixture_count", "execution_count", "element_observations", "wall_seconds", "child_cpu_seconds")}, indent=2))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Rebuild Axeon and record independently checked example outcomes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import runpy
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
EXPECTED = {
    "fibonacci": (0, "6765\nresult: 6765\n", ""),
    "effects": (0, "false\n1\n2\nresult: 12\n", ""),
    "control": (0, "125\nresult: 25\n", ""),
    "overflow": (70, "7\n", "trap[overflow] at 3:12\n"),
    "arrays": (0, "2\n13\nresult: 32\n", ""),
    "numerics": (0, "f64:0x4016000000000000\nf64:0x8000000000000000\nf64:0x7ff8000000000000\nresult: 0\n", ""),
    "bounds": (70, "7\n", "trap[bounds] at 9:5\n"),
    "tensors": (0, "f64:0x4014000000000000\nf64:0x4026000000000000\nf64:0x4026000000000000\nf64:0x4039000000000000\nf64:0x404a000000000000\nf64:0x3fe0000000000000\nf64:0x3fe0000000000000\nf64:0x3ff0000000000000\nresult: 0\n", ""),
    "tensors-recovery": (0, "1\n9\n7\n1\n2\nresult: 7\n", ""),
    "tensors-trap": (70, "1\n", "trap[division_by_zero] at 2:12\n"),
    "modules/main": (0, "42\nf64:0x4018000000000000\nresult: 42\n", ""),
    "records": (0, "f64:0x4010000000000000\nresult: 2\n", ""),
    "unions": (0, "f64:0x4002000000000000\nresult: 0\n", ""),
    "aggregate-modules/main": (0, "f64:0x402f000000000000\nresult: 3\n", ""),
    "trait-modules/main": (0, "42\nresult: 0\n", ""),
    "generic-modules/main": (0, "42\n7\n9\nresult: 0\n", ""),
    "generic-nominals/main": (0, "42\n-9223372036854775808\nresult: 0\n", ""),
    "unit-modules/main": (0, "1\n2\n3\n42\nresult: 0\n", ""),
    "nested-modules/main": (0, "f64:0x402f000000000000\nf64:0x8000000000000000\nresult: 3\n", ""),
}
METADATA_EXAMPLES = ["fibonacci", "arrays", "numerics", "tensors", "tensors-recovery", "tensors-trap"]
EXPECTED_REJECTION = {
    "schema_version": 1,
    "diagnostics": [{
        "code": "O007",
        "message": "cannot exclusively access array `values` while it is shared borrowed",
        "path": "examples/ownership-error.ax",
        "start": 183,
        "end": 189,
        "line": 5,
        "column": 27,
        "related": [
            {
                "start": 148,
                "end": 155,
                "message": "this loan lasts until its slice binding's scope ends",
            },
            {"start": 96, "end": 102, "message": "array declared here"},
        ],
    }],
}
EXPECTED_METADATA = {
    "schema_version": 2,
    "compiler": "axeon-0.13.0",
    "language_subset": 13,
    "bootstrap_rust": "1.85.0",
    "llvm_clang": "19.1.7",
    "target": "x86_64-pc-linux-gnu",
    "abi": "axeon-hosted-1",
    "runtime": "hosted-c-4",
    "tensor_runtime": None,
    "precision": "checked-i64-strict-f64-observer-nan-v1",
    "optimization": 2,
    "artifact_kind": "executable",
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def limits():
    resource.setrlimit(resource.RLIMIT_AS, (2 << 30, 2 << 30))
    resource.setrlimit(resource.RLIMIT_CPU, (120, 120))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def run(args, env, timeout=45):
    """Enforce a wall deadline and kill the entire subprocess group on expiry."""
    with subprocess.Popen(
        args,
        cwd=ROOT,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
        preexec_fn=limits,
    ) as process:
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.communicate()
            raise RuntimeError(f"subprocess exceeded {timeout}s: {args}") from error
        # Decode directly, preserving CR/LF bytes instead of newline translation.
        return subprocess.CompletedProcess(args, process.returncode, stdout.decode("utf-8"), stderr.decode("utf-8"))


def checked(result, expected, context):
    actual = (result.returncode, result.stdout, result.stderr)
    require(actual == expected, f"{context}: expected {expected!r}, found {actual!r}")


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path,
                        help="fresh evidence directory; defaults to a dated local Evidence/native-examples attempt")
    options = parser.parse_args()
    if options.output_dir is None:
        try:
            layout = runpy.run_path(str(ROOT / "scripts/project_layout.py"))
            options.output_dir = layout["reserve_evidence_output"]("native-examples")
        except (OSError, ValueError) as error:
            parser.error(str(error))
    destination = options.output_dir.resolve() / "native-examples.json"
    require(not destination.exists(), f"refusing to overwrite {destination}")
    env = os.environ.copy()
    if (ROOT / ".tools/rust/bin").is_dir():
        env["PATH"] = str(ROOT / ".tools/rust/bin") + os.pathsep + env["PATH"]
    env["SOURCE_DATE_EPOCH"] = "0"
    env["CARGO_BUILD_JOBS"] = "1"
    env["AXEON_TENSOR_RUNTIME"] = str(ROOT / "target/release/libaxeon_tensor_runtime.a")
    env.pop("CARGO_TARGET_DIR", None)
    build = run(["cargo", "build", "--offline", "--locked", "--release", "--jobs", "1"], env, 120)
    require(build.returncode == 0, f"compiler build failed:\n{build.stdout}{build.stderr}")
    compiler = ROOT / "target/release/axeon"
    version = run([str(compiler), "--version"], env, 10)
    checked(version, (0, "axeon 0.13.0; subset 13; LLVM/Clang 19.1.7; hosted ABI 1 (single-file), 2 (modules)\n", ""), "compiler version")

    runs = []
    for example, expected in EXPECTED.items():
        source = f"examples/{example}.ax"
        for command, optimization in [("interpret", None), ("run", 0), ("run", 2), ("run", 3)]:
            args = [str(compiler), command, source]
            if optimization is not None:
                args.extend(["--opt", str(optimization)])
            result = run(args, env)
            checked(result, expected, f"{example} {command} O{optimization}")
            runs.append({
                "example": example,
                "command": command,
                "opt": optimization,
                "exit_code": result.returncode,
                "stdout": result.stdout,
                "stderr": result.stderr,
            })

    rejection = run([
        str(compiler), "check", "examples/ownership-error.ax", "--diagnostic-format", "json",
    ], env)
    require(rejection.returncode == 1 and rejection.stdout == "", f"ownership rejection failed: {rejection}")
    diagnostic = json.loads(rejection.stderr)
    require(diagnostic == EXPECTED_REJECTION, f"ownership diagnostic differs: {diagnostic!r}")

    metadata = {}
    with tempfile.TemporaryDirectory(prefix="axeon-native-examples-") as temporary:
        directory = Path(temporary)
        for example in METADATA_EXAMPLES:
            source = ROOT / "examples" / f"{example}.ax"
            executable = directory / example
            ir = directory / f"{example}.ll"
            checked(run([str(compiler), "emit-ir", str(source), "-o", str(ir)], env), (0, "", ""), f"{example} IR emission")
            checked(run([str(compiler), "build", str(source), "-o", str(executable), "--opt", "2"], env), (0, "", ""), f"{example} build")
            checked(run([str(executable)], env, 10), EXPECTED[example], f"{example} standalone artifact")
            emitted = json.loads(executable.with_name(executable.name + ".json").read_text())
            expected = dict(EXPECTED_METADATA)
            if example.startswith("tensors"):
                expected["tensor_runtime"] = {
                    "required_abi": 1, "required_version": "0.1.0",
                    "native_library": "axeon-numerics-0.1.0",
                    "basic_precision": "cpu-f64-canonical-storage-ordered-v1",
                    "hosted_math_precision": "cpu-f64-exp-log1p-corpus-v1",
                    "link_required": False,
                    "archive_sha256": sha256(ROOT / "target/release/libaxeon_tensor_runtime.a"),
                }
            expected.update({
                "source_sha256": sha256(source),
                "ir_sha256": sha256(ir),
                "artifact_sha256": sha256(executable),
            })
            require(emitted == expected, f"{example} metadata differs: {emitted!r}")
            metadata[example] = emitted

    receipt = {
        "schema_version": 2,
        "command": "python3 scripts/record-native-examples.py",
        "source_scope": "Current working-tree sources, rebuilt in release mode before recording; no committed-source claim.",
        "compiler_version": version.stdout.strip(),
        "compiler_sha256": sha256(compiler),
        "expected_results_verified": True,
        "executable_examples": list(EXPECTED),
        "execution_count": len(runs),
        "standalone_metadata_artifacts_verified": METADATA_EXAMPLES,
        "limits": {
            "virtual_memory_mib_per_process": 2048,
            "cpu_seconds_per_process": 120,
            "compiler_build_wall_seconds": 120,
            "example_command_wall_seconds": 45,
            "timeout_kills_subprocess_group": True,
        },
        "runs": runs,
        "intentional_rejections": [{
            "example": "ownership-error",
            "command": "check",
            "diagnostic_format": "json",
            "exit_code": rejection.returncode,
            "stdout": rejection.stdout,
            "stderr": rejection.stderr,
            "diagnostic": diagnostic,
        }],
        "metadata_by_example": metadata,
    }
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"Recorded {len(runs)} exact example outcomes, O007 rejection, and {len(METADATA_EXAMPLES)} verified metadata artifacts in {destination}")


if __name__ == "__main__":
    main()

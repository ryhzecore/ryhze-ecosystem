#!/usr/bin/env python3
"""Rebuild the committed source twice without sharing target directories."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import resource
import runpy
import signal
import subprocess
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-dir", type=Path,
                    help="fresh evidence directory; defaults to a dated local Evidence/reproducibility attempt")
parser.add_argument("--include-numerics", action="store_true")
parser.add_argument("--include-tensors", action="store_true")
parser.add_argument("--include-modules", action="store_true", help="also reproduce the scalar/tensor local module example")
parser.add_argument("--include-records", action="store_true", help="also reproduce the nominal record example")
parser.add_argument("--include-unions", action="store_true", help="also reproduce the owned union result example")
parser.add_argument("--include-aggregate-interfaces", action="store_true", help="also reproduce owned aggregate module interfaces")
parser.add_argument("--include-nested-aggregates", action="store_true", help="also reproduce nested aggregate ownership across modules")
parser.add_argument("--include-unit-values", action="store_true", help="also reproduce unit scalar and aggregate interfaces across modules")
parser.add_argument("--include-static-traits", action="store_true", help="also reproduce concrete static trait module interfaces and dispatch")
parser.add_argument("--include-generic-functions", action="store_true", help="also reproduce explicit bounded generic functions across modules")
parser.add_argument("--include-generic-nominals", action="store_true", help="also reproduce finite generic records and unions across modules")
args = parser.parse_args()
if args.include_modules and not args.include_tensors:
    parser.error("--include-modules requires --include-tensors for the module example's runtime")
if args.output_dir is None:
    try:
        layout = runpy.run_path(str(ROOT / "scripts/project_layout.py"))
        args.output_dir = layout["reserve_evidence_output"]("reproducibility")
    except (OSError, ValueError) as error:
        parser.error(str(error))
EVIDENCE = args.output_dir.resolve()
EVIDENCE.mkdir(parents=True, exist_ok=True)
if (EVIDENCE / "clean-build.json").exists() or (EVIDENCE / "clean-build.log").exists():
    raise SystemExit("Choose a fresh output directory; prior reproduction evidence is preserved.")
TIMEOUT_SECONDS = 120
EXAMPLES = {
    "fibonacci": "6765\nresult: 6765\n",
    # The mutable slice changes samples to [3, 10, 8, 11].
    "arrays": "2\n13\nresult: 32\n",
    "numerics": "f64:0x4016000000000000\nf64:0x8000000000000000\nf64:0x7ff8000000000000\nresult: 0\n",
}
if args.include_tensors:
    EXAMPLES.update({
        "tensors": "f64:0x4014000000000000\nf64:0x4026000000000000\nf64:0x4026000000000000\nf64:0x4039000000000000\nf64:0x404a000000000000\nf64:0x3fe0000000000000\nf64:0x3fe0000000000000\nf64:0x3ff0000000000000\nresult: 0\n",
        "tensors-recovery": "1\n9\n7\n1\n2\nresult: 7\n",
    })
if args.include_modules:
    EXAMPLES["modules"] = "42\nf64:0x4018000000000000\nresult: 42\n"
if args.include_records:
    EXAMPLES["records"] = "f64:0x4010000000000000\nresult: 2\n"
if args.include_unions:
    EXAMPLES["unions"] = "f64:0x4002000000000000\nresult: 0\n"
if args.include_aggregate_interfaces:
    EXAMPLES["aggregate-modules"] = "f64:0x402f000000000000\nresult: 3\n"
if args.include_nested_aggregates:
    EXAMPLES["nested-modules"] = "f64:0x402f000000000000\nf64:0x8000000000000000\nresult: 3\n"

if args.include_unit_values:
    EXAMPLES["unit-modules"] = "1\n2\n3\n42\nresult: 0\n"

if args.include_static_traits:
    EXAMPLES["trait-modules"] = "42\nresult: 0\n"

if args.include_generic_functions:
    EXAMPLES["generic-modules"] = "42\n7\n9\nresult: 0\n"

if args.include_generic_nominals:
    EXAMPLES["generic-nominals"] = "42\n-9223372036854775808\nresult: 0\n"


def example_source(name):
    return f"examples/{name}/main.ax" if name in ("modules", "aggregate-modules", "nested-modules", "unit-modules", "trait-modules", "generic-modules", "generic-nominals") else f"examples/{name}.ax"


def limits():
    resource.setrlimit(resource.RLIMIT_AS, (2 << 30, 2 << 30))
    resource.setrlimit(resource.RLIMIT_CPU, (TIMEOUT_SECONDS, TIMEOUT_SECONDS))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def bounded(args, cwd, env=None):
    with subprocess.Popen(
        args, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        start_new_session=True, preexec_fn=limits,
    ) as process:
        try:
            stdout, stderr = process.communicate(timeout=TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired as error:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.communicate()
            raise RuntimeError(f"subprocess exceeded {TIMEOUT_SECONDS}s: {args}") from error
        if process.returncode:
            raise RuntimeError(f"command failed: {args}\n{stdout!r}\n{stderr!r}")
        return subprocess.CompletedProcess(args, process.returncode, stdout, stderr)


start = time.monotonic()
commit = bounded(["git", "rev-parse", "--verify", "HEAD"], ROOT).stdout.decode("utf-8").strip()
archive = bounded(["git", "archive", "--format=tar", commit], ROOT).stdout
env = os.environ.copy()
if (ROOT / ".tools/rust/bin").exists():
    env["PATH"] = str(ROOT / ".tools/rust/bin") + os.pathsep + env["PATH"]
env["SOURCE_DATE_EPOCH"] = "0"
env["CARGO_BUILD_JOBS"] = "1"
env.pop("GIT_DIR", None)
env.pop("GIT_WORK_TREE", None)
env.pop("CARGO_TARGET_DIR", None)

records = []
logs = []
for index in range(2):
    with tempfile.TemporaryDirectory(prefix="axeon-clean-", dir=ROOT.parent) as tmp:
        work = Path(tmp)
        with tarfile.open(fileobj=io.BytesIO(archive)) as packed:
            packed.extractall(work, filter="data")
        build_env = env.copy()
        build_env["RUSTFLAGS"] = f"--remap-path-prefix={work}=/axeon"
        # Each fresh build must link its own archive, even if the caller has
        # configured a runtime override for ordinary compiler invocations.
        build_env["AXEON_TENSOR_RUNTIME"] = str(work / "target/release/libaxeon_tensor_runtime.a")

        def run(args):
            result = bounded(args, work, build_env)
            logs.append(
                f"build {index}: {' '.join(args)}\n"
                f"{result.stdout.decode('utf-8', errors='replace')}"
                f"{result.stderr.decode('utf-8', errors='replace')}"
            )
            return result

        run(["cargo", "build", "--offline", "--locked", "--release", "--jobs", "1"])
        compiler = work / "target/release/axeon"
        paths = ["target/release/axeon"]
        native_outputs = {}
        if args.include_tensors:
            paths.append("target/release/libaxeon_tensor_runtime.a")
        if args.include_numerics or args.include_tensors:
            run(["cargo", "build", "--offline", "--locked", "--release", "--jobs", "1",
                 "-p", "axeon-numerics", "--lib", "--example", "cpu_numerics"])
            paths.extend(["target/release/libaxeon_numerics.rlib",
                          "target/release/examples/cpu_numerics"])
            numerical = run([str(work / "target/release/examples/cpu_numerics")])
            if numerical.stderr or not numerical.stdout:
                raise RuntimeError("Numerical example must produce a nonempty result without stderr")
            native_outputs["cpu_numerics_library_example"] = {
                "exit_code": numerical.returncode,
                "stdout": numerical.stdout.decode("utf-8"),
                "stderr": numerical.stderr.decode("utf-8"),
                "scope": "Two-build execution agreement; independent mathematical correctness is covered by the numerical fixture gate.",
            }
        for name, expected_stdout in EXAMPLES.items():
            for command, output in [("emit-object", f"{name}.o"), ("build", name)]:
                run([str(compiler), command, example_source(name), "-o", output, "--opt", "2"])
                paths.extend([output, f"{output}.json"])
            native = run([str(work / name)])
            if native.stdout != expected_stdout.encode("utf-8") or native.stderr != b"":
                raise RuntimeError(
                    f"build {index}: unexpected {name} native output: "
                    f"stdout={native.stdout!r}, stderr={native.stderr!r}"
                )
            native_outputs[name] = {
                "exit_code": native.returncode,
                "stdout": native.stdout.decode("utf-8"),
                "stderr": native.stderr.decode("utf-8"),
            }
        hashes = {}
        for path in paths:
            with (work / path).open("rb") as artifact:
                hashes[path] = hashlib.file_digest(artifact, "sha256").hexdigest()
        records.append({"build": index, "sha256": hashes, "native_outputs": native_outputs})
if records[0]["sha256"] != records[1]["sha256"]:
    raise RuntimeError("clean-build bytes differ")
if records[0]["native_outputs"] != records[1]["native_outputs"]:
    raise RuntimeError("clean-build native outputs differ")
receipt = {
    "schema_version": 3, "source_commit": commit, "exit_code": 0,
    "command": "python3 scripts/reproducibility.py" + (" --include-numerics" if args.include_numerics else "") + (" --include-tensors" if args.include_tensors else "") + (" --include-modules" if args.include_modules else "") + (" --include-records" if args.include_records else "") + (" --include-unions" if args.include_unions else "") + (" --include-aggregate-interfaces" if args.include_aggregate_interfaces else "") + (" --include-nested-aggregates" if args.include_nested_aggregates else "") + (" --include-unit-values" if args.include_unit_values else "") + (" --include-static-traits" if args.include_static_traits else "") + (" --include-generic-functions" if args.include_generic_functions else "") + (" --include-generic-nominals" if args.include_generic_nominals else ""),
    "includes_optional_numerical_library": args.include_numerics or args.include_tensors,
    "includes_tensor_runtime_archive": args.include_tensors,
    "includes_local_modules": args.include_modules,
    "includes_local_records": args.include_records,
    "includes_local_unions": args.include_unions,
    "includes_aggregate_interfaces": args.include_aggregate_interfaces,
    "includes_nested_aggregates": args.include_nested_aggregates,
    "includes_unit_values": args.include_unit_values,
    "includes_static_traits": args.include_static_traits,
    "includes_generic_functions": args.include_generic_functions,
    "includes_generic_nominals": args.include_generic_nominals,
    "cargo_jobs": 1,
    "output_directory": str(EVIDENCE.relative_to(ROOT)) if EVIDENCE.is_relative_to(ROOT) else str(EVIDENCE),
    "independent_builds": 2,
    "shared_target_directory": False, "compiler_path_remapping": "/axeon",
    "subprocess_timeout_seconds": TIMEOUT_SECONDS,
    "timeout_kills_subprocess_group": True, "optimization": 2,
    "examples": {
        name: {"source": example_source(name), "expected_stdout": stdout,
               "expected_stderr": "", "expected_exit_code": 0}
        for name, stdout in EXAMPLES.items()
    },
    "compiler_objects_executables_and_metadata_byte_identical": True,
    "native_execution_verified": True, "temporary_checkouts_removed": True,
    "wall_seconds": round(time.monotonic() - start, 3), "builds": records,
    "scope": "Same host and pinned toolchain; compiler built from committed source in separate fresh directories with path remapping; no cross-host claim.",
}
(EVIDENCE / "clean-build.json").write_text(json.dumps(receipt, indent=2) + "\n")
(EVIDENCE / "clean-build.log").write_text("\n".join(logs))
print(json.dumps(receipt, indent=2))

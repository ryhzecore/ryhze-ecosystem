#!/usr/bin/env python3
"""Package existing reproducible Axeon artifacts; never build or install tools.

Run from the same committed source checkout/snapshot used by reproducibility.py:
python3 scripts/package-toolchain.py --compiler target/release/axeon \
  --tensor-runtime target/release/libaxeon_tensor_runtime.a \
  --reproduction PATH/clean-build.json --output-dir NEW_PACKAGE_DIRECTORY

Requires Python 3.11+, Linux x86-64, and Clang 19.1.7 selected by AXEON_CLANG
or clang-19 on PATH. Existing destinations are never reused. Failed staging
directories and their package receipts remain for inspection. No upload/archive.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import runpy
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import tomllib


ROOT = Path(__file__).resolve().parent.parent
COMPILER_KEY = "target/release/axeon"
RUNTIME_KEY = "target/release/libaxeon_tensor_runtime.a"
MAX_BINARY = 512 << 20
MAX_RUNTIME = 128 << 20
MAX_DOCUMENT = 4 << 20
MAX_RECEIPT = 16 << 20
MAX_ANCILLARY_BYTES = 64 << 20
MAX_ANCILLARY_FILES = 4096
SMOKE_TIMEOUT = 45
MODULE_OUTPUT = b"42\nf64:0x4018000000000000\nresult: 42\n"
RECORD_OUTPUT = b"f64:0x4010000000000000\nresult: 2\n"
UNION_OUTPUT = b"f64:0x4002000000000000\nresult: 0\n"
AGGREGATE_OUTPUT = b"f64:0x402f000000000000\nresult: 3\n"
NESTED_OUTPUT = b"f64:0x402f000000000000\nf64:0x8000000000000000\nresult: 3\n"
GENERIC_NOMINAL_OUTPUT = b"42\n-9223372036854775808\nresult: 0\n"
GENERIC_NOMINAL_TEST_OUTPUT = (
    b"running 3 tests (reference interpreter; fuel 1000000 per test)\n"
    b"test test_absent_payload_retains_negative_zero ...\nPASS test_absent_payload_retains_negative_zero\n"
    b"test test_exact_array_payload ...\nPASS test_exact_array_payload\n"
    b"test test_nested_generic_payload ...\nPASS test_nested_generic_payload\n"
    b"test result: 3 passed; 0 failed\n")
STARTER_OUTPUT = b"42\nresult: 0\n"
TEST_OUTPUT = (b"running 2 tests (reference interpreter; fuel 1000000 per test)\n"
               b"test test_negative ...\nPASS test_negative\n"
               b"test test_twice ...\nPASS test_twice\n"
               b"test result: 2 passed; 0 failed\n")


class PackageError(RuntimeError):
    pass


def no_symlinks(path):
    path = Path(path).absolute()
    for part in reversed((path, *path.parents)):
        if part.is_symlink():
            raise PackageError(f"symlink input is unsupported: {part}")
    return path


def regular_file(path, maximum):
    path = no_symlinks(path)
    metadata = path.stat()
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > maximum:
        raise PackageError(f"expected regular file of at most {maximum} bytes: {path}")
    return path


def read_regular(path, maximum):
    path = regular_file(path, maximum)
    descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as source:
        if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
            raise PackageError(f"input changed to a nonregular file: {path}")
        data = source.read(maximum + 1)
    if len(data) > maximum:
        raise PackageError(f"input grew beyond its byte limit: {path}")
    return data


def file_hash(path, maximum=MAX_BINARY):
    path = regular_file(path, maximum)
    result, size = hashlib.sha256(), 0
    descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as source:
        if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
            raise PackageError(f"input changed to a nonregular file: {path}")
        for block in iter(lambda: source.read(1024 * 1024), b""):
            size += len(block)
            if size > maximum:
                raise PackageError(f"input grew beyond its byte limit: {path}")
            result.update(block)
    return result.hexdigest()


def validate_reproduction(receipt, compiler_hash, runtime_hash):
    if not isinstance(receipt, dict):
        raise PackageError("reproduction receipt must be a JSON object")
    if type(receipt.get("schema_version")) is not int or receipt["schema_version"] != 3 or type(receipt.get("exit_code")) is not int or receipt["exit_code"] != 0:
        raise PackageError("a successful schema-3 reproducibility.py receipt is required")
    commit = receipt.get("source_commit", "")
    if not isinstance(commit, str) or re.fullmatch(r"[0-9a-f]{40}", commit) is None:
        raise PackageError("receipt source_commit must be a complete 40-digit Git commit")
    for name in ("compiler_objects_executables_and_metadata_byte_identical",
                 "native_execution_verified", "includes_tensor_runtime_archive", "includes_local_modules",
                 "includes_local_records", "includes_local_unions", "includes_aggregate_interfaces", "includes_nested_aggregates", "includes_unit_values", "includes_static_traits", "includes_generic_functions", "includes_generic_nominals"):
        if receipt.get(name) is not True:
            raise PackageError(f"successful reproduction requires {name}=true")
    if receipt.get("independent_builds") != 2 or receipt.get("shared_target_directory") is not False:
        raise PackageError("two independent builds without a shared target directory are required")
    builds = receipt.get("builds")
    if not isinstance(builds, list) or len(builds) != 2:
        raise PackageError("receipt must contain both independent builds")
    if (any(not isinstance(build, dict) or type(build.get("build")) is not int for build in builds)
            or {build["build"] for build in builds} != {0, 1}):
        raise PackageError("receipt must identify distinct builds 0 and 1")
    for build in builds:
        hashes = build.get("sha256")
        if not isinstance(hashes, dict) or any(
            not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None
            for value in hashes.values()
        ):
            raise PackageError("each build requires valid SHA-256 artifact hashes")
        for name, expected in ((COMPILER_KEY, compiler_hash), (RUNTIME_KEY, runtime_hash)):
            if hashes.get(name) != expected:
                raise PackageError(f"provided {name} does not match build {build['build']}")
    if builds[0]["sha256"] != builds[1]["sha256"]:
        raise PackageError("independent build artifact maps differ")
    if (not isinstance(builds[0].get("native_outputs"), dict) or not builds[0]["native_outputs"]
            or builds[0]["native_outputs"] != builds[1].get("native_outputs")):
        raise PackageError("independent native execution records are missing or different")
    example = {
        "source": "examples/generic-nominals/main.ax",
        "expected_stdout": GENERIC_NOMINAL_OUTPUT.decode("ascii"),
        "expected_stderr": "", "expected_exit_code": 0,
    }
    examples = receipt.get("examples")
    if (not isinstance(examples, dict) or examples.get("generic-nominals") != example
            or type(examples["generic-nominals"].get("expected_exit_code")) is not int):
        raise PackageError("generic nominal reproduction example and exact expectations are required")
    expected_output = {"exit_code": 0, "stdout": example["expected_stdout"], "stderr": ""}
    for build in builds:
        output = build["native_outputs"].get("generic-nominals")
        if (output != expected_output or type(output.get("exit_code")) is not int):
            raise PackageError("both builds must verify the exact generic nominal native result")
        for artifact in ("generic-nominals.o", "generic-nominals.o.json",
                         "generic-nominals", "generic-nominals.json"):
            if artifact not in build["sha256"]:
                raise PackageError(f"generic nominal reproduction artifact is missing: {artifact}")
    return commit


def ancillary_files(root):
    paths = []
    for directory in (root / "docs", root / "examples"):
        if directory.is_symlink() or not directory.is_dir():
            raise PackageError(f"required source directory is missing or linked: {directory}")
        pending = [directory]
        entries = 0
        while pending:
            current = pending.pop()
            # Bound enumeration itself; sorting an iterator first would consume
            # an arbitrarily large directory before reaching the count check.
            with os.scandir(current) as children:
                for entry in children:
                    entries += 1
                    if entries > MAX_ANCILLARY_FILES:
                        raise PackageError("source directory entry limit exceeded")
                    child = Path(entry.path)
                    if entry.is_symlink():
                        raise PackageError(f"symlink source entry: {child}")
                    if entry.is_dir(follow_symlinks=False):
                        pending.append(child)
                    else:
                        paths.append(regular_file(child, MAX_DOCUMENT))
    paths.extend(regular_file(root / name, MAX_DOCUMENT) for name in
                 ("runtime/runtime.c", "runtime/runtime.h", "toolchain-lock.json", "README.md"))
    if any(any(character in path.relative_to(root).as_posix() for character in "\r\n\\") for path in paths):
        raise PackageError("source names containing newlines or backslashes cannot enter SHA256SUMS")
    if len(paths) > MAX_ANCILLARY_FILES or sum(path.stat().st_size for path in paths) > MAX_ANCILLARY_BYTES:
        raise PackageError("ancillary package size or file count exceeds its bound")
    return sorted(paths)


def copy_regular(source, destination, maximum, expected_hash=None, mode=0o644):
    source = regular_file(source, maximum)
    destination.parent.mkdir(parents=True, exist_ok=True)
    hashed, size = hashlib.sha256(), 0
    descriptor = os.open(source, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as original, destination.open("xb") as copied:
        if not stat.S_ISREG(os.fstat(original.fileno()).st_mode):
            raise PackageError("source changed to a nonregular file")
        for block in iter(lambda: original.read(1024 * 1024), b""):
            size += len(block)
            if size > maximum:
                raise PackageError("source grew beyond its copy limit")
            copied.write(block)
            hashed.update(block)
    digest = hashed.hexdigest()
    if expected_hash is not None and digest != expected_hash:
        raise PackageError(f"source bytes no longer match reproduction: {source}")
    if file_hash(destination, maximum) != digest:
        raise PackageError(f"copied bytes differ: {destination}")
    destination.chmod(mode)
    return digest


def write_atomic_text(path, write, exclusive=False):
    """Publish complete text only; preserve an existing receipt on write failure."""
    descriptor, name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            write(output)
            output.flush()
            os.fsync(output.fileno())
        temporary.chmod(0o644)
        if exclusive:
            # Same-directory hard linking is atomic and refuses an existing name.
            os.link(temporary, path)
        else:
            os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def write_json(path, value, exclusive=False):
    def serialize(output):
        json.dump(value, output, indent=2, allow_nan=False)
        output.write("\n")
    write_atomic_text(path, serialize, exclusive)


def write_manifest(path, manifest):
    def serialize(output):
        for relative, checksum in manifest.items():
            output.write(f"{checksum}  {relative}\n")
    write_atomic_text(path, serialize, exclusive=True)


def smoke_limits():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (MAX_RUNTIME, MAX_RUNTIME))


def smoke(package, receipt, name, command, expected=b"", environment=None):
    index = len(receipt["checks"])
    stdout_path = package / "checks" / f"{index:02}-{name}.stdout"
    stderr_path = package / "checks" / f"{index:02}-{name}.stderr"
    timed_out = False
    started = time.monotonic()
    with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
        with subprocess.Popen(command, cwd=package, env=environment, stdin=subprocess.DEVNULL,
                              stdout=stdout, stderr=stderr, start_new_session=True,
                              preexec_fn=smoke_limits) as process:
            try:
                code = process.wait(timeout=SMOKE_TIMEOUT)
            except subprocess.TimeoutExpired:
                timed_out = True
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                code = process.wait()
            except BaseException:
                if process.poll() is None:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                process.wait()
                raise
    check = {"name": name, "command": command, "exit_code": code,
             "timed_out": timed_out, "wall_seconds": time.monotonic() - started,
             "stdout_sha256": file_hash(stdout_path, MAX_RUNTIME),
             "stderr_sha256": file_hash(stderr_path, MAX_RUNTIME), "passed": False}
    receipt["checks"].append(check)
    write_json(package / "package-receipt.json", receipt)
    stdout = read_regular(stdout_path, 65536)
    stderr = read_regular(stderr_path, 65536)
    check.update({"stdout": stdout.decode("utf-8", errors="replace"),
                  "stderr": stderr.decode("utf-8", errors="replace")})
    if timed_out or code != 0 or stderr or (expected is not None and stdout != expected):
        raise PackageError(f"package smoke check failed: {name}; inspect checks/{index:02}-{name}.*")
    check["passed"] = True
    write_json(package / "package-receipt.json", receipt)
    return stdout


def run_smokes(package, receipt, source_version):
    environment = os.environ.copy()
    environment.pop("AXEON_TENSOR_RUNTIME", None)  # Exercise adjacent packaged archive discovery.
    environment.update({"LC_ALL": "C", "SOURCE_DATE_EPOCH": "0"})
    clang = environment.get("AXEON_CLANG") or shutil.which("clang-19")
    if not clang:
        raise PackageError("Clang 19.1.7 is required; set AXEON_CLANG or provide clang-19 on PATH")
    environment["AXEON_CLANG"] = clang
    version = smoke(package, receipt, "clang-version", [clang, "--version"], None, environment)
    lines = version.decode("utf-8").splitlines()
    if not lines:
        raise PackageError("Clang version probe returned no identity")
    first = lines[0]
    if re.search(r"\bclang version 19\.1\.7(?:\s|$)", first) is None:
        raise PackageError(f"unsupported native compiler: {first}")
    receipt["clang_version"] = first
    axeon = str(package / "bin" / "axeon")
    version = smoke(package, receipt, "axeon-version", [axeon, "--version"], None, environment)
    identity = re.fullmatch(rb"axeon ([^;\s]+); subset ([0-9]+); LLVM/Clang 19\.1\.7;[^\n]+\n", version)
    if identity is None or identity.group(1).decode("ascii") != source_version:
        raise PackageError("packaged compiler identity differs from source Cargo.toml or supported backend")
    receipt.update({"compiler_version": identity.group(1).decode("ascii"),
                    "language_subset": int(identity.group(2)), "version_output": version.decode("utf-8")})
    for name, source, expected in (("records", "examples/records.ax", RECORD_OUTPUT),
                                    ("unions", "examples/unions.ax", UNION_OUTPUT),
                                    ("aggregate-interfaces", "examples/aggregate-modules/main.ax", AGGREGATE_OUTPUT),
                                    ("nested-aggregates", "examples/nested-modules/main.ax", NESTED_OUTPUT),
                                    ("unit-values", "examples/unit-modules/main.ax", b"1\n2\n3\n42\nresult: 0\n"),
                                    ("static-traits", "examples/trait-modules/main.ax", b"42\nresult: 0\n"),
                                    ("generic-functions", "examples/generic-modules/main.ax", b"42\n7\n9\nresult: 0\n"),
                                    ("generic-nominals", "examples/generic-nominals/main.ax", GENERIC_NOMINAL_OUTPUT),
                                    ("modules", "examples/modules/main.ax", MODULE_OUTPUT)):
        for mode in ("check", "interpret", "run"):
            args = [axeon, mode, source] + (["--opt", "3"] if mode == "run" else [])
            smoke(package, receipt, f"{name}-{mode}", args, b"" if mode == "check" else expected, environment)
    smoke(package, receipt, "generic-nominals-test",
          [axeon, "test", "examples/generic-nominals/tests.ax"], GENERIC_NOMINAL_TEST_OUTPUT, environment)
    (package / "smoke").mkdir()
    smoke(package, receipt, "new", [axeon, "new", "smoke/project"],
          b"created project smoke/project (main.ax, math.ax, tests.ax)\n", environment)
    smoke(package, receipt, "new-check", [axeon, "check", "smoke/project/main.ax"], environment=environment)
    smoke(package, receipt, "new-interpret", [axeon, "interpret", "smoke/project/main.ax"], STARTER_OUTPUT, environment)
    for name in ("main", "math", "tests"):
        smoke(package, receipt, f"new-format-{name}",
              [axeon, "format", f"smoke/project/{name}.ax", "--check"], environment=environment)
    smoke(package, receipt, "new-test", [axeon, "test", "smoke/project/tests.ax"], TEST_OUTPUT, environment)
    smoke(package, receipt, "new-build", [axeon, "build", "smoke/project/main.ax", "--opt", "3",
                                            "-o", "bin/hello-example"], environment=environment)
    smoke(package, receipt, "standalone", [str(package / "bin/hello-example")], STARTER_OUTPUT, environment)
    sidecar = json.loads(read_regular(package / "bin/hello-example.json", MAX_DOCUMENT))
    if sidecar.get("artifact_sha256") != file_hash(package / "bin/hello-example"):
        raise PackageError("standalone example receipt does not match executable bytes")
    receipt["runtime_discovered_beside_compiler"] = True


def package_readme(receipt):
    return f"""# Axeon {receipt['compiler_version']} local Linux toolchain

Compiler identity: `{receipt['version_output'].strip()}`.
Source commit: `{receipt['source_commit']}`. Tensor runtime package version: **0.1.0**.
The compiler and tensor archive match **both independent committed-source builds**
in [the reproduction receipt](reproduction.json). The [package receipt](package-receipt.json)
records the actual smoke-test host, commands and outputs.

These are **Linux x86-64 ELF executables**, qualified within the recorded Debian/WSL
host scope. They are not native Windows/macOS programs or a cross-host qualification.
Native compilation needs Clang **19.1.7**, a compatible hosted C/linker/libc development
environment and GNU `timeout`/`sha256sum`. No operating system or native compiler is
vendored. Check/interpret/test/format do not need Clang. Keep
`bin/libaxeon_tensor_runtime.a` beside `bin/axeon`; no installation or credentials are required.

From this extracted directory in Linux/WSL (use a fresh `demo` destination):

```sh
sha256sum -c SHA256SUMS
export PATH="$PWD/bin:$PATH"
export AXEON_CLANG=/usr/bin/clang-19 # or your verified Clang 19.1.7 path
unset AXEON_TENSOR_RUNTIME         # use the packaged adjacent archive
axeon --version
axeon check examples/records.ax
axeon interpret examples/records.ax
axeon run examples/unions.ax --opt 3
axeon run examples/aggregate-modules/main.ax --opt 3
axeon run examples/nested-modules/main.ax --opt 3
axeon run examples/unit-modules/main.ax --opt 3
axeon run examples/trait-modules/main.ax --opt 3
axeon test examples/trait-modules/tests.ax
axeon run examples/generic-modules/main.ax --opt 3
axeon test examples/generic-modules/tests.ax
axeon check examples/generic-nominals/main.ax
axeon interpret examples/generic-nominals/main.ax
axeon run examples/generic-nominals/main.ax --opt 3
axeon test examples/generic-nominals/tests.ax
axeon run examples/modules/main.ax --opt 3
axeon new demo
axeon check demo/main.ax
axeon test demo/tests.ax
axeon build demo/main.ax -o demo-program --opt 3
./demo-program
./bin/hello-example
```

The record example prints `f64:0x4010000000000000` and `result: 2`; the module
example prints `42`, `f64:0x4018000000000000` and `result: 42`. The union example
prints `f64:0x4002000000000000` and `result: 0`. Both starter
executables print `42` and `result: 0`. Tests use the reference interpreter.
The aggregate-module example prints `f64:0x402f000000000000` and `result: 3`.
The nested-module example additionally prints `f64:0x8000000000000000`, preserving negative zero.
The generic-module example prints `42`, `7`, `9` and `result: 0`; its concrete
test wrappers check nested-owner, empty-packet, scalar and unit instances.
The generic-nominal example prints `42`, `-9223372036854775808` and `result: 0`;
its three tests cover exact arrays, negative zero and nested generic payloads.
Distribute generated native executables with their `.json` receipts and required
compatible Linux shared libraries; the target does not need the Axeon compiler.

Read the [language contract](docs/language.md), [records](docs/records-design.md),
[unions](docs/unions-design.md),
[owned aggregate interfaces](docs/aggregate-interfaces-design.md),
[nested aggregates and partial moves](docs/nested-aggregates-design.md),
[explicit generic functions](docs/generic-functions.md),
[finite generic records and unions](docs/generic-nominals.md),
[modules](docs/modules-design.md), [numerical rules](docs/numerics-design.md),
[developer workflow](docs/workflow-design.md), [support matrix](docs/support.json)
and [architecture coverage](docs/architecture-coverage.md). These copies come from
the caller-supplied same-commit source snapshot; every packaged file is hashed.
Historical reports describe their own versions, not additional current validation.
The original source [README](SOURCE-README.md) is retained and may reference files
available only in the full source checkout. Full source and Git history are separate
from this binary distribution; build its recorded commit with the pinned toolchain
in [toolchain-lock.json](toolchain-lock.json). From that source checkout, run:

```sh
cargo build --offline --locked --release --jobs 1
python3 scripts/reproducibility.py --include-tensors --include-modules --include-records --include-unions --include-aggregate-interfaces --include-nested-aggregates --include-unit-values --include-static-traits --include-generic-functions --include-generic-nominals --output-dir NEW_EVIDENCE
```

The complete architecture remains unfinished. Recursive value layouts, generic
traits/implementations, inference, default/const arguments, associated types,
field reinitialization, borrowed interfaces, package resolution, LSP/debugging, public FFI and GPU/NPU/distributed
backends are not made available by this package. Existing precision, ownership,
source/resource budgets and host restrictions remain in force. This is a local
checkpoint, not a public release or device/performance certification.
"""


def package_toolchain(root, compiler, runtime, reproduction, package):
    receipt = {"schema_version": 1, "status": "staging", "checks": [],
               "target": "x86_64-pc-linux-gnu", "tensor_runtime_version": "0.1.0",
               "host": {"platform": platform.platform(), "kernel": platform.release(),
                        "machine": platform.machine()},
               "source_snapshot_contract": "Caller must run this script from the same source commit as reproduction; ancillary files are hashed, not independently Git-attested.",
               "limits": {"smoke_timeout_seconds": SMOKE_TIMEOUT, "ancillary_files": MAX_ANCILLARY_FILES,
                          "ancillary_total_bytes": MAX_ANCILLARY_BYTES, "symlinks": "rejected"}}
    write_json(package / "package-receipt.json", receipt, exclusive=True)
    try:
        compiler_hash, runtime_hash = file_hash(compiler), file_hash(runtime, MAX_RUNTIME)
        raw_reproduction = read_regular(reproduction, MAX_RECEIPT)
        evidence = json.loads(raw_reproduction)
        receipt["source_commit"] = validate_reproduction(evidence, compiler_hash, runtime_hash)
        with regular_file(compiler, MAX_BINARY).open("rb") as source:
            header = source.read(20)
        if len(header) != 20 or header[:6] != b"\x7fELF\x02\x01" or header[18:20] != b"\x3e\x00":
            raise PackageError("compiler must be a little-endian x86-64 ELF executable")
        with regular_file(runtime, MAX_RUNTIME).open("rb") as source:
            if source.read(8) != b"!<arch>\n":
                raise PackageError("tensor runtime must be a regular static archive")
        runtime_toml = tomllib.loads(read_regular(root / "crates/axeon-tensor-runtime/Cargo.toml", MAX_DOCUMENT).decode("utf-8"))
        if runtime_toml.get("package", {}).get("version") != "0.1.0":
            raise PackageError("this package contract requires tensor runtime version 0.1.0")
        source_version = tomllib.loads(read_regular(root / "Cargo.toml", MAX_DOCUMENT).decode("utf-8"))["package"]["version"]
        files = ancillary_files(root)
        (package / "checks").mkdir()
        receipt["reproduction_sha256"] = hashlib.sha256(raw_reproduction).hexdigest()
        (package / "reproduction.json").write_bytes(raw_reproduction)
        copied, copied_bytes = {}, 0
        for source in files:
            relative = source.relative_to(root)
            if relative == Path("README.md"):
                relative = Path("SOURCE-README.md")
            copied[relative.as_posix()] = copy_regular(source, package / relative, MAX_DOCUMENT)
            copied_bytes += (package / relative).stat().st_size
            if copied_bytes > MAX_ANCILLARY_BYTES:
                raise PackageError("ancillary files grew beyond the aggregate copy budget")
        copied["bin/axeon"] = copy_regular(compiler, package / "bin/axeon", MAX_BINARY, compiler_hash, 0o755)
        copied["bin/libaxeon_tensor_runtime.a"] = copy_regular(runtime, package / "bin/libaxeon_tensor_runtime.a", MAX_RUNTIME, runtime_hash)
        run_smokes(package, receipt, source_version)
        for relative, expected in copied.items():
            if file_hash(package / relative) != expected:
                raise PackageError(f"packaged input changed during smoke tests: {relative}")
        (package / "README.md").write_text(package_readme(receipt), encoding="utf-8")
        receipt.update({"status": "passed", "matches_both_independent_builds": True,
                        "compiler_sha256": compiler_hash, "tensor_runtime_sha256": runtime_hash,
                        "copied_input_files": copied})
        write_json(package / "package-receipt.json", receipt)
        entries = sorted(package.rglob("*"))
        if any(path.is_symlink() for path in entries):
            raise PackageError("an unexpected symlink appeared in the staged package")
        paths = [path for path in entries if path.is_file()]
        manifest = {path.relative_to(package).as_posix(): file_hash(path) for path in paths}
        write_manifest(package / "SHA256SUMS", manifest)
        return 0
    except (OSError, ValueError, KeyError, PackageError) as error:
        receipt.update({"status": "failed", "failure": str(error)})
        write_json(package / "package-receipt.json", receipt)
        print(f"Package incomplete; preserved {package}: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        receipt.update({"status": "interrupted", "failure": "packaging interrupted; staging preserved"})
        write_json(package / "package-receipt.json", receipt)
        return 130


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("compiler", "tensor-runtime", "reproduction"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path,
                        help="new package directory; defaults to local Releases/candidates/<source-commit>/package")
    args = parser.parse_args(argv)
    if sys.platform != "linux" or platform.machine() != "x86_64":
        parser.error("packaging smoke tests require Linux x86-64")
    try:
        # Validate input path kinds before staging; validation failures still
        # create no public artifact or modify any prior output directory.
        compiler = regular_file(args.compiler, MAX_BINARY)
        runtime = regular_file(args.tensor_runtime, MAX_RUNTIME)
        reproduction = regular_file(args.reproduction, MAX_RECEIPT)
        if args.output_dir is None:
            layout = runpy.run_path(str(ROOT / "scripts/project_layout.py"))
            args.output_dir = layout["candidate_package_output"](ROOT)
            no_symlinks(args.output_dir.parent).mkdir(parents=True, exist_ok=True)
        package = no_symlinks(args.output_dir)
        package.mkdir()
    except (OSError, ValueError, PackageError) as error:
        parser.error(str(error))
    result = package_toolchain(ROOT, compiler, runtime, reproduction, package)
    print(json.dumps({"status": "passed" if result == 0 else "incomplete",
                      "package": str(package), "receipt": str(package / "package-receipt.json")}, indent=2))
    return result


if __name__ == "__main__":
    raise SystemExit(main())

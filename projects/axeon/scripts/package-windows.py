#!/usr/bin/env python3
"""Stage and qualify an internal native Windows candidate from matching evidence.

No tool downloads, compiler bootstrap builds, public upload, signing or promotion.
Existing destinations and all failed staging/installation attempts are preserved.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import sys
import zipfile

API = runpy.run_path(str(Path(__file__).with_name("reproduce-windows.py")))
need, safe, regular, sha, load, write_json, fresh = (
    API[name] for name in ("need", "safe_path", "regular", "sha", "load", "write_json", "fresh"))
TARGET = API["TARGET"]
MAX_PAYLOAD_FILES = 4096
MAX_PAYLOAD_BYTES = 256 << 20
MAX_DOCUMENT = 8 << 20
# Read-only inspector from the qualified LLVM19.1.7 Windows x64 toolchain.
# Record its version as well; an unqualified replacement must not silently pass.
READOBJ_SHA256 = "610ddab6a3f7621da7161f466cf12d1b28cba03a9eda6d0e6e36a0c4e9b7c748"


def parse_pe_imports(output):
    """Accept only the pinned reader's x64 PE output and ordinary DLL imports."""
    text = output.decode("utf-8").replace("\r\n", "\n")
    for field, expected in (("Format", "COFF-x86-64"), ("Arch", "x86_64"), ("AddressSize", "64bit")):
        need(re.findall(rf"(?m)^{field}: (.+)$", text) == [expected],
             f"unsupported PE inspection {field}")
    need(re.findall(r"(?m)^\s+Machine: (.+)$", text) ==
         ["IMAGE_FILE_MACHINE_AMD64 (0x8664)"], "unsupported PE machine")
    # Delayed imports need their own parser and evidence before they are accepted.
    for field in ("DelayImportDescriptorRVA", "DelayImportDescriptorSize"):
        values = re.findall(rf"(?m)^\s+{field}: (0x[0-9A-Fa-f]+)\s*$", text)
        need(len(values) == 1 and int(values[0], 16) == 0,
             "missing or unsupported delayed PE import inventory")
    need(re.search(r"(?m)^DelayImport", text) is None, "unsupported delayed PE imports")
    blocks = re.findall(r"(?ms)^Import \{\r?\n(.*?)^\}\r?$", text)
    need(0 < len(blocks) <= 512 and len(blocks) == len(re.findall(r"(?m)^Import \{", text)),
         "missing, malformed or excessive PE import blocks")
    names = []
    for block in blocks:
        values = re.findall(r"(?im)^  Name: ([a-z0-9_.-]+\.dll)\r?$", block)
        need(len(values) == 1, "missing or unsupported PE import DLL name")
        names.extend(values)
    need(len(set(name.casefold() for name in names)) == len(names), "duplicate PE import DLL name")
    return {"format": "COFF-x86-64", "architecture": "x86_64", "direct_dll_imports": names,
            "delayed_dll_imports": [], "scope": "direct PE import tables; not recursive DLL resolution"}


def inspect_pe(path, expected_hash, reader, inspect):
    path = regular(path)
    digest = sha(path)
    need(digest == expected_hash, f"PE input differs before inspection: {path}")
    API["pe"](path)
    out, err, check = inspect([reader, "--file-headers", "--coff-imports", path])
    need(err == b"", f"PE inspector reported diagnostics for {path}")
    imports = parse_pe_imports(out)
    need(sha(path) == digest, f"PE input changed during inspection: {path}")
    return {"path": str(path), "sha256": digest, **imports,
            "inspection": {"tool_sha256": READOBJ_SHA256,
                "stdout": {"path": check["stdout_path"], "sha256": check["stdout_sha256"]},
                "stderr": {"path": check["stderr_path"], "sha256": check["stderr_sha256"]}}}


def portable_imports(inventory):
    return {name: inventory[name] for name in
            ("sha256", "format", "architecture", "direct_dll_imports", "delayed_dll_imports", "scope")}


def launch_dependencies(inventories):
    names = sorted({name for item in inventories.values() for name in item["direct_dll_imports"]}, key=str.casefold)
    visual_cpp = [name for name in names if re.fullmatch(r"(?i)(?:vcruntime|msvcp|concrt)[a-z0-9_]*\.dll", name)]
    requirements = []
    if visual_cpp:
        requirements.append("Microsoft Visual C++ x64 runtime providing " + ", ".join(visual_cpp))
    if any(name.casefold() == "ucrtbase.dll" or name.casefold().startswith("api-ms-win-crt-") for name in names):
        requirements.append("Windows Universal CRT (UCRT)")
    requirements.append("All direct DLL/API-set dependencies listed in pe-imports.json")
    return requirements


def copy_file(source, destination, expected=None, maximum=API["MAX_FILE"]):
    source = regular(source, maximum)
    digest = sha(source, maximum)
    if expected is not None:
        need(digest == expected, f"input hash differs: {source}")
    safe(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as src, destination.open("xb") as dst:
        copied = 0
        for block in iter(lambda: src.read(1 << 20), b""):
            copied += len(block)
            need(copied <= maximum, "copy input grew beyond limit")
            dst.write(block)
    need(sha(destination, maximum) == digest and sha(source, maximum) == digest,
         f"copy verification failed: {source}")


def checksums(package):
    files, total = {}, 0
    for directory, dirs, names in os.walk(package, followlinks=False):
        for name in dirs:
            safe(Path(directory) / name)
        for name in names:
            path = Path(directory) / name
            relative = API["relative_name"](path.relative_to(package).as_posix())
            if relative == "SHA256SUMS":
                continue
            files[relative] = sha(path)
            total += path.stat().st_size
            need(len(files) <= MAX_PAYLOAD_FILES and total <= MAX_PAYLOAD_BYTES, "package payload budget exceeded")
    return dict(sorted(files.items()))


def verify_manifest(package):
    values = {}
    for line in regular(package / "SHA256SUMS", 1 << 20).read_text("utf-8").splitlines():
        need(re.fullmatch(r"[0-9a-f]{64}  .+", line) is not None, "invalid checksum line")
        name = API["relative_name"](line[66:])
        need(name not in values and name != "SHA256SUMS", "duplicate or self-referential checksum")
        values[name] = line[:64]
    need(values == checksums(package), "package SHA256SUMS differs from exact payload")
    return values


class DigestWriter(io.RawIOBase):
    """Use identical unseekable ZIP framing for the file and replay hash."""
    def __init__(self, destination=None):
        self.destination = destination
        self.digest = hashlib.sha256()
        self.count = 0

    def writable(self):
        return True

    def tell(self):
        return self.count

    def write(self, data):
        if self.destination is not None:
            self.destination.write(data)
        self.digest.update(data)
        self.count += len(data)
        return len(data)


def write_zip(package, destination=None):
    writer = DigestWriter(destination)
    names = sorted([*verify_manifest(package), "SHA256SUMS"])
    with zipfile.ZipFile(writer, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as archive:
        for name in names:
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_STORED
            with archive.open(info, "w", force_zip64=True) as target, (package / name).open("rb") as source:
                shutil.copyfileobj(source, target, 1 << 20)
    return writer.digest.hexdigest(), writer.count


def extract_zip(archive_path, destination, expected_hash):
    need(sha(archive_path) == expected_hash, "candidate ZIP changed before extraction")
    destination.mkdir()
    total, seen = 0, set()
    with zipfile.ZipFile(archive_path) as archive:
        entries = archive.infolist()
        need(len(entries) <= MAX_PAYLOAD_FILES + 1, "ZIP entry limit exceeded")
        for item in entries:
            name = API["relative_name"](item.filename)
            need(name.casefold() not in seen and not item.is_dir() and (item.external_attr >> 16) == 0o100644,
                 "duplicate, redirected or nonregular ZIP entry")
            seen.add(name.casefold())
            total += item.file_size
            need(0 <= item.file_size <= API["MAX_FILE"] and total <= MAX_PAYLOAD_BYTES + (1 << 20), "ZIP byte budget exceeded")
            output = safe(destination / name)
            output.parent.mkdir(parents=True, exist_ok=True)
            count = 0
            with archive.open(item) as src, output.open("xb") as dst:
                for block in iter(lambda: src.read(1 << 20), b""):
                    count += len(block)
                    need(count <= item.file_size, "ZIP member exceeded declared size")
                    dst.write(block)
            need(count == item.file_size, "truncated ZIP member")
    need(sha(archive_path) == expected_hash, "candidate ZIP changed during extraction")
    verify_manifest(destination)


def evidence(args, reproduction, build_record):
    identity = {"source_commit": reproduction["source_commit"], "target": TARGET,
                "compiler_sha256": reproduction["artifact_sha256"]["bin/axeon.exe"],
                "runtime_sha256": reproduction["artifact_sha256"]["bin/axeon_tensor_runtime.lib"],
                "supervisor_sha256": reproduction["artifact_sha256"]["bin/axeon-process.exe"]}
    examples = load(args.examples_receipt)
    gate = load(args.windows_gate)
    for name, value in (("native examples", examples), ("Windows gate", gate)):
        need(value.get("status") == "passed" and value.get("wsl_invoked_by_harness") is False,
             f"successful native Windows {name} receipt required")
        for field, expected in identity.items():
            need(value.get(field) == expected, f"{name} identity differs: {field}")
    source_manifest = load(args.examples_source_manifest)
    need(sha(args.examples_source_manifest) == examples["source_manifest_sha256"], "example source manifest changed")
    need(source_manifest["commit"] == identity["source_commit"]
         and source_manifest["archive_sha256"] == reproduction["archive_sha256"], "example source archive differs")
    files = source_manifest["files"]
    need(isinstance(files, list) and len(files) <= API["MAX_FILES"], "invalid example source manifest")
    file_map = {}
    for item in files:
        name = API["relative_name"](item["path"])
        need(name not in file_map, "duplicate example source input")
        file_map[name] = item["sha256"]
    need({name: value for name, value in file_map.items() if API["selected"](name)} == build_record["source_files"],
         "example source inputs differ from independently reproduced source")
    need(examples.get("oracle_sha256") == build_record["source_files"]["scripts/record-native-examples.py"],
         "native example oracle differs")
    expected = API["expected_examples"](Path(build_record["source"]))
    cases = examples.get("checks", [])
    need(cases and len(cases) <= 1000 and all(x.get("passed") is True for x in cases), "incomplete native examples")
    names = {case["name"] for case in cases}
    need(len(names) == len(cases), "duplicate native example checks")
    required = {"ownership-negative", "coff-object", "generic-nominal-tests"}
    for name in expected:
        label = name.replace("/", "-")
        required.add(label + "-interpret")
        required.update(label + f"-o{opt}-{stage}" for opt in (0, 2, 3) for stage in ("build", "execute"))
    need(required == names and examples.get("example_count") == len(expected) and examples.get("failures") == 0
         and examples.get("passed") == len(cases),
         "native O0/O2/O3 example gates are incomplete")
    for case in cases:
        for stream in ("stdout", "stderr"):
            need(sha(args.examples_receipt.parent / (case["name"] + "." + stream)) == case[stream + "_sha256"],
                 "native example output receipt differs")
    by_name = {case["name"]: case for case in cases}
    for name, (code, out, err) in expected.items():
        label = name.replace("/", "-")
        for check_name in [label + "-interpret", *(label + f"-o{opt}-execute" for opt in (0, 2, 3))]:
            item = by_name[check_name]
            need(item["exit_code"] == code and item["stdout_sha256"] == hashlib.sha256(out.encode()).hexdigest()
                 and item["stderr_sha256"] == hashlib.sha256(err.encode()).hexdigest(), "example output differs from oracle")
        for opt in (0, 2, 3):
            item = by_name[label + f"-o{opt}-build"]
            need(item["exit_code"] == 0 and item["stdout_sha256"] == hashlib.sha256(b"").hexdigest()
                 and item["stderr_sha256"] == hashlib.sha256(b"").hexdigest(), "example build was not clean")
    ownership = by_name["ownership-negative"]
    ownership_json = load(args.examples_receipt.parent / "ownership-negative.stderr")
    need(ownership["exit_code"] == 1 and ownership["stdout_sha256"] == hashlib.sha256(b"").hexdigest()
         and ownership_json.get("diagnostics") and ownership_json["diagnostics"][0].get("code") == "O007",
         "ownership rejection evidence differs")
    need(by_name["coff-object"]["exit_code"] == 0 and by_name["generic-nominal-tests"]["exit_code"] == 0,
         "COFF/generic test command failed")
    nominal_output = regular(args.examples_receipt.parent / "generic-nominal-tests.stdout", 1 << 20).read_bytes()
    need(nominal_output.endswith(b"test result: 3 passed; 0 failed\n"), "generic nominal test count differs")
    for profile in ("debug", "release"):
        tests = gate.get("tests", {}).get(profile, {})
        need(type(tests.get("passed")) is int and tests["passed"] > 0 and tests.get("failed") == 0,
             f"successful named Windows {profile} test totals required")
    for name in ("fmt", "clippy", "negative", "compiler_fixture", "integration", "regression"):
        check = gate.get("required_checks", {}).get(name, {})
        need(check.get("passed") is True, f"required Windows gate is missing: {name}")
        reference = check.get("receipt", {})
        need(sha(reference.get("path", "")) == reference.get("sha256"), f"gate receipt changed: {name}")
    return identity


def readme(reproduction, runtime_requirements):
    runtime_text = "\n".join("- " + item for item in runtime_requirements)
    return f'''# Axeon {reproduction["compiler_version"]} native Windows x64 candidate

Source: `{reproduction["source_commit"]}`. Target: `{TARGET}`.
This is an internal candidate, not a public release or architecture-completion claim.
The package contains the real compiler, native process supervisor and tensor runtime.
Ordinary compilation uses Windows executables directly and does not invoke WSL.

## Prerequisites

The initial target is Windows10+ x64; execution is qualified only on the host in the
accompanying Evidence receipt. Launching the compiler/helper requires these external
runtime components, based on the actual packaged EXEs:

{runtime_text}

VCRUNTIME140.dll, when listed, is supplied by the Microsoft Visual C++ x64 runtime;
UCRT is the Windows Universal CRT. These launch-time requirements are separate from
the tools needed to generate programs. No Microsoft runtime DLLs are bundled.
The hash-bound direct import tables are in pe-imports.json. They do not enumerate
transitive dependencies, resolve API sets or qualify other Windows installations.

To compile programs, use Visual Studio2022 C++ Build Tools {reproduction["toolchain"]["msvc"]},
Windows SDK {reproduction["toolchain"]["windows_sdk"]}, and LLVM/Clang19.1.7 including lld-link.
These build tools are external dependencies and are not bundled. Use the x64 Native Tools
environment so INCLUDE/LIB and the linker are available; set AXEON_CLANG to your
absolute clang.exe path. No administrator execution is required for normal use.
The toolchain is not self-contained. Rust is required to rebuild Axeon, not to use it.
The currently qualified source workspace and output filesystem is local NTFS.
ReFS/exFAT, UNC/network paths, device namespaces and reparse/symlink source paths
are not supported by this initial Windows identity contract.

From a separate working directory, using the extracted package path:

```powershell
& 'C:\\your package\\bin\\axeon.exe' new demo
& 'C:\\your package\\bin\\axeon.exe' test demo\\tests.ax
& 'C:\\your package\\bin\\axeon.exe' check demo\\main.ax
& 'C:\\your package\\bin\\axeon.exe' build demo\\main.ax -o demo.exe
.\\demo.exe
& 'C:\\your package\\bin\\axeon.exe' run demo\\main.ax
& 'C:\\your package\\bin\\axeon.exe' format demo\\main.ax --check
& 'C:\\your package\\bin\\axeon.exe' explain O007
```

Keep all three bin files together. Generated executables do not need Axeon or WSL,
but require their own imported Windows DLLs/API sets. The accompanying Evidence receipt
records actual hash-bound import tables and execution for installed scalar and tensor
examples; inspect each distributable program separately before making broader claims.
Inspect package.json, source-identity.json, pe-imports.json and SHA256SUMS before use.
Windows installed acceptance and ZIP hashes are recorded separately in the Evidence receipt.
Old Linux validation documents retain their original target scope.

Strings/heap containers, Result/Option library support, public language FFI, GPU/NPU,
concurrency, autodiff, debugger/LSP and other unimplemented architecture features are
not established by this package. Existing bounded CPU semantics remain authoritative.

## Source reproduction and rights

Use the recorded Git source commit/archive and pinned Rust1.85.0; the accompanying
reproduction receipts record two disjoint builds, remapping and exact artifact hashes.
See docs/windows-package-design.md when present, and the recorded reproduction recipe.
The external Evidence receipts carry the executed commands and installation checks.

No project license grant is invented: the repository currently supplies no Axeon
license file. This is an owner-internal candidate; public redistribution requires
the owner's approved license/distribution decision. Rust notices in notices/rust
are copied from the supplied official toolchain notice files. MSVC and LLVM installers,
SDK files and tool binaries are not included. Code signing and public publication
have not been performed.
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("reproduction", "examples-receipt", "examples-source-manifest", "windows-gate",
                 "rust-notices", "output-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--package-dir", type=Path)
    args = parser.parse_args()
    API["native_host"]()
    run = fresh(API["evidence_location"](args.output_dir))
    result = {"schema_version": 1, "status": "running", "target": TARGET, "checks": [],
              "wsl_invoked_by_harness": False, "recipe_sha256": sha(__file__),
              "reproduction_recipe_sha256": sha(Path(__file__).with_name("reproduce-windows.py"))}
    receipt = run / "package.json"
    write_json(receipt, result)
    try:
        reproduction = load(args.reproduction)
        need(reproduction.get("kind") == "axeon-windows-reproduction" and reproduction.get("status") == "passed",
             "successful independent Windows reproduction required")
        refs = reproduction["qualification_receipts"]
        need(isinstance(refs, list) and len(refs) == 2, "two qualification receipts required")
        for item in refs:
            need(sha(item["path"]) == item["sha256"], "qualification receipt changed")
        recalculated = API["comparison"](refs[0]["path"], refs[1]["path"])
        for field in ("source_commit", "archive_sha256", "compiler_version", "artifact_sha256", "native_outputs", "toolchain", "recipe_sha256"):
            need(reproduction[field] == recalculated[field], f"reproduction receipt mismatch: {field}")
        qualified = load(refs[0]["path"])
        build_record = API["validated_build"](qualified["build_receipt"])
        identity = evidence(args, reproduction, build_record)
        candidate = safe(Path("C:/Projects/Axeon/Releases/candidates") / identity["source_commit"] / "windows-x86_64")
        package = safe(args.package_dir or candidate / "package")
        need(package.name == "package" and package.is_relative_to(candidate), "package must be within this source's Windows candidate directory")
        package.parent.mkdir(parents=True, exist_ok=True)
        fresh(package)
        result.update(identity, package=str(package), reproduction_sha256=sha(args.reproduction),
                      windows_gate_sha256=sha(args.windows_gate), examples_receipt_sha256=sha(args.examples_receipt))
        write_json(receipt, result)
        source = Path(build_record["source"])
        payload_inputs = []
        for name in API["BINARIES"]:
            entry = qualified["artifacts"]["bin/" + name]
            payload_inputs.append((regular(entry["path"]), "bin/" + name, entry["sha256"], API["MAX_FILE"]))
        for name, digest in build_record["source_files"].items():
            if name.startswith(("docs/", "examples/")) or name in ("toolchain-lock.json", "runtime/runtime.c", "runtime/runtime.h"):
                payload_inputs.append((regular(source / name, MAX_DOCUMENT), name, digest, MAX_DOCUMENT))
            elif name in ("LICENSE", "LICENSE.md", "LICENSE.txt", "NOTICE", "NOTICE.md", "COPYING"):
                payload_inputs.append((regular(source / name, MAX_DOCUMENT), name, digest, MAX_DOCUMENT))
        notices = {}
        for name in ("COPYRIGHT", "LICENSE-MIT", "LICENSE-APACHE"):
            notice = regular(args.rust_notices / name, MAX_DOCUMENT)
            notices[name] = sha(notice, MAX_DOCUMENT)
            payload_inputs.append((notice, "notices/rust/" + name, notices[name], MAX_DOCUMENT))
        # Admit the complete selected payload before any large copy; reserve room for generated records.
        need(len(payload_inputs) + 5 <= MAX_PAYLOAD_FILES and
             sum(path.stat().st_size for path, _, _, _ in payload_inputs) + (8 << 20) <= MAX_PAYLOAD_BYTES,
             "selected package payload exceeds file/byte budget")
        for path, name, digest, maximum in payload_inputs:
            copy_file(path, package / name, digest, maximum)
        compiler, helper = package / "bin/axeon.exe", package / "bin/axeon-process.exe"
        env = API["native_environment"](build_record["toolchain"]["native_environment"])
        env.pop("AXEON_TENSOR_RUNTIME", None)
        env["AXEON_CLANG"] = reproduction["toolchain"]["tools"]["clang"]["path"]
        need(sha(env["AXEON_CLANG"]) == reproduction["toolchain"]["tools"]["clang"]["sha256"], "installed Clang dependency differs")
        windir = Path(os.environ["SystemRoot"])
        env["PATH"] = os.pathsep.join(map(str, (Path(env["AXEON_CLANG"]).parent,
            Path(reproduction["toolchain"]["tools"]["cl"]["path"]).parent, windir / "System32", windir)))
        inventory = {"schema_version": 1, "status": "running", "source_commit": identity["source_commit"],
                     "target": TARGET, "checks": [], "artifacts": {}}
        result["pe_import_inventory"] = inventory
        write_json(receipt, result)
        reader = regular(Path(env["AXEON_CLANG"]).with_name("llvm-readobj.exe"))
        need(sha(reader) == READOBJ_SHA256, "qualified LLVM19.1.7 PE inspector is missing or differs")
        import_logs = fresh(run / "pe-import-checks")
        inventory["inspector"] = {"path": str(reader), "sha256": READOBJ_SHA256}

        def inspect(command):
            need(sha(reader) == READOBJ_SHA256, "PE inspector changed")
            index = len(inventory["checks"])
            try:
                out, err = API["execute"](helper, command, run, env, import_logs, inventory["checks"], seconds=15)
            finally:
                if len(inventory["checks"]) > index:
                    inventory["checks"][index].update(stdout_path=str(import_logs / f"{index:03d}.stdout"),
                                                      stderr_path=str(import_logs / f"{index:03d}.stderr"))
                write_json(receipt, result)
            need(sha(reader) == READOBJ_SHA256, "PE inspector changed during execution")
            return out, err, inventory["checks"][index]

        def record_import(label, path, expected_hash):
            entry = {"path": str(path), "sha256": expected_hash, "status": "running"}
            inventory["artifacts"][label] = entry
            try:
                entry.update(inspect_pe(path, expected_hash, reader, inspect), status="passed")
                return entry
            except BaseException as error:
                entry.update(status="failed", error=f"{type(error).__name__}: {error}")
                raise
            finally:
                write_json(receipt, result)

        version_out, version_err, _ = inspect([reader, "--version"])
        reader_version = version_out.decode("utf-8")
        need(version_err == b"" and re.search(r"(?m)^\s*LLVM version 19\.1\.7(?:\s|$)", reader_version),
             "LLVM19.1.7 PE inspector version differs")
        inventory["inspector"]["version"] = reader_version
        packaged_imports = {}
        for name, digest in (("axeon.exe", identity["compiler_sha256"]), ("axeon-process.exe", identity["supervisor_sha256"])):
            entry = record_import("package/bin/" + name, package / "bin" / name, digest)
            packaged_imports["bin/" + name] = {**portable_imports(entry),
                "inspection_stdout_sha256": entry["inspection"]["stdout"]["sha256"],
                "inspection_stderr_sha256": entry["inspection"]["stderr"]["sha256"]}
        runtime_requirements = launch_dependencies(packaged_imports)
        write_json(package / "pe-imports.json", {"schema_version": 1, "source_commit": identity["source_commit"],
            "target": TARGET, "inspector": {"name": reader.name, "sha256": READOBJ_SHA256, "version": reader_version},
            "files": packaged_imports, "raw_output_location": "accompanying Evidence receipt: pe-import-checks"})
        write_json(package / "source-identity.json", {"source_commit": identity["source_commit"],
            "archive_sha256": reproduction["archive_sha256"], "source_files": build_record["source_files"],
            "reproduction_sha256": sha(args.reproduction), "toolchain": reproduction["toolchain"]})
        write_json(package / "package.json", {"schema_version": 1, "status": "internal-candidate", **identity,
            "compiler_version": reproduction["compiler_version"], "rust_notice_sha256": notices,
            "dependencies_bundled": False, "code_signed": False, "publicly_released": False,
            "filesystem": "local NTFS; no UNC/device namespaces or reparse source paths",
            "external_dependencies": [*runtime_requirements, "Clang19.1.7 including lld-link", "MSVC2022 x64 C++ Build Tools", "Windows SDK"],
            "launch_dependencies": runtime_requirements, "pe_import_inventory": "pe-imports.json",
            "compile_dependencies": ["Clang19.1.7 including lld-link", "MSVC2022 x64 C++ Build Tools", "Windows SDK"]})
        (package / "README.md").write_text(readme(reproduction, runtime_requirements), encoding="utf-8", newline="\n")
        manifest = checksums(package)
        with (package / "SHA256SUMS").open("x", encoding="utf-8", newline="\n") as stream:
            for name, digest in manifest.items():
                stream.write(f"{digest}  {name}\n")
        archive = package.parent / f"axeon-{reproduction['compiler_version']}-windows-x86_64.zip"
        with archive.open("xb") as target:
            archive_sha, archive_bytes = write_zip(package, target)
        need((archive_sha, archive_bytes) == write_zip(package), "deterministic ZIP byte replay differs")
        need(sha(archive) == archive_sha, "written ZIP hash differs")
        result.update(archive=str(archive), archive_sha256=archive_sha, archive_bytes=archive_bytes,
                      deterministic_zip_replay=True, checksum_entries=len(manifest))
        installation = run / "installed package 雪"
        extract_zip(archive, installation, archive_sha)
        workspace = fresh(run / "sample workspace café")
        logs = fresh(run / "installed-checks")
        compiler, helper = installation / "bin/axeon.exe", installation / "bin/axeon-process.exe"
        for name in ("axeon.exe", "axeon-process.exe"):
            expected_imports = packaged_imports["bin/" + name]
            entry = record_import("installed/bin/" + name, installation / "bin" / name, expected_imports["sha256"])
            need(portable_imports(entry) == portable_imports(expected_imports), "installed PE import inventory differs")
        def check(command, expected=None):
            try:
                return API["execute"](helper, command, workspace, env, logs, result["checks"],
                                      seconds=45, expected=expected)
            finally:
                write_json(receipt, result)
        version = check([compiler, "--version"])[0].decode("utf-8")
        need(version.startswith("axeon " + reproduction["compiler_version"] + ";"), "installed compiler version differs")
        check([compiler, "new", "demo"])
        out = check([compiler, "test", "demo/tests.ax"])[0]
        need(out.endswith(b"test result: 2 passed; 0 failed\n"), "installed project tests differ")
        check([compiler, "check", "demo/main.ax"], (0, b"", b""))
        check([compiler, "format", "demo/main.ax", "--check"], (0, b"", b""))
        expected = (0, b"42\nresult: 0\n", b"")
        check([compiler, "build", "demo/main.ax", "-o", "demo.exe"], (0, b"", b""))
        record_import("installed-programs/scalar", workspace / "demo.exe", sha(workspace / "demo.exe"))
        check([workspace / "demo.exe"], expected)
        check([compiler, "run", "demo/main.ax"], expected)
        before = sha(workspace / "demo.exe")
        metadata_before = sha(workspace / "demo.exe.json")
        (workspace / "invalid.ax").write_text("fn main()->i64{return true;}\n", encoding="utf-8", newline="\n")
        # Negative commands use the same supervised path while admitting their expected status.
        error_logs = fresh(run / "invalid-check")
        negative_checks = []
        try:
            API["execute"](helper, [compiler, "build", "invalid.ax", "-o", "demo.exe", "--diagnostic-format", "json"],
                           workspace, env, error_logs, negative_checks, seconds=45)
        except RuntimeError:
            need(negative_checks and negative_checks[0].get("exit_code") == 1, "invalid source must fail with status1")
            invalid_out = (error_logs / "000.stdout").read_bytes()
            diagnostics = json.loads((error_logs / "000.stderr").read_bytes())
            need(invalid_out == b"" and diagnostics.get("diagnostics"), "invalid source diagnostics missing")
            need(sha(workspace / "demo.exe") == before, "failed compilation changed existing installed output")
            need(sha(workspace / "demo.exe.json") == metadata_before, "failed compilation changed existing installed metadata")
            negative_checks[0]["passed"] = True
        else:
            raise RuntimeError("invalid source unexpectedly compiled")
        result["negative_checks"] = negative_checks
        generic = installation / "examples/generic-nominals/main.ax"
        check([compiler, "build", generic, "-o", "generic.exe"], (0, b"", b""))
        check([workspace / "generic.exe"], (0, b"42\n-9223372036854775808\nresult: 0\n", b""))
        tensor = installation / "examples/tensors-recovery.ax"
        check([compiler, "build", tensor, "-o", "tensor.exe"], (0, b"", b""))
        record_import("installed-programs/tensor", workspace / "tensor.exe", sha(workspace / "tensor.exe"))
        check([workspace / "tensor.exe"], (0, b"1\n9\n7\n1\n2\nresult: 7\n", b""))
        check([compiler, "run", tensor], (0, b"1\n9\n7\n1\n2\nresult: 7\n", b""))
        verify_manifest(installation)
        verify_manifest(package)
        need(sha(archive) == archive_sha, "ZIP changed during installed qualification")
        need(sha(reader) == READOBJ_SHA256, "PE inspector changed during qualification")
        for entry in inventory["artifacts"].values():
            need(entry["status"] == "passed" and sha(entry["path"]) == entry["sha256"], "inspected PE changed during qualification")
        for check in inventory["checks"]:
            for stream in ("stdout", "stderr"):
                need(sha(check[stream + "_path"]) == check[stream + "_sha256"], "raw PE inspection evidence changed")
        inventory["status"] = "passed"
        result.update(status="passed", installed_path=str(installation), installed_checks=len(result["checks"]),
                      failures=0, package_accepted=False, source_checkout_needed_for_installed_use=False)
    except BaseException as error:
        result.update(status="failed", error=f"{type(error).__name__}: {error}")
        if "pe_import_inventory" in result:
            result["pe_import_inventory"]["status"] = "failed"
        raise
    finally:
        write_json(receipt, result)
    print(json.dumps({"status": result["status"], "package": result["package"], "receipt": str(receipt)}))


if __name__ == "__main__":
    main()

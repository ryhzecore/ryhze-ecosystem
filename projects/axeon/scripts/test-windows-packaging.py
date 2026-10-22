#!/usr/bin/env python3
"""Synthetic unit checks for Windows packaging safeguards, never qualification.

Run under the admitted Windows guard with a fresh --output-dir below a reserved
dated Evidence attempt. No compiler, subprocess or accepted package is accessed.
Every fixture, deliberately corrupted input and failure record is retained.
"""
import argparse
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import runpy
import sys
import tarfile
import traceback
from unittest.mock import patch


HERE = Path(__file__).absolute().parent
REPRO = runpy.run_path(str(HERE / "reproduce-windows.py"))
PACKAGE = runpy.run_path(str(HERE / "package-windows.py"))
COMMIT = "1" * 40


def require(value, message):
    if not value:
        raise AssertionError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def put(path, data):
    REPRO["safe_path"](path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(data)
    return path


def reject(action, exception, message):
    try:
        action()
    except exception as error:
        require(message in str(error), f"wrong rejection: {type(error).__name__}: {error}")
        return {"exception": type(error).__name__, "message": str(error)}
    raise AssertionError("unsafe or inconsistent fixture was accepted")


def environment_isolation(case):
    injected = {"PATH": str(case / "ambient-bin"), "INCLUDE": "ambient-include", "LIB": "ambient-lib",
                "LIBPATH": "ambient-libpath", "UNRELATED_TEST_SENTINEL": "retained",
                "CL": "/DUNRECORDED=1", "_CL_": "/Od /MD", "LINK": "/DEBUG", "_LINK_": "/INCREMENTAL"}
    pinned = {name: None for name in REPRO["NATIVE_ENVIRONMENT"]}
    pinned.update(PATH=str(case / "pinned tools 雪"), INCLUDE=str(case / "pinned include café"),
                  LIB=str(case / "pinned libraries 雪"), WINDOWSSDKVERSION="synthetic-sdk",
                  VCTOOLSVERSION="synthetic-toolset")
    with patch.dict(os.environ, injected, clear=True):
        built = REPRO["environment"](case / "build", case / "source", case / "clang.exe", case / "rustc.exe")
        qualified = REPRO["native_environment"](pinned)
        require(all(name not in built and name not in qualified for name in ("CL", "_CL_", "LINK", "_LINK_")),
                "MSVC ambient argument overrides survived")
        require(built["INCLUDE"] == "ambient-include" and built["LIB"] == "ambient-lib",
                "build discarded its declared native dependency environment")
        require(qualified["INCLUDE"] == pinned["INCLUDE"] and qualified["LIB"] == pinned["LIB"]
                and qualified["PATH"] == pinned["PATH"] and "LIBPATH" not in qualified,
                "qualification did not restore pinned values and remove absent optional values")
        require(qualified["UNRELATED_TEST_SENTINEL"] == "retained", "unrelated environment setting changed")
        incomplete = dict(pinned)
        incomplete.pop("LIB")
        failure = reject(lambda: REPRO["native_environment"](incomplete), RuntimeError,
                         "complete pinned native dependency environment required")
    return {"synthetic_pinned_environment": pinned, "incomplete_pin_rejection": failure}


def qualification_fixture(case):
    root = REPRO["fresh"](case / "qualification 雪")
    source = REPRO["fresh"](case / "source café")
    source_file = put(source / "examples" / "sample.ax", b"SYNTHETIC UNIT FIXTURE; NOT LANGUAGE SOURCE\n")
    binaries = {}
    for name in ("axeon.exe", "axeon-process.exe", "axeon_tensor_runtime.lib"):
        data = ("SYNTHETIC; NOT AN EXECUTABLE OR RUNTIME: " + name + "\n").encode()
        path = put(case / "binaries" / name, data)
        binaries["bin/" + name] = {"path": str(path), "sha256": digest(data)}
    build = {"source": str(source), "artifacts": binaries, "synthetic_unit_fixture": True}
    item = {"root": str(root), "artifacts": copy.deepcopy(binaries), "example_count": 1,
            "checks": [], "synthetic_unit_fixture": True, "native_qualification": False}
    compiler, supervisor = binaries["bin/axeon.exe"]["path"], binaries["bin/axeon-process.exe"]["path"]
    expected = {"sample": {"exit_code": 70, "stdout": "42\n", "stderr": "synthetic trap\n"}}
    object_path, executable_path = root / "sample.obj", root / "sample.exe"
    # This independently authored fixture describes the external four-command contract.
    commands = [
        ([compiler, "interpret", str(source_file)], 70, b"42\n", b"synthetic trap\n"),
        ([compiler, "emit-object", str(source_file), "--opt", "2", "-o", str(object_path)], 0, b"", b""),
        ([compiler, "build", str(source_file), "--opt", "2", "-o", str(executable_path)], 0, b"", b""),
        ([str(executable_path)], 70, b"42\n", b"synthetic trap\n"),
    ]
    for path in (object_path, executable_path, Path(str(object_path) + ".json"), Path(str(executable_path) + ".json")):
        data = b"SYNTHETIC ARTIFACT; NEVER EXECUTE\n"
        put(path, data)
        item["artifacts"][path.name] = {"path": str(path), "sha256": digest(data)}
    for index, (command, code, out, err) in enumerate(commands):
        put(root / "logs" / f"{index:03d}.stdout", out)
        put(root / "logs" / f"{index:03d}.stderr", err)
        item["checks"].append({"command": command, "cwd": str(source), "supervisor": supervisor,
                               "seconds_limit": 45, "passed": True, "exit_code": code,
                               "stdout_sha256": digest(out), "stderr_sha256": digest(err)})
    receipt = root / "fixture.json"
    REPRO["write_json"](receipt, item)
    REPRO["verify_qualification_execution"](receipt, item, build, expected)
    return receipt, item, build, expected


def qualification_rejection(case, mutation):
    receipt, item, build, expected = qualification_fixture(case)
    exception, message = RuntimeError, "qualification output receipt changed"
    if mutation in ("changed-log", "missing-log"):
        log = receipt.parent / "logs" / "003.stderr"
        log.rename(log.with_name(log.name + ".saved-original"))
        if mutation == "changed-log":
            put(log, b"altered output after qualification\n")
        else:
            exception, message = FileNotFoundError, "003.stderr"
    elif mutation == "swapped-binary":
        original = Path(build["artifacts"]["bin/axeon.exe"]["path"])
        alternative = put(case / "unrelated-build" / "axeon.exe", original.read_bytes())
        item["artifacts"]["bin/axeon.exe"]["path"] = str(alternative)
        message = "qualification binary does not match its build"
    elif mutation == "changed-command":
        item["checks"][1]["command"][4] = "3"
        message = "qualification command differs"
    else:
        raise AssertionError("unknown qualification mutation")
    receipt.rename(receipt.with_name("baseline-fixture.json"))
    REPRO["write_json"](receipt, item)
    return reject(lambda: REPRO["verify_qualification_execution"](receipt, item, build, expected), exception, message)


SOURCE_PAYLOAD = {
    "Cargo.toml": b"# SYNTHETIC UNIT FIXTURE\n",
    "Cargo.lock": b"# SYNTHETIC UNIT FIXTURE\n",
    "src/main.rs": b"// SYNTHETIC UNIT FIXTURE\n",
    "runtime/windows_process.c": b"/* SYNTHETIC UNIT FIXTURE */\n",
    "scripts/record-native-examples.py": b"# SYNTHETIC UNIT FIXTURE\n",
    "examples/generic-nominals/main.ax": b"// SYNTHETIC UNIT FIXTURE\n",
    "docs/说明 雪.txt": "synthetic Unicode archive fixture\r\n".encode(),
}


def source_archive(case, additions=(), comment=COMMIT):
    path = case / "synthetic-source.tar"
    with path.open("xb") as stream, tarfile.open(fileobj=stream, mode="w", format=tarfile.PAX_FORMAT,
                                               pax_headers={"comment": comment}) as archive:
        for name, data in SOURCE_PAYLOAD.items():
            member = tarfile.TarInfo(name)
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
        for member, data in additions:
            archive.addfile(member, io.BytesIO(data) if member.isfile() else None)
    return path


def archive_acceptance(case):
    archive = source_archive(case)
    expected = {name: digest(data) for name, data in SOURCE_PAYLOAD.items()}
    files = REPRO["extract_archive"](archive, digest(archive.read_bytes()), COMMIT, case / "source")
    require(files == expected, "archive extraction changed bytes or omitted selected inputs")
    REPRO["verify_source"](case / "source", expected)
    return {"synthetic_archive_sha256": digest(archive.read_bytes()), "selected_files": len(files)}


def archive_rejection(case, mutation):
    additions, comment = [], COMMIT
    if mutation == "commit":
        comment = "2" * 40
        message = "full commit in the global PAX comment"
    else:
        name = {"traversal": "../escaped.ax", "case-alias": "SRC/main.rs"}.get(mutation, "src/nonregular.rs")
        member = tarfile.TarInfo(name)
        data = b"SYNTHETIC HOSTILE ARCHIVE MEMBER\n"
        if mutation in ("symlink", "hardlink", "fifo"):
            member.type = {"symlink": tarfile.SYMTYPE, "hardlink": tarfile.LNKTYPE, "fifo": tarfile.FIFOTYPE}[mutation]
            member.linkname = "../../escaped.ax" if mutation == "symlink" else "src/main.rs"
            message = "nonregular archive member"
        else:
            member.size = len(data)
            message = "unsafe Windows name" if mutation == "traversal" else "duplicate/case-aliased archive member"
        additions.append((member, data))
    archive = source_archive(case, additions, comment)
    rejection = reject(lambda: REPRO["extract_archive"](archive, digest(archive.read_bytes()), COMMIT, case / "source"),
                       RuntimeError, message)
    require(not (case / "escaped.ax").exists(), "archive wrote outside its source destination")
    return rejection


def package_fixture(case):
    package = REPRO["fresh"](case / "synthetic package 雪")
    payload = {"bin/never-execute.txt": b"SYNTHETIC UNIT PAYLOAD; NOT A COMPILER\n",
               "docs/説明 café.txt": "synthetic text\r\n雪\n".encode()}
    for name, data in payload.items():
        put(package / name, data)
    sums = "".join(f"{digest(data)}  {name}\n" for name, data in sorted(payload.items()))
    put(package / "SHA256SUMS", sums.encode())
    PACKAGE["verify_manifest"](package)
    return package


def zip_determinism(case):
    package = package_fixture(case)
    first, second = case / "first.zip", case / "second.zip"
    with first.open("xb") as target:
        first_digest, first_size = PACKAGE["write_zip"](package, target)
    os.utime(package / "docs/説明 café.txt", (946684800, 946684800))
    with second.open("xb") as target:
        second_digest, second_size = PACKAGE["write_zip"](package, target)
    require(first.read_bytes() == second.read_bytes(), "ZIP bytes changed with source timestamps")
    require((first_digest, first_size) == (second_digest, second_size) == PACKAGE["write_zip"](package),
            "ZIP streaming replay differs")
    require(digest(first.read_bytes()) == first_digest and first.stat().st_size == first_size,
            "ZIP byte/hash receipt differs from independent check")
    PACKAGE["extract_zip"](first, case / "installed fixture 雪", first_digest)
    return {"zip_sha256": first_digest, "zip_bytes": first_size, "synthetic_payload_only": True}


def checksum_rejection(case, mutation):
    package = package_fixture(case)
    target = package / ("SHA256SUMS" if mutation == "manifest" else "bin/never-execute.txt")
    original = target.read_bytes()
    put(case / "saved-original.bin", original)
    target.write_bytes((b"0" * 64 + original[64:]) if mutation == "manifest" else b"tampered synthetic payload\n")
    manifest = reject(lambda: PACKAGE["verify_manifest"](package), RuntimeError, "package SHA256SUMS differs")
    archive = reject(lambda: PACKAGE["write_zip"](package), RuntimeError, "package SHA256SUMS differs")
    return {"manifest_rejection": manifest, "archive_generation_rejection": archive}


def zip_hash_rejection(case):
    package = package_fixture(case)
    archive = case / "baseline.zip"
    with archive.open("xb") as target:
        expected, _ = PACKAGE["write_zip"](package, target)
    changed = bytearray(archive.read_bytes())
    changed[len(changed) // 2] ^= 1
    altered = put(case / "tampered.zip", changed)
    destination = case / "must-not-be-created"
    result = reject(lambda: PACKAGE["extract_zip"](altered, destination, expected), RuntimeError,
                    "candidate ZIP changed before extraction")
    require(not destination.exists(), "changed archive created an installation directory")
    return result


PE_IMPORT_FIXTURE = """File: C:\\synthetic package\\bin\\axeon.exe
Format: COFF-x86-64
Arch: x86_64
AddressSize: 64bit
ImageFileHeader {
  Machine: IMAGE_FILE_MACHINE_AMD64 (0x8664)
  SectionCount: 6
  TimeDateStamp: 1970-01-01 00:00:00 (0x0)
  PointerToSymbolTable: 0x0
  SymbolCount: 0
  StringTableSize: 0
  OptionalHeaderSize: 240
  Characteristics [ (0x22)
    IMAGE_FILE_EXECUTABLE_IMAGE (0x2)
    IMAGE_FILE_LARGE_ADDRESS_AWARE (0x20)
  ]
}
ImageOptionalHeader {
  Magic: 0x20B
  MajorLinkerVersion: 14
  MinorLinkerVersion: 0
  AddressOfEntryPoint: 0x1000
  BaseOfCode: 0x1000
  ImageBase: 0x140000000
  SectionAlignment: 4096
  FileAlignment: 512
  MajorOperatingSystemVersion: 6
  MinorOperatingSystemVersion: 0
  SizeOfImage: 65536
  SizeOfHeaders: 1024
  Subsystem: IMAGE_SUBSYSTEM_WINDOWS_CUI (0x3)
  NumberOfRvaAndSize: 16
  DataDirectory {
    ExportTableRVA: 0x0
    ExportTableSize: 0x0
    ImportTableRVA: 0x7000
    ImportTableSize: 0x8C
    IATRVA: 0x8000
    IATSize: 0xC0
    DelayImportDescriptorRVA: 0x0
    DelayImportDescriptorSize: 0x0
  }
}
Import {
  Name: KERNEL32.dll
  ImportLookupTableRVA: 0x7100
  ImportAddressTableRVA: 0x8000
  Symbol: GetLastError (640)
}
Import {
  Name: VCRUNTIME140.dll
  ImportLookupTableRVA: 0x7120
  ImportAddressTableRVA: 0x8020
  Symbol: __C_specific_handler (8)
}
Import {
  Name: VCRUNTIME140_1.dll
  ImportLookupTableRVA: 0x7140
  ImportAddressTableRVA: 0x8040
  Symbol: __CxxFrameHandler4 (0)
}
Import {
  Name: MSVCP140.dll
  ImportLookupTableRVA: 0x7160
  ImportAddressTableRVA: 0x8060
  Symbol: _Cnd_destroy (386)
}
Import {
  Name: api-ms-win-crt-runtime-l1-1-0.dll
  ImportLookupTableRVA: 0x7180
  ImportAddressTableRVA: 0x8080
  Symbol: exit (85)
}
Import {
  Name: ntdll.dll
  ImportLookupTableRVA: 0x71A0
  ImportAddressTableRVA: 0x80A0
  Symbol: NtClose (300)
}
"""


def pe_import_acceptance(case):
    # Independently authored reader output, not generated by the parser under test.
    expected_names = ["KERNEL32.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll", "MSVCP140.dll",
                      "api-ms-win-crt-runtime-l1-1-0.dll", "ntdll.dll"]
    expected_requirements = [
        "Microsoft Visual C++ x64 runtime providing MSVCP140.dll, VCRUNTIME140.dll, VCRUNTIME140_1.dll",
        "Windows Universal CRT (UCRT)",
        "All direct DLL/API-set dependencies listed in pe-imports.json",
    ]
    inventories = []
    for label, text in (("lf", PE_IMPORT_FIXTURE), ("crlf", PE_IMPORT_FIXTURE.replace("\n", "\r\n"))):
        output = put(case / (label + ".stdout"), text.encode("utf-8"))
        inventory = PACKAGE["parse_pe_imports"](output.read_bytes())
        require(inventory["direct_dll_imports"] == expected_names, "PE inventory lost or changed a direct import")
        require(inventory["architecture"] == "x86_64" and inventory["format"] == "COFF-x86-64"
                and inventory["delayed_dll_imports"] == [], "PE inventory describes another target or delay imports")
        require(inventory["scope"] == "direct PE import tables; not recursive DLL resolution",
                "PE inventory overstates its dependency-resolution scope")
        inventories.append(inventory)
    require(inventories[0] == inventories[1], "Windows reader line endings changed the import inventory")
    requirements = PACKAGE["launch_dependencies"]({"bin/axeon.exe": inventories[0],
                                                 "bin/axeon-process.exe": inventories[1]})
    require(requirements == expected_requirements, "launch requirements omit or duplicate an imported runtime")
    readme = PACKAGE["readme"]({"compiler_version": "synthetic", "source_commit": COMMIT,
                                "toolchain": {"msvc": "synthetic", "windows_sdk": "synthetic"}}, requirements)
    for requirement in expected_requirements:
        require("- " + requirement + "\n" in readme, "README omitted an imported launch-time dependency")
    put(case / "synthetic-readme.md", readme.encode("utf-8"))
    return {"inventory": inventories[0], "launch_dependencies": requirements, "native_qualification": False}


def pe_dependency_control(case, ucrt):
    # System-only executables must not acquire an invented Visual C++ requirement;
    # a direct ucrtbase.dll import must disclose UCRT just like its API-set form.
    header = PE_IMPORT_FIXTURE.split("Import {", 1)[0]
    text = header + "Import {\n  Name: KERNEL32.dll\n  Symbol: GetLastError (640)\n}\n"
    expected = ["All direct DLL/API-set dependencies listed in pe-imports.json"]
    if ucrt:
        text += "Import {\n  Name: ucrtbase.dll\n  Symbol: exit (85)\n}\n"
        expected.insert(0, "Windows Universal CRT (UCRT)")
    output = put(case / "readobj.stdout", text.encode("utf-8"))
    inventory = PACKAGE["parse_pe_imports"](output.read_bytes())
    requirements = PACKAGE["launch_dependencies"]({"bin/synthetic.exe": inventory})
    require(requirements == expected, "launch requirements misclassify system or direct UCRT imports")
    return {"inventory": inventory, "launch_dependencies": requirements}


def pe_import_rejection(case, mutation):
    replacements = {
        "machine": ("IMAGE_FILE_MACHINE_AMD64 (0x8664)", "IMAGE_FILE_MACHINE_ARM64 (0xAA64)",
                    "unsupported PE machine"),
        "architecture": ("Arch: x86_64", "Arch: aarch64", "unsupported PE inspection Arch"),
        "missing-delay-rva": ("    DelayImportDescriptorRVA: 0x0\n", "",
                              "missing or unsupported delayed PE import inventory"),
        "missing-delay-size": ("    DelayImportDescriptorSize: 0x0\n", "",
                               "missing or unsupported delayed PE import inventory"),
        "nonnull-delay-rva": ("DelayImportDescriptorRVA: 0x0", "DelayImportDescriptorRVA: 0x9000",
                              "missing or unsupported delayed PE import inventory"),
        "nonnull-delay-size": ("DelayImportDescriptorSize: 0x0", "DelayImportDescriptorSize: 0x40",
                               "missing or unsupported delayed PE import inventory"),
        "dll-name": ("Name: VCRUNTIME140.dll", "Name: VCRUNTIME140.dll extra",
                     "missing or unsupported PE import DLL name"),
        "dll-path": ("Name: VCRUNTIME140.dll", "Name: ../VCRUNTIME140.dll",
                     "missing or unsupported PE import DLL name"),
    }
    if mutation in replacements:
        old, new, message = replacements[mutation]
        require(PE_IMPORT_FIXTURE.count(old) == 1, "fixture mutation is not unique")
        text = PE_IMPORT_FIXTURE.replace(old, new)
    elif mutation == "delay-block":
        text = PE_IMPORT_FIXTURE + "DelayImport {\n  Name: delayed.dll\n}\n"
        message = "unsupported delayed PE imports"
    elif mutation == "duplicate-dll":
        text = PE_IMPORT_FIXTURE + "Import {\n  Name: vcruntime140.DLL\n  Symbol: memcpy (55)\n}\n"
        message = "duplicate PE import DLL name"
    elif mutation == "unterminated-import":
        text = PE_IMPORT_FIXTURE.removesuffix("}\n")
        message = "missing, malformed or excessive PE import blocks"
    else:
        raise AssertionError("unknown PE inspection mutation")
    output = put(case / "readobj.stdout", text.encode("utf-8"))
    return reject(lambda: PACKAGE["parse_pe_imports"](output.read_bytes()), RuntimeError, message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    REPRO["native_host"]()
    root = REPRO["fresh"](REPRO["evidence_location"](args.output_dir))
    recipe_paths = (Path(__file__).absolute(), HERE / "reproduce-windows.py", HERE / "package-windows.py")
    recipe_hashes = {str(path): REPRO["sha"](path) for path in recipe_paths}
    result = {"schema_version": 1, "kind": "axeon-windows-packaging-synthetic-unit-tests", "status": "running",
              "native_qualification": False, "compiler_executed": False, "subprocesses_started": 0,
              "accepted_packages_accessed": False, "fixtures_preserved": True, "checks": [],
              "recipe_sha256": recipe_hashes}
    receipt = root / "packaging-tests.json"
    REPRO["write_json"](receipt, result)
    cases = [("environment-isolation", environment_isolation)]
    cases += [("qualification-" + value, lambda case, value=value: qualification_rejection(case, value))
              for value in ("changed-log", "missing-log", "swapped-binary", "changed-command")]
    cases += [("archive-valid-control", archive_acceptance)]
    cases += [("archive-rejects-" + value, lambda case, value=value: archive_rejection(case, value))
              for value in ("traversal", "case-alias", "symlink", "hardlink", "fifo", "commit")]
    cases += [("zip-deterministic-control", zip_determinism),
              ("checksum-payload-tamper", lambda case: checksum_rejection(case, "payload")),
              ("checksum-manifest-tamper", lambda case: checksum_rejection(case, "manifest")),
              ("zip-hash-tamper", zip_hash_rejection)]
    cases += [("pe-import-valid-control", pe_import_acceptance),
              ("pe-system-dependencies-control", lambda case: pe_dependency_control(case, False)),
              ("pe-ucrtbase-dependencies-control", lambda case: pe_dependency_control(case, True))]
    cases += [("pe-rejects-" + value, lambda case, value=value: pe_import_rejection(case, value))
              for value in ("machine", "architecture", "missing-delay-rva", "missing-delay-size",
                            "nonnull-delay-rva", "nonnull-delay-size", "delay-block", "dll-name", "dll-path",
                            "duplicate-dll", "unterminated-import")]
    with patch("subprocess.Popen", side_effect=AssertionError("synthetic unit fixtures must not launch subprocesses")):
        for name, test in cases:
            case = REPRO["fresh"](root / name)
            check = {"name": name, "fixture": str(case), "passed": False}
            result["checks"].append(check)
            try:
                check["details"] = test(case)
                check["passed"] = True
            except Exception as error:
                check["error"] = f"{type(error).__name__}: {error}"
                put(case / "failure.txt", traceback.format_exc().encode())
            REPRO["write_json"](case / "case-result.json", check)
            REPRO["write_json"](receipt, result)
    result["passed"] = sum(check["passed"] for check in result["checks"])
    result["failed"] = len(cases) - result["passed"]
    result["recipes_unchanged"] = all(REPRO["sha"](path) == recipe_hashes[str(path)] for path in recipe_paths)
    result["status"] = "passed" if result["failed"] == 0 and result["recipes_unchanged"] else "failed"
    REPRO["write_json"](receipt, result)
    print(json.dumps({"status": result["status"], "synthetic_unit_tests": True, "receipt": str(receipt),
                      "passed": result["passed"], "failed": result["failed"]}))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())

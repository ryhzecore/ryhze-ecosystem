#!/usr/bin/env python3
"""Synthetic committed-archive validation regressions; no native qualification.

Use the admitted Windows guard and a fresh --output-dir below a reserved dated
Evidence attempt. Fixtures are synthetic TAR/JSON files, never real compiler or
accepted-package inputs. The adapter is invoked in-process in --validate-only
mode. Subprocesses, native destination discovery, mkdir and writes are forbidden
during each dry call. Every fixture, rejection and failure log is preserved.
"""
import argparse
import contextlib
import hashlib
import io
import json
from pathlib import Path
import runpy
import sys
import tarfile
import traceback
from unittest.mock import patch


HERE = Path(__file__).absolute().parent
CAPTURE = runpy.run_path(str(HERE / "capture-linux-archive.py"))
LAYOUT = runpy.run_path(str(HERE / "reproduce-windows.py"))
COMMIT = "1" * 40
PAYLOAD = {
    ".gitattributes": (b"*.rs text eol=lf\n", "100644"),
    "src/main.rs": (b"// SYNTHETIC SOURCE; never compiled\n", "100644"),
    "scripts/verify.sh": (b"#!/bin/sh\n# SYNTHETIC; never executed\n", "100755"),
    "tests/fixture.rs": (b"// Preserved synthetic regression source\n", "100644"),
    "numerics-tests/fixtures.json": (b'{"synthetic":true,"value":"-0.0"}\n', "100644"),
    "tools/profiler/Cargo.toml": (b"# Synthetic tool input\n", "100644"),
    "docs/guide.txt": ("Synthetic UTF-8 source: caf\u00e9 \u96ea\n".encode(), "100644"),
}


def require(value, message):
    if not value:
        raise AssertionError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def put(path, data):
    LAYOUT["safe_path"](path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(data)
    return path


def put_json(path, value):
    return put(path, (json.dumps(value, indent=2, ensure_ascii=True) + "\n").encode())


def inventory(root):
    entries = {}
    for path in sorted(root.rglob("*")):
        LAYOUT["safe_path"](path)
        name = path.relative_to(root).as_posix()
        entries[name] = {"directory": True} if path.is_dir() else {
            "bytes": path.stat().st_size, "sha256": LAYOUT["sha"](path)}
    return entries


def fixture(case, mutation):
    """Author independent TAR headers and declared hashes; never extract them."""
    members = [(name, data, int(mode[-3:], 8), tarfile.REGTYPE, "") for name, (data, mode) in PAYLOAD.items()]
    comment = "2" * 40 if mutation == "wrong-pax-commit" else COMMIT
    if mutation == "wrong-mode":
        members[1] = (members[1][0], members[1][1], 0o755, tarfile.REGTYPE, "")
    elif mutation == "special-permission-bits":
        members[1] = (members[1][0], members[1][1], 0o4644, tarfile.REGTYPE, "")
    elif mutation == "git-umask-control":
        members[1] = (members[1][0], members[1][1], 0o664, tarfile.REGTYPE, "")
    elif mutation == "wrong-file-size":
        members[1] = (members[1][0], b"x", members[1][2], tarfile.REGTYPE, "")
    elif mutation == "missing-file":
        members.pop()
    additions = {
        "traversal": ("../outside.txt", b"untrusted\n", 0o644, tarfile.REGTYPE, ""),
        "absolute-path": ("/outside.txt", b"untrusted\n", 0o644, tarfile.REGTYPE, ""),
        "ancestor-case-alias": ("DOCS/other.txt", b"untrusted\n", 0o644, tarfile.REGTYPE, ""),
        "file-directory-collision": ("src", b"untrusted\n", 0o644, tarfile.REGTYPE, ""),
        "duplicate-member": members[1],
        "symlink": ("unsafe-link", b"", 0o777, tarfile.SYMTYPE, "../outside.txt"),
        "hardlink": ("unsafe-link", b"", 0o644, tarfile.LNKTYPE, "src/main.rs"),
        "fifo": ("unsafe-fifo", b"", 0o644, tarfile.FIFOTYPE, ""),
        "character-device": ("unsafe-device", b"", 0o644, tarfile.CHRTYPE, ""),
    }
    if mutation in additions:
        members.append(additions[mutation])
    archive = case / "synthetic-source.tar"
    with archive.open("xb") as target, tarfile.open(fileobj=target, mode="w", format=tarfile.PAX_FORMAT,
                                                   pax_headers={"comment": comment}) as packed:
        for name, data, mode, kind, link in members:
            info = tarfile.TarInfo(name)
            info.mode, info.type, info.linkname, info.mtime = mode, kind, link, 0
            info.uid, info.gid, info.uname, info.gname = 0, 0, "", ""
            info.size = len(data) if kind == tarfile.REGTYPE else 0
            packed.addfile(info, io.BytesIO(data) if kind == tarfile.REGTYPE else None)
    if mutation == "trailing-nonzero":
        # This is deliberately part of the synthetic hostile archive, not a
        # modification of an existing receipt or owner-provided source archive.
        with archive.open("ab") as stream:
            stream.write(b"X" + b"\0" * 511)
    actual_sha = LAYOUT["sha"](archive)
    archive_sha = "0" * 64 if mutation == "wrong-archive-sha" else actual_sha
    entries = [{"path": name, "bytes": len(data), "sha256": sha(data), "mode": mode}
               for name, (data, mode) in PAYLOAD.items()]
    if mutation == "wrong-file-hash":
        entries[1]["sha256"] = "0" * 64
    if mutation == "manifest-case-alias":
        entries.append({"path": "DOCS/other.txt", "bytes": 1, "sha256": sha(b"x"), "mode": "100644"})
    external_modes = mutation in ("external-modes-control", "wrong-mode-binding", "wrong-external-mode")
    mode_entries = [{"path": item["path"], "mode": item["mode"]} for item in entries]
    if external_modes or mutation == "missing-modes":
        for item in entries:
            item.pop("mode")
    manifest = {"commit": "2" * 40 if mutation == "wrong-manifest-commit" else COMMIT,
                "archive_sha256": archive_sha, "source": "SYNTHETIC: no real source directory",
                "files": entries}
    manifest_path = put_json(case / "source-manifest.json", manifest)
    mode_path = None
    if external_modes:
        if mutation == "wrong-external-mode":
            mode_entries[1]["mode"] = "100755"
        mode_path = put_json(case / "git-modes.json", {"source_commit": COMMIT,
            "source_manifest_sha256": "0" * 64 if mutation == "wrong-mode-binding" else LAYOUT["sha"](manifest_path),
            "files": mode_entries})
    return {"archive": archive, "archive_sha": archive_sha, "actual_archive_sha": actual_sha,
            "manifest": manifest_path, "modes": mode_path}


def dry_call(case, inputs):
    arguments = [str(HERE / "capture-linux-archive.py"), "--validate-only", "--archive", str(inputs["archive"]),
                 "--archive-sha256", inputs["archive_sha"], "--source-commit", COMMIT,
                 "--source-manifest", str(inputs["manifest"]), "--name", "generic-nominals-99"]
    if inputs["modes"] is not None:
        arguments.extend(["--mode-manifest", str(inputs["modes"])])
    before = inventory(case)
    captured, stderr = io.StringIO(), io.StringIO()
    original_open = Path.open
    def readonly_open(path, mode="r", *args, **kwargs):
        require(not any(flag in mode for flag in "wax+"), f"dry validation tried to write {path}")
        return original_open(path, mode, *args, **kwargs)
    def forbidden(*args, **kwargs):
        raise AssertionError("dry validation attempted native discovery, extraction, directory creation or receipt writing")
    globals_ = CAPTURE["main"].__globals__
    with patch.object(sys, "argv", arguments), contextlib.redirect_stdout(captured), contextlib.redirect_stderr(stderr), \
            patch.object(Path, "open", readonly_open), patch.object(Path, "mkdir", forbidden), \
            patch.dict(globals_, {"native_destination": forbidden, "write_new": forbidden,
                                  "BASE": case / "must-not-exist-snapshot-parent"}):
        code = CAPTURE["main"]()
    after = inventory(case)
    put(case / "dry-stdout.json", captured.getvalue().encode())
    put(case / "dry-stderr.log", stderr.getvalue().encode())
    require(before == after, "dry validation changed the fixture tree or created output")
    require(not (case / "must-not-exist-snapshot-parent").exists(), "dry validation created a snapshot")
    require(stderr.getvalue() == "", "dry validation wrote unexpected stderr")
    report = json.loads(captured.getvalue())
    require(report.get("validate_only") is True, "dry receipt did not identify its scope")
    require(report.get("snapshot_created") is not True, "dry validation claimed snapshot creation")
    return code, report, {"command_arguments": arguments, "fixture_tree_unchanged": True,
                         "snapshot_created": False, "native_destination_discovery_forbidden": True,
                         "writes_forbidden_during_call": True, "input_inventory": before}


REJECTIONS = {
    "wrong-archive-sha": "archive SHA-256 differs",
    "wrong-pax-commit": "Git PAX commit differs",
    "wrong-manifest-commit": "source manifest commit/archive differs",
    "wrong-mode": "Git executable mode differs",
    "wrong-file-hash": "file contents differ",
    "wrong-file-size": "archive file size differs",
    "missing-file": "archive source file inventory differs",
    "missing-modes": "every input needs a Git mode",
    "wrong-mode-binding": "Git mode manifest is not bound",
    "wrong-external-mode": "Git executable mode differs",
    "manifest-case-alias": "case alias or file/directory collision",
    "traversal": "unsafe path component",
    "absolute-path": "unsafe archive path",
    "ancestor-case-alias": "case alias or file/directory collision",
    "file-directory-collision": "case alias or file/directory collision",
    "duplicate-member": "duplicate archive member",
    "symlink": "links and nonregular TAR members are forbidden",
    "hardlink": "links and nonregular TAR members are forbidden",
    "fifo": "links and nonregular TAR members are forbidden",
    "character-device": "links and nonregular TAR members are forbidden",
    "special-permission-bits": "special file permission bits are forbidden",
    "trailing-nonzero": "nonzero data follows the TAR end marker",
}


def test_case(case, mutation):
    inputs = fixture(case, mutation)
    code, report, evidence = dry_call(case, inputs)
    if mutation in REJECTIONS:
        require(code == 1 and report.get("status") == "failed", "hostile archive was not rejected")
        require(REJECTIONS[mutation] in report.get("error", ""), f"wrong rejection: {report.get('error')}")
    else:
        require(code == 0 and report.get("status") == "passed", f"valid fixture was rejected: {report.get('error')}")
        require(report.get("files") == len(PAYLOAD) and report.get("bytes") == sum(len(data) for data, _ in PAYLOAD.values()),
                "valid capture inventory/counts differ")
        require(report.get("source_commit") == COMMIT and report.get("archive_sha256") == inputs["actual_archive_sha"]
                and report.get("source_manifest_sha256") == LAYOUT["sha"](inputs["manifest"]),
                "valid capture provenance differs")
        wanted_mode_hash = None if inputs["modes"] is None else LAYOUT["sha"](inputs["modes"])
        require(report.get("mode_manifest_sha256") == wanted_mode_hash, "Git mode provenance differs")
    evidence["adapter_result"] = report
    evidence["exit_code"] = code
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    LAYOUT["native_host"]()
    root = LAYOUT["fresh"](LAYOUT["evidence_location"](args.output_dir))
    recipes = (Path(__file__).absolute(), HERE / "capture-linux-archive.py", HERE / "reproduce-windows.py")
    hashes = {str(path): LAYOUT["sha"](path) for path in recipes}
    result = {"schema_version": 1, "kind": "axeon-linux-archive-synthetic-dry-tests", "status": "running",
              "native_extraction_qualified": False, "linux_execution_qualified": False,
              "compiler_executed": False, "subprocesses_started": 0, "wsl_started": False,
              "accepted_packages_accessed": False, "fixtures_preserved": True,
              "recipe_sha256": hashes, "checks": []}
    receipt = root / "capture-tests.json"
    LAYOUT["write_json"](receipt, result)
    cases = ["valid-control", "git-umask-control", "external-modes-control", *REJECTIONS]
    with patch("subprocess.Popen", side_effect=AssertionError("synthetic capture tests must not launch subprocesses")):
        for name in cases:
            case = LAYOUT["fresh"](root / name)
            check = {"name": name, "fixture": str(case), "passed": False}
            result["checks"].append(check)
            try:
                check["details"] = test_case(case, name)
                check["passed"] = True
            except Exception as error:
                check["error"] = f"{type(error).__name__}: {error}"
                put(case / "failure.txt", traceback.format_exc().encode())
            LAYOUT["write_json"](case / "case-result.json", check)
            LAYOUT["write_json"](receipt, result)
    result["passed"] = sum(check["passed"] for check in result["checks"])
    result["failed"] = len(cases) - result["passed"]
    result["recipes_unchanged"] = all(LAYOUT["sha"](path) == hashes[str(path)] for path in recipes)
    result["status"] = "passed" if result["failed"] == 0 and result["recipes_unchanged"] else "failed"
    LAYOUT["write_json"](receipt, result)
    print(json.dumps({"status": result["status"], "synthetic_unit_tests": True, "receipt": str(receipt),
                      "passed": result["passed"], "failed": result["failed"]}))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())

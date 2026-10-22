"""Verify the approved, already-acquired Jolt bytes; never download or run SDK code."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat

from output_paths import evidence_path, project_root

LOCK = Path(__file__).with_name("physics_dependencies.lock.json")


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def regular(path: Path) -> None:
    for part in (path, *path.parents):
        info = part.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError(f"Dependency redirects through a link: {part}")
    if not path.is_file():
        raise ValueError(f"Dependency input is not a regular file: {path}")


def relative_name(value: str) -> str:
    path = PurePosixPath(value)
    if (not value or path.is_absolute() or path.as_posix() != value
            or any(p in {".", ".."} or p.endswith((" ", ".")) for p in path.parts)
            or re.search(r'[\\:\x00-\x1f]', value)):
        raise ValueError("Invalid dependency manifest path")
    return value


def verify_files(source: Path, records: list[dict]) -> tuple[int, int]:
    expected: set[str] = set()
    folded: set[str] = set()
    total = 0
    for item in records:
        name = relative_name(item["path"])
        if name.casefold() in folded:
            raise ValueError("Duplicate dependency manifest path")
        expected.add(name)
        folded.add(name.casefold())
        path = source / name
        regular(path)
        size = path.stat().st_size
        if size != item["bytes"] or size > 128 * 1024 * 1024:
            raise ValueError(f"Dependency size differs: {name}")
        if sha256(path) != item["sha256"]:
            raise ValueError(f"Dependency content differs: {name}")
        total += size
    # Links are refused before traversal, including otherwise empty directory links.
    actual: set[str] = set()
    for directory, dirs, files in source.walk(follow_symlinks=False):
        for name in dirs + files:
            path = directory / name
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                raise ValueError(f"Dependency contains a link: {path}")
        actual.update((directory / name).relative_to(source).as_posix() for name in files)
    if actual != expected:
        raise ValueError("Dependency file set differs from approved upstream tree")
    return len(expected), total


def verify(root: Path) -> dict:
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    if lock["schema"] != 1:
        raise ValueError("Unsupported dependency lock schema")
    dependency = root / relative_name(lock["directory"])
    manifest = root / relative_name(lock["manifest"])
    archive = dependency / "archive" / (lock["commit"] + ".tar.gz")
    for path, digest in ((manifest, lock["manifest_sha256"]), (archive, lock["archive_sha256"])):
        regular(path)
        if sha256(path) != digest:
            raise ValueError(f"Pinned dependency identity differs: {path.name}")
    records = json.loads(manifest.read_text(encoding="utf-8"))["files"]
    source = dependency / "source"
    count, total = verify_files(source, records)
    if count != lock["files"] or total != lock["bytes"]:
        raise ValueError("Pinned dependency counts differ")
    licence_files = [item for item in records if "license" in item["path"].lower()]
    spdx: dict[str, list[str]] = {}
    without_spdx = []
    for item in records:
        if not item["path"].startswith("Jolt/"):
            continue
        text = (source / item["path"]).read_text(encoding="utf-8", errors="replace")
        labels = re.findall(r"SPDX-License-Identifier:\s*([^\r\n]+)", text)
        if not labels:
            without_spdx.append(item["path"])
        for label in labels:
            spdx.setdefault(label, []).append(item["path"])
    if set(spdx) != {"MIT"}:
        raise ValueError("Unexpected licence in the admitted Jolt library subtree")
    return {"schema": 1, "passed": True, "source_root": str(source),
            "commit": lock["commit"], "git_tree_sha1": lock["git_tree_sha1"],
            "archive_sha256": lock["archive_sha256"], "lock_sha256": sha256(LOCK),
            "manifest_sha256": lock["manifest_sha256"], "files": count, "bytes": total,
            "licences": licence_files, "library_spdx_counts": {k: len(v) for k, v in spdx.items()},
            "library_without_spdx": without_spdx,
            "licence_scope": "Root MIT applies to library; Assets/Racetracks is LGPL-3.0. "
                             "All Assets, examples, viewers and TestFramework are excluded.",
            "sdk_code_executed": False, "native_qualified": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = evidence_path(args.output) if args.output else None
    if output and output.exists():
        raise ValueError("Refusing to replace an existing dependency receipt")
    try:
        result = verify(project_root())
    except (OSError, KeyError, TypeError, ValueError) as error:
        result = {"schema": 1, "passed": False, "error": str(error)}
    encoded = json.dumps(result, indent=2) + "\n"
    if output:
        with output.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(encoded)
    print(encoded, end="")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

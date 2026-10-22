"""Generate the versioned qualification-only Jolt tree-stack correction.

The acquired upstream tree remains immutable. The derived translation unit is
permitted only in this checkout's ignored build cache, with both hashes pinned.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from physics_dependencies import regular, relative_name

ROOT = Path(__file__).resolve().parents[1]
LOCK = Path(__file__).with_name("physics_build_overlay.lock.json")
SOURCE = "Jolt/Physics/Collision/BroadPhase/QuadTree.cpp"
PATCH = "race-jolt-5.6.0-tree-stack-256-v1"
REPLACEMENTS = (
    (b"\tStackEntry stack[cStackSize / 4]; // We don't process 4 at a time in this loop but 1, so the stack can be 4x as small",
     b"\tStackEntry stack[256]; // RACE tree-stack-256-v1: <=256 disjoint body/node inputs; depth <256."),
    (b"\t\t\t\tJPH_ASSERT(top < cStackSize / 4);",
     b"\t\t\t\tJPH_ASSERT(top < 256);"),
)


def derive(source: bytes, lock: dict) -> bytes:
    if (lock["schema"] != 1 or lock["patch"] != PATCH or lock["source"] != SOURCE
            or lock["maximum_bodies"] != 256
            or lock["upstream_commit"] != "e77f175595e64cb44218cc9d9d56fc365ad0e36a"):
        raise ValueError("Unsupported physics build overlay")
    if hashlib.sha256(source).hexdigest() != lock["upstream_sha256"]:
        raise ValueError("Overlay upstream content differs")
    result = source
    for old, new in REPLACEMENTS:
        if result.count(old) != 1:
            raise ValueError("Overlay anchor count differs")
        result = result.replace(old, new)
    if hashlib.sha256(result).hexdigest() != lock["derived_sha256"]:
        raise ValueError("Overlay derived content differs")
    return result


def checked_output(output: Path, root: Path, filename: str) -> Path:
    # Check lexical location and all existing ancestors before creating anything.
    root = root.resolve(strict=True)
    output = output.absolute()
    if not output.is_relative_to(root / "out") or ".." in output.parts or output.name != filename:
        raise ValueError(f"Overlay output must be {filename} inside the checkout build cache")
    for parent in (output, *output.parents):
        if parent.exists() or parent.is_symlink():
            # regular() also rejects directory junctions anywhere in the ancestry.
            if parent.is_file():
                regular(parent)
            else:
                import stat
                info = parent.lstat()
                if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                    raise ValueError("Overlay output redirects through a link")
    return output


def write_immutable(output: Path, result: bytes) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        regular(output)
        if output.read_bytes() != result:
            raise ValueError("Refusing to overwrite different derived SDK bytes")
    else:
        with output.open("xb") as stream:
            stream.write(result)


def generate(source: Path, output: Path, root: Path = ROOT) -> dict:
    output = checked_output(output, root, "QuadTree.cpp")
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    original = source / relative_name(lock["source"])
    regular(original)
    result = derive(original.read_bytes(), lock)
    write_immutable(output, result)
    return dict(schema=1, patch=PATCH, upstream=str(original), derived=str(output),
                upstream_sha256=lock["upstream_sha256"], derived_sha256=lock["derived_sha256"],
                maximum_bodies=256, stack_bytes=256*64, qualification_only=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(generate(args.source, args.output), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

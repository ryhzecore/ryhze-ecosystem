#!/usr/bin/env python3
"""Create a reproducible UNSIGNED payload from an already verified build.

This is packaging/provenance, not a Windows build, signature verification,
installer, updater or proof that its inputs were reproducibly compiled.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile

from prepare_bundle import bundle_files


def archive_payload(bundle: Path, output: Path, *, version: str,
                    source: str, lockfile: Path) -> dict:
    if not re.fullmatch(r"0\.6\.0\+3", version):
        raise ValueError("This candidate requires the explicitly selected version 0.6.0+3")
    if not re.fullmatch(r"[0-9a-f]{40}", source):
        raise ValueError("An exact source revision is required")
    files = bundle_files(bundle)
    bundle = bundle.resolve(strict=True)
    # Resolve parents, but refuse existing output/symlinks instead of overwriting.
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError("Output must not already exist")
    destination = output.parent.resolve(strict=True) / output.name
    if destination == bundle or bundle in destination.parents:
        raise ValueError("Output must be outside the application payload")
    payloads = [(path.as_posix(), (bundle / path).read_bytes()) for path in files]
    manifest = {
        "format": 1,
        "product": "Ryhze",
        "version": version,
        "source_revision": source,
        "artifact_kind": "unsigned-application-payload",
        "dependency_lock_sha256": hashlib.sha256(lockfile.read_bytes()).hexdigest(),
        "files": [{"path": name, "bytes": len(data),
                   "sha256": hashlib.sha256(data).hexdigest()} for name, data in payloads],
    }
    entries = [(f"app/{name}", data) for name, data in payloads]
    entries += [("provenance.json", (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode()),
                ("dependency-lock.yaml", lockfile.read_bytes())]
    # ZIP_STORED removes compressor-version variability; fixed metadata removes
    # source mtimes/host modes. Signed installers are separate timestamped outputs.
    with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_STORED) as archive:
        for name, data in sorted(entries):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data)
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--lockfile", type=Path, required=True)
    args = parser.parse_args()
    archive_payload(args.bundle, args.output, version=args.version,
                    source=args.source_revision, lockfile=args.lockfile)
    print(f"Unsigned payload SHA-256: {hashlib.sha256(args.output.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    main()

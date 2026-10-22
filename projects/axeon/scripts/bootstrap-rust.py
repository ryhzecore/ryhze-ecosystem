#!/usr/bin/env python3
"""Install pinned prebuilt Rust components locally; never execute install hooks.

Requires Python 3.12+ for safe tar extraction. Clang is supplied separately.
Archives are fetched only from the committed lock and verified before extraction.
"""
import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
LOCK = json.loads((ROOT / "toolchain-lock.json").read_text())
TOOLS = ROOT / ".tools"
for item in LOCK["rust_components"]:
    archive = TOOLS / "downloads" / item["url"].rsplit("/", 1)[1]
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        with urllib.request.urlopen(item["url"], timeout=60) as response:
            with archive.open("wb") as dest:
                shutil.copyfileobj(response, dest)
    if hashlib.file_digest(archive.open("rb"), "sha256").hexdigest() != item["sha256"]:
        raise SystemExit(f"hash mismatch: {archive}; remove it and retry")
    with tarfile.open(archive) as packed:
        packed.extractall(TOOLS / "unpacked", filter="data")
    base = TOOLS / "unpacked" / archive.name.removesuffix(".tar.xz")
    for component in base.iterdir():
        if component.is_dir() and (component / "manifest.in").exists():
            for name in ["bin", "lib", "libexec", "etc", "share"]:
                if (component / name).exists():
                    shutil.copytree(component / name, TOOLS / "rust" / name, dirs_exist_ok=True)
    print(f"Verified and extracted {item['component']}")
print(f'export PATH="{TOOLS / "rust/bin"}:$PATH"')

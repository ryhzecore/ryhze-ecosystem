#!/usr/bin/env python3
"""Measure a clean, isolated release build; no benchmark kernels are run."""
import hashlib
import json
import os
from pathlib import Path
import tempfile

from run import HERE, ROOT, Runner, file_hash


def main():
    sources = [ROOT / "Cargo.toml", ROOT / "Cargo.lock"]
    sources += sorted((ROOT / "src").glob("*.rs"))
    sources += sorted((ROOT / "runtime").glob("*"))
    fingerprints = {str(path.relative_to(ROOT)): file_hash(path) for path in sources if path.is_file()}
    source_fingerprint = hashlib.sha256(json.dumps(fingerprints, sort_keys=True).encode()).hexdigest()
    runner = Runner(HERE, min(os.sched_getaffinity(0)))
    cargo = ROOT / ".tools/rust/bin/cargo"
    rustc = ROOT / ".tools/rust/bin/rustc"
    version = runner.run([rustc, "--version"], "rust_version")["stdout"].strip()
    with tempfile.TemporaryDirectory(prefix="compiler-build-", dir=HERE) as temporary:
        command = ["/usr/bin/env", f"CARGO_TARGET_DIR={temporary}",
                   f"PATH={cargo.parent}:{os.environ.get('PATH', '')}",
                   cargo, "build", "--offline", "--release"]
        record = runner.run(command, "clean_rust_release_compiler_build")
        fresh_hash = file_hash(Path(temporary) / "release/axeon")
    after = {name: file_hash(ROOT / name) for name in fingerprints}
    if after != fingerprints:
        raise RuntimeError("compiler source changed during its measured build")
    receipt = {"rust_version": version, "source_sha256_by_file": fingerprints,
               "source_fingerprint_sha256": source_fingerprint,
               "fresh_compiler_sha256": fresh_hash,
               "benchmark_compiler_sha256": file_hash(ROOT / "target/release/axeon"),
               "build": record, "build_wall_seconds": record["wall_ns"] / 1e9,
               "limits": {"virtual_memory_bytes": 2 * 1024**3, "subprocess_wall_seconds": 30},
               "note": "Fresh temporary CARGO_TARGET_DIR; no target artifacts reused; directory removed after hashing."}
    receipt["fresh_matches_benchmark_compiler"] = receipt["fresh_compiler_sha256"] == receipt["benchmark_compiler_sha256"]
    destination = HERE / "compiler-build-receipt.json"
    destination.write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"receipt": str(destination), "wall_seconds": receipt["build_wall_seconds"],
                      "peak_rss_kib": record["peak_rss_kib"],
                      "fresh_matches_benchmark_compiler": receipt["fresh_matches_benchmark_compiler"]}))


if __name__ == "__main__":
    main()

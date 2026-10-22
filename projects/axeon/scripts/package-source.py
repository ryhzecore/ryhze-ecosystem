#!/usr/bin/env python3
"""Create a deterministic source/history/evidence archive from clean committed Git."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, timeout=60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if git("status", "--porcelain").strip():
        raise SystemExit("Commit or preserve worktree changes before packaging.")
    commit = git("rev-parse", "HEAD").decode().strip()
    original = git("rev-parse", "ryhze-cpu-foundation-0.1").decode().strip()
    prefix = "axeon-cpu-0.5/"
    files = {}
    archive = git("archive", "--format=tar", commit)
    with tarfile.open(fileobj=io.BytesIO(archive)) as source:
        for item in source.getmembers():
            if item.isfile():
                files[f"source/{item.name}"] = (source.extractfile(item).read(), item.mode)
            elif not item.isdir():
                raise SystemExit(f"Unsupported nonregular source member: {item.name}")
    with tempfile.TemporaryDirectory(prefix="axeon-package-") as temporary:
        bundle = Path(temporary) / "repository.bundle"
        git("-c", "pack.threads=1", "bundle", "create", str(bundle), "--all")
        git("bundle", "verify", str(bundle))
        files["repository.bundle"] = (bundle.read_bytes(), 0o644)
    delivery = f"""# Axeon source CPU tensor checkpoint 0.5

Source commit: `{commit}`
Original scalar checkpoint: `{original}` (`ryhze-cpu-foundation-0.1`).

This archive contains complete tracked source, specifications, native evidence,
benchmark chart/raw samples, and the full local Git history. No paid resources,
external CI, publication or changes to stopped product repositories were used.

Verify all enclosed files from this directory with `sha256sum -c SHA256SUMS`.
Recover history using `git clone repository.bundle axeon-language`.
The `source/` directory is also directly buildable. Follow `source/README.md`:
Rust 1.85.0, Clang 19.1.7 and a hosted x86-64 Linux environment are required;
no third-party Cargo dependencies. Toolchain binaries are not bundled.

This checkpoint adds actual scoped CPU tensors to Axeon source programs:
compiler 0.4.0, CPU language subset 4. Native tensor execution calls the unchanged
`axeon-numerics` 0.1.0 library through `axeon-tensor-runtime` 0.1.0, internal
tensor ABI 1. Hosted C implementation 2 retains the scalar hosted ABI and adds
the internal fatal-cleanup hook. The reference interpreter independently
implements tensor storage and arithmetic; it does not call the numerical crate.

This is general-purpose language and numerical infrastructure. Gideon remains
planning-only; no model architecture selection, model implementation, production
training preparation or weight training is included or authorized by this
checkpoint.

The full debug/release workspace gate passed 592 tests, 296 per profile.
Read `source/docs/validation-0.5.md` for current source numerical-oracle,
independent-review and clean-build receipts, resource costs and qualifications.
Earlier validation records retain their original checkpoint scope.

Source support includes checked i64, strict f64, bool functions/control flow,
bounded homogeneous local arrays and lexical slices. New `tensor_scope` blocks
provide explicit payload budgets, owned CPU f64 tensors, checked shared/exclusive
views, affine moves, conservative lexical loans and cleanup. Fallible numerical
operations require `try`; a typed `tensor_error` catch binding exposes the error
kind and original source byte span after protected-body cleanup. Earlier scalar
writes and output persist. Fatal scalar traps bypass catch after cleanup.

Tensor operations include checked construction, shape/linear-index queries and
writes, direct-owner transpose/slice views, ordered add/mul, sum and matrix
multiplication, and finite-input stable sigmoid, softmax and log-sum-exp.
The existing canonical-NaN, strict binary64 and ordered-reduction contracts
remain in force. Hosted transcendental accuracy has recorded corpus/host
acceptance bounds, not a universal correct-rounding guarantee.

The bounded surface permits one active numerical context, a selected live
requested-payload budget of 0 through 64 MiB, rank at most 8, extents/elements
at most 65,536, and 256 reusable owner/view slots. Matrix admission requires
`M*N*K + M*N <= 1,048,576`. The payload budget excludes allocator overhead and
metadata. Scopes occur directly in function bodies; protected-body returns or
loop exits crossing the scope boundary are rejected. Owners/views cannot escape,
be implicitly copied, or pass through function parameters/results. Full source,
parser and function limits remain in `source/docs/language.md`.

Unsupported: generic first-class Result, general heap containers/aggregates,
composed source views/reborrowing, tensor function interfaces, broadcasting,
general-axis reductions, f32, FMA/fast-math, scalar libm APIs, user FFI, arbitrary
allocators, package imports, freestanding targets, tasks, autodiff,
optimizers/training, non-CPU devices and distributed execution.

The preserved 0.2 integer comparison is in `source/benchmarks/results/`; its
phone-friendly line chart is `source/benchmarks/social-0.2/social.png`. Read
`source/docs/benchmark-results.md` for its precise workload-limited claims.
The preserved 0.3 binary64 profiles and comparisons are in `source/benchmarks/numerical/`,
with ordinary Python, optimized checked C++, and a separately qualified NumPy
comparison for matrix-vector work. Raw data and precision limits are included.
Those are preserved 0.3 source-language workloads; the distinct 0.4 native
Rust library comparison is under `source/benchmarks/numerical-library-0.4/`.
Its original order-limited run and one corrected scheduling run are both
preserved. Checkpoint 0.5 has no new timed comparison; those 0.4 measurements
remain native Rust library evidence and are not source-language tensor timing.
Small shared-host measurements do not establish a general speed lead.

The complete 0.4 checkpoint remains at commit
`22ebf32e5bbc6128c8e53584d447c1d482ed56e2`, tag `axeon-cpu-0.4`, and Library
`libfile_012753586ff88191bd804caaf605909b`.

The complete 0.3 checkpoint remains at commit
`04576e6b6f81d5501e73c79132354f45400761c3`, tag `axeon-cpu-0.3`, and Library
`libfile_29a14904f2ac81918e75f30debb87c0d`.

The 0.2 ownership checkpoint remains at commit
`77499b4bb8d118666a582daeeb04927638b99c3f`, tag `axeon-cpu-0.2`, and Library
`libfile_1f2f7278c2908191b16c129c93df5119`.

The original Library checkpoint remains `libfile_d05e6e8b74c08191b01d543284eb86fd`.
This new archive does not replace it. The source architecture retains its
review history and a refreshed numerical-section review after its owner renamed
it to AXEON LANGUAGE ARCHITECTURE; diagram pixels remain unreviewed.
"""
    files["DELIVERY.md"] = (delivery.encode(), 0o644)
    manifest = "".join(
        f"{hashlib.sha256(data).hexdigest()}  {name}\n"
        for name, (data, _) in sorted(files.items())
    )
    files["SHA256SUMS"] = (manifest.encode(), 0o644)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as target:
                for name, (data, mode) in sorted(files.items()):
                    info = tarfile.TarInfo(prefix + name)
                    info.size = len(data)
                    info.mode = mode
                    info.mtime = info.uid = info.gid = 0
                    target.addfile(info, io.BytesIO(data))
    with tarfile.open(output, "r:gz") as check:
        for name, (data, _) in files.items():
            if check.extractfile(prefix + name).read() != data:
                raise RuntimeError(f"Archive verification failed: {name}")
    print(json.dumps({
        "file": str(output), "source_commit": commit,
        "original_scalar_commit": original, "size_bytes": output.stat().st_size,
        "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "members_verified": len(files), "git_bundle_verified": True,
    }, indent=2))


if __name__ == "__main__":
    main()

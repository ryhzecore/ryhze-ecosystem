#!/usr/bin/env python3
"""Record bounded local verification; not a performance benchmark."""
import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import resource
import runpy
import shutil
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
os.chdir(ROOT)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-dir", type=Path,
                    help="fresh evidence directory; defaults to a dated local Evidence/workspace-validation attempt")
args = parser.parse_args()
if args.output_dir is None:
    try:
        layout = runpy.run_path(str(ROOT / "scripts/project_layout.py"))
        args.output_dir = layout["reserve_evidence_output"]("workspace-validation")
    except (OSError, ValueError) as error:
        parser.error(str(error))
EVIDENCE = args.output_dir.resolve()
EVIDENCE.mkdir(parents=True, exist_ok=True)
if any((EVIDENCE / name).exists() for name in ("verification.json", "verify.log", "host.json")):
    raise SystemExit("Choose a fresh output directory; prior verification evidence is preserved.")
def read(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return "unavailable"
def command(*args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=10)
    return result.stdout.strip() or result.stderr.strip()

host = {
    "schema_version": 1,
    "captured_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "platform": platform.platform(),
    "cpu": json.loads(command("lscpu", "--json")),
    "cgroup_memory_max": read("/sys/fs/cgroup/memory.max"),
    "cgroup_cpu_max": read("/sys/fs/cgroup/cpu.max"),
    "rust": command(str(ROOT / ".tools/rust/bin/rustc") if (ROOT / ".tools/rust/bin/rustc").exists() else (shutil.which("rustc") or "rustc"), "--version"),
    "clang": command(os.environ.get("AXEON_CLANG") or ("/workspace/tools/sysroot/usr/lib/llvm-19/bin/clang" if Path("/workspace/tools/sysroot/usr/lib/llvm-19/bin/clang").exists() else "clang-19"), "--version"),
    "linker": command("ld", "--version").splitlines()[0],
    "libc": command("ldd", "--version").splitlines()[0],
    "limits": {"virtual_memory_mib_per_process": 2048, "cpu_seconds_per_process": 180, "build_command_wall_seconds": 180, "test_profile_wall_seconds": 300, "test_workers": int(os.environ.get("RUST_TEST_THREADS", "1")), "cargo_jobs": int(os.environ.get("CARGO_BUILD_JOBS", "1"))},
    "claims": "Local virtualized CPU correctness validation only; no benchmark, device or cross-host reproducibility claim.",
}
(EVIDENCE / "host.json").write_text(json.dumps(host, indent=2) + "\n")
start = time.monotonic()
with (EVIDENCE / "verify.log").open("w") as log:
    with subprocess.Popen(
        [str(ROOT / "scripts/verify.sh")], stdout=log, stderr=subprocess.STDOUT,
        start_new_session=True,
    ) as process:
        try:
            exit_code = process.wait(timeout=900)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
            log.write("\nVerification exceeded its 900-second overall deadline.\n")
            exit_code = 124
usage = resource.getrusage(resource.RUSAGE_CHILDREN)
summary = {
    "schema_version": 2, "command": "scripts/verify.sh", "exit_code": exit_code,
    "output_directory": str(EVIDENCE.relative_to(ROOT)) if EVIDENCE.is_relative_to(ROOT) else str(EVIDENCE),
    "overall_wall_timeout_seconds": 900,
    "overall_timeout_kills_process_group": True,
    "wall_seconds": round(time.monotonic() - start, 3),
    "children_user_cpu_seconds": round(usage.ru_utime, 3),
    "children_system_cpu_seconds": round(usage.ru_stime, 3),
    "maximum_child_rss_kib": usage.ru_maxrss,
    "measurement_scope": "Cumulative child CPU and maximum reported child RSS, not simultaneous total process-tree memory; includes host probes.",
}
(EVIDENCE / "verification.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, indent=2))
raise SystemExit(exit_code)

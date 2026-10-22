#!/usr/bin/env python3
"""Serial, local checkpoint 0.14 delivery from a committed ext4 snapshot.

Run with the pinned Debian/WSL Python. Stages are preflight, reproduce, build,
package, export, installed, and all. Every destination/receipt is exclusive;
failed stages remain inspectable and require a fresh output configuration.
The source checkout may have a Windows-style .git worktree file. Only Git reads
use that checkout; every compiler/native command runs on ext4. The caller must
first pass the retained Windows/WSL memory, commit-headroom, free-disk and
no-other-compute launch guards; this helper does not replace those host guards.
"""
import argparse
import ctypes
import fcntl
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import resource
import select
import shutil
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile
import time


CHECKPOINT = "0.14"
VERSION = "0.13.0"
SUBSET = 13
VERSION_OUTPUT = b"axeon 0.13.0; subset 13; LLVM/Clang 19.1.7; hosted ABI 1 (single-file), 2 (modules)\n"
FEATURES = ("numerics", "tensors", "modules", "records", "unions", "aggregate-interfaces",
            "nested-aggregates", "unit-values", "static-traits", "generic-functions", "generic-nominals")
FLAGS = ["--include-" + feature for feature in FEATURES]
# Reviewed delivery-only changes may postdate the unchanged functional source.
# Every changed path still needs an explicit --allow-post-validation argument.
DELIVERY_HELPER_CHANGES = {"scripts/deliver-generic-nominals.py",
                           "scripts/test-deliver-generic-nominals.py"}
BINARY_KEYS = ("target/release/axeon", "target/release/libaxeon_tensor_runtime.a")
GENERIC_OUTPUT = b"42\n7\n9\nresult: 0\n"
GENERIC_NOMINAL_OUTPUT = b"42\n-9223372036854775808\nresult: 0\n"
GENERIC_NOMINAL_TEST_OUTPUT = (
    b"running 3 tests (reference interpreter; fuel 1000000 per test)\n"
    b"test test_absent_payload_retains_negative_zero ...\nPASS test_absent_payload_retains_negative_zero\n"
    b"test test_exact_array_payload ...\nPASS test_exact_array_payload\n"
    b"test test_nested_generic_payload ...\nPASS test_nested_generic_payload\n"
    b"test result: 3 passed; 0 failed\n")
ORDINARY_OUTPUT = b"42\nresult: 0\n"
MAX_FILE = 512 << 20
MAX_TREE = 768 << 20
MAX_DIAGNOSTIC = 64 << 10
CLEANUP_SECONDS = 15
MAX_OWNED_PROCESSES = 512
MAX_PROCESS_TASKS = 4096


def need(condition, message):
    if not condition:
        raise RuntimeError(message)


def plain_path(path):
    path = Path(path).absolute()
    for ancestor in (path, *path.parents):
        need(not ancestor.is_symlink(), f"symlink path is unsupported: {ancestor}")
    return path


def fresh(path):
    plain_path(path)
    need(not path.exists(), f"preserve existing destination: {path}")


def checksum(path):
    path = plain_path(path)
    metadata = path.stat()
    need(stat.S_ISREG(metadata.st_mode) and metadata.st_size <= MAX_FILE,
         f"bounded regular file required: {path}")
    with path.open("rb") as content:
        return hashlib.file_digest(content, "sha256").hexdigest()


def read_json(path):
    checksum(path)
    return json.loads(path.read_text(encoding="utf-8"))


def write_new(path, value):
    fresh(path)
    with path.open("x", encoding="utf-8") as target:
        json.dump(value, target, indent=2, allow_nan=False)
        target.write("\n")


def tree_hashes(base):
    base = plain_path(base)
    need(base.is_dir(), f"directory required: {base}")
    hashes, total, entries = {}, 0, 0
    pending = [base]
    while pending:
        with os.scandir(pending.pop()) as children:
            for entry in children:
                entries += 1
                need(entries <= 16384, f"tree entry budget exceeded: {base}")
                path = Path(entry.path)
                need(not entry.is_symlink(), f"symlink tree entry: {path}")
                if entry.is_dir(follow_symlinks=False):
                    pending.append(path)
                else:
                    need(entry.is_file(follow_symlinks=False), f"nonregular tree entry: {path}")
                    total += entry.stat().st_size
                    need(total <= MAX_TREE, f"tree byte budget exceeded: {base}")
                    hashes[path.relative_to(base).as_posix()] = checksum(path)
    return dict(sorted(hashes.items()))


def limits():
    resource.setrlimit(resource.RLIMIT_AS, (2 << 30, 2 << 30))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    _, cpu_hard = resource.getrlimit(resource.RLIMIT_CPU)
    cpu_bound = 180 if cpu_hard == resource.RLIM_INFINITY else min(180, cpu_hard)
    resource.setrlimit(resource.RLIMIT_CPU, (cpu_bound, cpu_bound))
    resource.setrlimit(resource.RLIMIT_FSIZE, (MAX_FILE, MAX_FILE))
    soft, hard = resource.getrlimit(resource.RLIMIT_NPROC)
    bound = 256 if hard == resource.RLIM_INFINITY else min(256, hard)
    resource.setrlimit(resource.RLIMIT_NPROC, (bound, bound))


def require_cleanup_support():
    need(sys.platform == "linux" and hasattr(os, "pidfd_open")
         and hasattr(signal, "pidfd_send_signal"), "Linux pidfd cleanup support is required")


def task_paths(pid):
    tasks = []
    with os.scandir(f"/proc/{pid}/task") as entries:
        for entry in entries:
            need(len(tasks) < MAX_PROCESS_TASKS, "owned process task limit exceeded")
            tasks.append(Path(entry.path))
    return tasks


def direct_children(pid):
    children = set()
    for task in task_paths(pid):
        try:
            with (task / "children").open() as content:
                text = content.read(MAX_DIAGNOSTIC + 1)
            need(len(text) <= MAX_DIAGNOSTIC, "owned child inventory exceeds byte limit")
            children.update(map(int, text.split()))
            need(len(children) <= MAX_OWNED_PROCESSES, "owned child inventory exceeds process limit")
        except FileNotFoundError:
            pass
    return children


def freeze_tree(pid, parent, handles, deadline):
    """Adapt the audited baseline pidfd traversal, awaiting group-stop first.

    Each opened pidfd is checked against the expected parent before signaling.
    The still-live subreaper retains any child whose parent exits during lookup;
    its direct-child inventory is scanned again before cleanup completes.
    """
    need(time.monotonic() < deadline, "owned tree cleanup deadline exceeded")
    if pid in handles:
        return
    need(len(handles) < MAX_OWNED_PROCESSES, "owned tree cleanup process limit exceeded")
    try:
        handle = os.pidfd_open(pid)
    except ProcessLookupError:
        return
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
        if int(fields[1]) != parent:
            return
        handles[pid] = handle
        signal.pidfd_send_signal(handle, signal.SIGSTOP)
        while not select.select([handle], [], [], 0)[0]:
            need(time.monotonic() < deadline, "owned process did not stop before cleanup deadline")
            tasks = task_paths(pid)
            stopped = all(task.joinpath("stat").read_text().rsplit(") ", 1)[1].split()[0]
                          in ("T", "t", "Z", "X") for task in tasks)
            if stopped and {task.name for task in tasks} == {task.name for task in task_paths(pid)}:
                for child in sorted(direct_children(pid)):
                    freeze_tree(child, pid, handles, deadline)
                break
            time.sleep(0.002)
    except (FileNotFoundError, ProcessLookupError):
        # The private subreaper owns surviving orphans, even if the original
        # parent disappeared before SIGSTOP or while reading /proc.
        pass
    finally:
        if pid not in handles:
            os.close(handle)


def kill_handles(handles):
    for handle in reversed(list(handles.values())):
        try:
            signal.pidfd_send_signal(handle, signal.SIGKILL)
        except ProcessLookupError:
            pass
        finally:
            os.close(handle)


def reap_owned_children():
    """Called only in the isolated supervisor, never in the delivery process."""
    while True:
        try:
            pid, _ = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            return True
        if pid == 0:
            return False


def cleanup_descendants():
    deadline = time.monotonic() + CLEANUP_SECONDS
    observed = set()
    while True:
        handles = {}
        try:
            for child in sorted(direct_children(os.getpid())):
                freeze_tree(child, os.getpid(), handles, deadline)
            observed.update(handles)
            need(len(observed) <= MAX_OWNED_PROCESSES, "owned cleanup inventory limit exceeded")
        finally:
            kill_handles(handles)
        if reap_owned_children():
            return sorted(observed)
        need(time.monotonic() < deadline, "owned descendants were not reaped before cleanup deadline")
        time.sleep(0.002)


class SupervisorCancelled(Exception):
    def __init__(self, signum):
        super().__init__(f"supervisor cancelled by signal {signum}")
        self.signum = signum


def supervise(command):
    """Private process boundary: it has no children except this one command.

    PR_SET_CHILD_SUBREAPER keeps double-forked/new-session descendants here
    after a wrapper exits. The delivery process itself is never a subreaper,
    so its other children and the user's processes are outside this inventory.
    """
    require_cleanup_support()
    need(bool(command), "supervisor requires a command")
    libc = ctypes.CDLL(None, use_errno=True)
    libc.prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong,
                          ctypes.c_ulong, ctypes.c_ulong]
    libc.prctl.restype = ctypes.c_int
    if libc.prctl(36, 1, 0, 0, 0) != 0:  # Linux PR_SET_CHILD_SUBREAPER.
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))

    def cancelled(signum, _frame):
        raise SupervisorCancelled(signum)

    signal.signal(signal.SIGTERM, cancelled)
    signal.signal(signal.SIGINT, cancelled)
    code = 125
    try:
        process = subprocess.Popen(command, start_new_session=True)
        code = process.wait()
    except SupervisorCancelled as error:
        code = 128 + error.signum
    except BaseException as error:
        print(f"delivery supervisor failed: {str(error)[:4096]}", file=sys.stderr)
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        try:
            remaining = cleanup_descendants()
            if remaining and code == 0:
                print("delivery command exited with surviving descendants; cleaned owned tree", file=sys.stderr)
                code = 125
        except BaseException as error:
            print(f"delivery descendant cleanup failed: {str(error)[:4096]}", file=sys.stderr, flush=True)
            code = 125
            if not reap_owned_children():
                # Do not exit and reparent live children outside the private
                # owner on an exceptional cleanup failure. Stay stopped until
                # the parent's bounded emergency traversal freezes/kills this
                # complete tree. This path can never report stage success.
                os.kill(os.getpid(), signal.SIGSTOP)
                while True:
                    signal.pause()
    return code


def owned_command(command):
    require_cleanup_support()
    return [sys.executable, "-B", str(Path(__file__).resolve()), "--_supervise", *command]


def cancel_supervisor(process):
    # The unreaped direct child cannot have its PID reused. A pidfd additionally
    # makes each signal target explicit; no process groups or global scans.
    if process.poll() is not None:
        return
    handle = os.pidfd_open(process.pid)
    try:
        try:
            signal.pidfd_send_signal(handle, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=CLEANUP_SECONDS + 5)
        except subprocess.TimeoutExpired as error:
            # A failed/unresponsive supervisor is never accepted as cleanup.
            # Apply the audited parent-first freeze to its entire remaining
            # tree, then terminate those exact pidfds. The normal path above
            # keeps the supervisor alive to reap every descendant itself.
            handles = {}
            try:
                freeze_tree(process.pid, os.getpid(), handles,
                            time.monotonic() + CLEANUP_SECONDS)
            finally:
                kill_handles(handles)
            process.wait(timeout=5)
            raise RuntimeError("supervisor cleanup deadline failed; emergency owned-tree termination applied") from error
    finally:
        os.close(handle)


def run(command, cwd, env=None, timeout=120, expected=None, record=None):
    started = time.monotonic()
    process = subprocess.Popen(owned_command(command), cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               start_new_session=True, preexec_fn=limits)
    try:
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except BaseException:
            cancel_supervisor(process)
            process.communicate(timeout=5)
            raise
    finally:
        # Popen.__exit__ performs an unbounded wait. The explicit waits above
        # retain the cleanup deadline even if the emergency path itself fails.
        process.stdout.close()
        process.stderr.close()
    failed = process.returncode != 0
    unexpected = expected is not None and (stdout != expected or stderr != b"")
    if record is not None or failed or unexpected:
        result = {"command": command, "exit_code": process.returncode,
                  "stdout": stdout[:MAX_DIAGNOSTIC].decode("utf-8", errors="replace"),
                  "stderr": stderr[:MAX_DIAGNOSTIC].decode("utf-8", errors="replace"),
                  "stdout_bytes": len(stdout), "stderr_bytes": len(stderr),
                  "stdout_truncated": len(stdout) > MAX_DIAGNOSTIC,
                  "stderr_truncated": len(stderr) > MAX_DIAGNOSTIC,
                  "wall_seconds": time.monotonic() - started}
        if record is not None:
            record.append(result)
        # Construct diagnostics only on failure. In particular, never render a
        # successful command's payload while evaluating an assertion argument.
        if failed:
            raise RuntimeError(f"command failed: {result}")
        if unexpected:
            raise RuntimeError(f"unexpected command output: {result}")
    return stdout


def linux_path(value, base):
    """Convert a Git worktree pointer without relying on Windows Git from WSL."""
    if re.match(r"^[A-Za-z]:[/\\]", value):
        return plain_path(Path("/mnt") / value[0].lower() / value[3:].replace("\\", "/"))
    path = Path(value)
    return plain_path(path if path.is_absolute() else base / path)


def formatted_copy(compiler, workflow, project, original, env, checks):
    """Format/check a fresh source copy without compiling or touching originals."""
    formatted_name = f"formatted-{project}"
    formatted_dir = workflow / formatted_name
    fresh(formatted_dir)
    need(tree_hashes(workflow / project) == original, "original formatter inputs changed")
    shutil.copytree(workflow / project, formatted_dir)
    need(tree_hashes(formatted_dir) == original, "formatter input copy differs")
    ax_sources = [name for name in original if name.endswith(".ax")]
    for name in ax_sources:
        relative = f"{formatted_name}/{name}"
        formatted = run([compiler, "format", relative], workflow, env, timeout=45, record=checks)
        need(checks[-1]["stderr_bytes"] == 0 and 0 < len(formatted) <= 1 << 20,
             f"formatter output missing, oversized, or accompanied by stderr: {relative}")
        # Only this freshly created formatting copy is transformed.
        (formatted_dir / name).write_bytes(formatted)
    hashes = tree_hashes(formatted_dir)
    for name in ax_sources:
        relative = f"{formatted_name}/{name}"
        run([compiler, "format", relative, "--check"], workflow, env,
            timeout=45, expected=b"", record=checks)
        run([compiler, "format", relative], workflow, env,
            timeout=45, expected=(formatted_dir / name).read_bytes(), record=checks)
    need(tree_hashes(formatted_dir) == hashes, "formatted copy changed during idempotence checks")
    need(tree_hashes(workflow / project) == original, "formatter changed original source bytes")
    return formatted_name, hashes


class Delivery:
    def __init__(self, args):
        self.args = args
        for name in ("source", "snapshot", "functional_snapshot", "output_dir", "package_dir",
                     "export_dir", "archive", "installed_dir", "evidence_dir"):
            setattr(self, name, plain_path(getattr(args, name)))
        self.manifest = read_json(self.snapshot / ".snapshot-manifest.json")
        self.manifest_sha256 = checksum(self.snapshot / ".snapshot-manifest.json")
        self.commit = self.manifest.get("source_commit", "")
        self.env = os.environ.copy()
        for key in ("CARGO_TARGET_DIR", "CARGO_ENCODED_RUSTFLAGS", "RUSTC_WRAPPER",
                    "RUSTC_WORKSPACE_WRAPPER", "RUSTFLAGS", "AXEON_TENSOR_RUNTIME",
                    "GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_INDEX_FILE"):
            self.env.pop(key, None)
        self.env.update({"PATH": "/home/Leo/.local/share/axeon/toolchains/rust-1.85.0/rust/bin:"
                                 + self.env.get("PATH", ""),
                         "AXEON_CLANG": "/usr/bin/clang-19", "CARGO_BUILD_JOBS": "1",
                         "RUST_TEST_THREADS": "1", "SOURCE_DATE_EPOCH": "0", "LC_ALL": "C",
                         "CARGO_NET_OFFLINE": "true"})
        dotgit = self.source / ".git"
        if dotgit.is_file():
            pointer = dotgit.read_text(encoding="utf-8").strip()
            need(pointer.startswith("gitdir: "), "invalid source worktree .git file")
            self.gitdir = linux_path(pointer[8:], self.source)
        else:
            self.gitdir = plain_path(dotgit)
        need(self.gitdir.is_dir(), f"Git metadata directory missing: {self.gitdir}")
        self.git_env = self.env.copy()
        self.git_env.update({"GIT_DIR": str(self.gitdir), "GIT_WORK_TREE": str(self.source),
                             "GIT_CONFIG_COUNT": "2", "GIT_CONFIG_KEY_0": "safe.directory",
                             "GIT_CONFIG_VALUE_0": str(self.source), "GIT_CONFIG_KEY_1": "core.autocrlf",
                             "GIT_CONFIG_VALUE_1": "false"})

    def git(self, *args):
        return run(["git", *args], self.source, self.git_env, timeout=30)

    def committed_files(self):
        """Hash Git's exact archive without capturing or logging its payload."""
        command = ["git", "archive", "--format=tar", self.commit]
        # The receipt parent is already verified as ext4 by preconditions.
        # Anonymous owned temporary files keep archive bytes out of both RAM
        # and receipts; neither source tree nor historical evidence is changed.
        with tempfile.TemporaryFile(prefix="axeon-delivery-archive-", dir=self.output_dir.parent) as packed, \
                tempfile.TemporaryFile(prefix="axeon-delivery-git-stderr-", dir=self.output_dir.parent) as errors:
            process = subprocess.Popen(owned_command(command), cwd=self.source, env=self.git_env,
                                       stdin=subprocess.DEVNULL, stdout=packed, stderr=errors,
                                       start_new_session=True, preexec_fn=limits)
            try:
                code = process.wait(timeout=30)
            except BaseException:
                cancel_supervisor(process)
                raise
            if code != 0:
                errors.seek(0)
                detail = errors.read(MAX_DIAGNOSTIC).decode("utf-8", errors="replace")
                raise RuntimeError(f"Git archive exited {code}; bounded stderr: {detail}")
            need(os.fstat(errors.fileno()).st_size == 0, "Git archive produced unexpected stderr")
            need(0 < os.fstat(packed.fileno()).st_size <= MAX_FILE, "Git archive byte budget exceeded")
            packed.seek(0)
            hashes, modes, total, entries = {}, {}, 0, 0
            with tarfile.open(fileobj=packed, mode="r|") as archive:
                for member in archive:
                    entries += 1
                    need(entries <= 16384, "Git archive entry budget exceeded")
                    relative = PurePosixPath(member.name)
                    need(not relative.is_absolute() and ".." not in relative.parts,
                         f"invalid committed path: {member.name}")
                    need(member.isdir() or member.isfile(), f"nonregular committed entry: {member.name}")
                    if member.isfile():
                        need(member.name not in hashes and 0 <= member.size <= MAX_FILE,
                             f"duplicate/oversized archive member: {member.name}")
                        total += member.size
                        need(total <= MAX_TREE, "Git archive source byte budget exceeded")
                        with archive.extractfile(member) as content:
                            hashes[member.name] = hashlib.file_digest(content, "sha256").hexdigest()
                        # Git tracks a regular file's executable bit, not its
                        # group-write permissions (tar.umask may affect those).
                        modes[member.name] = "100755" if member.mode & 0o111 else "100644"
        return hashes, modes

    def ext4(self, path):
        plain_path(path)
        existing = path
        while not existing.exists():
            existing = existing.parent
        filesystem = run(["stat", "-f", "-c", "%T", str(existing)], self.snapshot,
                         self.env, timeout=10).decode().strip()
        need(filesystem in ("ext2/ext3", "ext4"), f"ext4 build/workflow storage required: {path}: {filesystem}")

    def index(self, root):
        manifest = read_json(root / ".snapshot-manifest.json")
        result = {}
        for entry in manifest["files"]:
            path, digest = entry["path"], entry["sha256"]
            relative = PurePosixPath(path)
            need(not relative.is_absolute() and ".." not in relative.parts
                 and path == relative.as_posix() and "\\" not in path and path not in result,
                 f"invalid snapshot manifest path: {path}")
            need(re.fullmatch(r"[0-9a-f]{64}", digest) is not None, f"invalid manifest hash: {path}")
            need(checksum(root / path) == digest, f"snapshot source bytes changed: {root / path}")
            result[path] = digest
        return manifest, result

    def source_match(self):
        delivery_manifest, delivery = self.index(self.snapshot)
        _, functional = self.index(self.functional_snapshot)
        allowed = set(self.args.allow_post_validation)
        changed = {path for path in delivery.keys() | functional.keys()
                   if delivery.get(path) != functional.get(path)}
        need(changed <= allowed, f"unapproved post-validation source changes: {sorted(changed - allowed)}")
        for path in allowed:
            need(path in changed, f"unused post-validation allowance: {path}")
            need(path in delivery, f"post-validation deletion is unsupported: {path}")
            helper = path in DELIVERY_HELPER_CHANGES
            prose = (path == "README.md" or (path.startswith("docs/") and path.endswith(".md"))
                     or path in ("docs/architecture-coverage.json", "docs/support.json")
                     or (path.startswith("evidence/0.14/")
                         and Path(path).suffix in (".json", ".md", ".txt", ".log", ".csv", ".tsv")))
            need(helper or prose, f"only the two reviewed delivery helpers or prose records may postdate validation: {path}")
        need(delivery.get("docs/diagnostic-explanations.tsv")
             == functional.get("docs/diagnostic-explanations.tsv") is not None,
             "offline explanations must match the functional validation source")
        # Git archive is also what reproducibility.py builds. This comparison
        # catches clean/smudge and CRLF differences hidden by Git status.
        archived, archive_modes = self.committed_files()
        need(archived == delivery, "committed Git bytes differ from delivery snapshot manifest")
        expected_modes = {entry["path"]: entry["mode"] for entry in delivery_manifest["files"]}
        need(all(mode in ("100644", "100755") for mode in expected_modes.values())
             and archive_modes == expected_modes, "committed Git modes differ from delivery snapshot manifest")
        for directory in ("docs", "examples", "runtime"):
            expected = {p[len(directory) + 1:]: h for p, h in delivery.items()
                        if p.startswith(directory + "/")}
            need(tree_hashes(self.snapshot / directory) == expected,
                 f"unlisted/mutated package ancillary inputs: {directory}")
        return {"unchanged_files": len(delivery.keys() & functional.keys() - changed),
                "post_validation_changes": {path: {"functional_sha256": functional.get(path),
                                                   "delivery_sha256": delivery[path]}
                                            for path in sorted(changed)},
                "committed_archive_matches_exact_snapshot_bytes": True,
                "committed_archive_matches_snapshot_git_modes": True,
                "functional_manifest_sha256": checksum(self.functional_snapshot / ".snapshot-manifest.json"),
                "delivery_manifest_sha256": checksum(self.snapshot / ".snapshot-manifest.json")}

    def preconditions(self):
        need(sys.platform == "linux", "delivery requires the approved Debian/WSL host")
        need(Path.cwd() == self.snapshot, "run from the immutable delivery snapshot")
        need(Path(__file__).resolve() == self.snapshot / "scripts/deliver-generic-nominals.py",
             "run the frozen delivery helper, not a mutable checkout copy")
        need(re.fullmatch(r"[0-9a-f]{40}", self.commit) is not None, "complete source commit required")
        need(self.manifest.get("name") == self.snapshot.name and self.manifest.get("status") == [],
             "delivery snapshot must capture a clean committed checkout")
        need(checksum(self.snapshot / ".snapshot-manifest.json") == self.manifest_sha256,
             "delivery snapshot manifest changed during a stage")
        need(self.git("rev-parse", "--verify", "HEAD").decode().strip() == self.commit,
             "source checkout HEAD differs from delivery snapshot")
        for path in (self.snapshot, self.functional_snapshot, self.output_dir,
                     self.package_dir, self.installed_dir):
            self.ext4(path)
        validation = read_json(self.snapshot / "evidence/0.14/validation.json")
        need((validation.get("checkpoint"), validation.get("compiler"), validation.get("language_subset"))
             == (CHECKPOINT, VERSION, SUBSET), "wrong current functional acceptance identity")
        need(validation.get("status") == "passed"
             and validation.get("source_snapshot") == self.functional_snapshot.name,
             "functional validation is missing or identifies a different snapshot")
        need(validation.get("source_commit") in (None, self.commit), "conflicting validation source commit")
        gate = validation["workspace_gate"]
        need(gate.get("status") == "passed" and gate.get("failures") == 0
             and gate.get("ignored") == 0 and type(gate.get("tests_total_both_profiles")) is int
             and gate["tests_total_both_profiles"] > 0 and gate.get("format_check") == "passed"
             and gate.get("clippy_with_warnings_denied") == "passed", "current workspace gate is incomplete")
        for name in ("source_oracle", "library_oracle", "native_examples", "profiling", "compatibility"):
            need(validation.get(name, {}).get("status") == "passed", f"current {name} acceptance is missing")
        rust = run(["rustc", "--version"], self.snapshot, self.env, timeout=10).decode().strip()
        clang = run([self.env["AXEON_CLANG"], "--version"], self.snapshot, self.env, timeout=10).decode().splitlines()[0]
        need(rust.startswith("rustc 1.85.0 "), "pinned Rust 1.85.0 required")
        need(re.search(r"\bclang version 19\.1\.7(?:\s|$)", clang), "pinned Clang 19.1.7 required")
        match = self.source_match()
        return {"status": "passed", "checkpoint": CHECKPOINT, "compiler": VERSION, "language_subset": SUBSET,
                "source_commit": self.commit, "source_snapshot": str(self.snapshot),
                "functional_snapshot": str(self.functional_snapshot), "git_directory": str(self.gitdir),
                "destinations": {name: str(getattr(self, name)) for name in
                                 ("output_dir", "package_dir", "export_dir", "archive", "installed_dir", "evidence_dir")},
                "rust": rust, "clang": clang, "source_comparison": match,
                "functional_tests_total_both_profiles": gate["tests_total_both_profiles"],
                "expected_reproduced_artifacts_per_build": 60, "expected_package_checks": 39,
                "helper_sha256": checksum(Path(__file__)),
                "limits": {"cargo_jobs": 1, "test_threads": 1, "address_space_bytes": 2 << 30,
                           "process_limit": 256, "timeout_cleanup": "private subreaper and pidfd descendant cleanup",
                           "cleanup_seconds": CLEANUP_SECONDS,
                           "cancellation_wait_seconds": CLEANUP_SECONDS + 5,
                           "emergency_cleanup_seconds": CLEANUP_SECONDS + 5,
                           "pipe_drain_seconds": 5,
                           "cleanup_process_limit": MAX_OWNED_PROCESSES,
                           "emergency_cleanup_marks_failure": True}}

    def preflight(self):
        result = self.preconditions()
        fresh(self.output_dir)
        for path in (self.package_dir, self.export_dir, self.archive, self.installed_dir,
                     self.snapshot / "target"):
            fresh(path)
        need(self.evidence_dir.is_dir(), "create the current acceptance evidence directory before delivery")
        for filename in ("clean-build.json", "clean-build.log", "delivery-build.json", "package.json",
                         "delivery-export.json", "installed-workflow.json"):
            fresh(self.evidence_dir / filename)
        self.output_dir.mkdir()
        write_new(self.output_dir / "delivery-preconditions.json", result)

    def reproduction(self):
        receipt = read_json(self.output_dir / "clean-build/clean-build.json")
        need(receipt.get("exit_code") == 0 and receipt.get("source_commit") == self.commit,
             "reproduction identity/status mismatch")
        for key in ("includes_optional_numerical_library", "includes_tensor_runtime_archive",
                    "includes_local_modules", "includes_local_records", "includes_local_unions",
                    "includes_aggregate_interfaces", "includes_nested_aggregates", "includes_unit_values",
                    "includes_static_traits", "includes_generic_functions", "includes_generic_nominals",
                    "native_execution_verified",
                    "compiler_objects_executables_and_metadata_byte_identical"):
            need(receipt.get(key) is True, f"reproduction missing {key}")
        need(receipt.get("independent_builds") == 2 and receipt.get("shared_target_directory") is False,
             "two independent fresh builds required")
        builds = receipt["builds"]
        need(len(builds) == 2 and {build["build"] for build in builds} == {0, 1}
             and all(len(build["sha256"]) == 60 for build in builds), "60 artifacts per distinct build required")
        need(builds[0]["sha256"] == builds[1]["sha256"]
             and builds[0]["native_outputs"] == builds[1]["native_outputs"], "independent build results differ")
        for build in builds:
            for artifact in ("generic-nominals.o", "generic-nominals.o.json",
                             "generic-nominals", "generic-nominals.json"):
                need(re.fullmatch(r"[0-9a-f]{64}", build["sha256"].get(artifact, "")) is not None,
                     f"generic nominal reproduction artifact missing or invalid: {artifact}")
            for name, expected in (("trait-modules", ORDINARY_OUTPUT), ("generic-modules", GENERIC_OUTPUT),
                                   ("generic-nominals", GENERIC_NOMINAL_OUTPUT)):
                result = build["native_outputs"][name]
                need((result["exit_code"], result["stdout"].encode(), result["stderr"])
                     == (0, expected, ""), f"incorrect reproduced {name} output")
        return receipt

    def reproduce(self):
        fresh(self.output_dir / "clean-build")
        run([sys.executable, "scripts/reproducibility.py", *FLAGS, "--output-dir",
             str(self.output_dir / "clean-build")], self.snapshot, self.git_env, timeout=600)
        self.reproduction()

    def delivery_build(self):
        receipt = read_json(self.output_dir / "delivery-build.json")
        need(receipt.get("status") == "passed" and receipt.get("source_commit") == self.commit
             and receipt.get("matches_both_reproduction_builds") is True
             and receipt.get("source_and_tests_match_functional_snapshot") == self.functional_snapshot.name,
             "delivery build identity/acceptance mismatch")
        need(set(receipt["sha256"]) == set(BINARY_KEYS), "unexpected delivery artifact set")
        repro = self.reproduction()
        for key, digest in receipt["sha256"].items():
            need(checksum(self.snapshot / key) == digest
                 and all(build["sha256"][key] == digest for build in repro["builds"]),
                 f"delivery bytes differ from independent builds: {key}")
        return receipt

    def build(self):
        repro = self.reproduction()
        fresh(self.output_dir / "delivery-build.json")
        fresh(self.snapshot / "target")
        env = self.env.copy()
        env["RUSTFLAGS"] = f"--remap-path-prefix={self.snapshot}=/axeon"
        env["AXEON_TENSOR_RUNTIME"] = str(self.snapshot / BINARY_KEYS[1])
        commands = []
        run(["cargo", "build", "--offline", "--locked", "--release", "--jobs", "1"],
            self.snapshot, env, timeout=180, record=commands)
        hashes = {key: checksum(self.snapshot / key) for key in BINARY_KEYS}
        need(all(all(build["sha256"][key] == digest for key, digest in hashes.items())
                 for build in repro["builds"]), "third build differs from both independent builds")
        write_new(self.output_dir / "delivery-build.json", {
            "status": "passed", "source_commit": self.commit,
            "source_and_tests_match_functional_snapshot": self.functional_snapshot.name,
            "fresh_target_directory": True, "path_remapping": "/axeon",
            "build_wall_seconds": commands[0]["wall_seconds"], "commands": commands,
            "sha256": hashes, "matches_both_reproduction_builds": True})

    def package_receipt(self):
        receipt = read_json(self.package_dir / "package-receipt.json")
        build = self.delivery_build()
        need(receipt.get("status") == "passed" and receipt.get("source_commit") == self.commit,
             "package identity/status mismatch")
        need((receipt.get("compiler_version"), receipt.get("language_subset"), receipt.get("tensor_runtime_version"))
             == (VERSION, SUBSET, "0.1.0") and receipt.get("version_output", "").encode() == VERSION_OUTPUT,
             "unexpected package compiler identity")
        need(receipt.get("matches_both_independent_builds") is True and len(receipt["checks"]) == 39
             and all(check["passed"] is True and check["exit_code"] == 0
                     and check["timed_out"] is False for check in receipt["checks"]),
             "39 successful package checks required")
        generic_checks = {check["name"]: check for check in receipt["checks"]
                          if check["name"].startswith("generic-nominals-")}
        expected_generic = {"generic-nominals-check": b"",
                            "generic-nominals-interpret": GENERIC_NOMINAL_OUTPUT,
                            "generic-nominals-run": GENERIC_NOMINAL_OUTPUT,
                            "generic-nominals-test": GENERIC_NOMINAL_TEST_OUTPUT}
        need(set(generic_checks) == set(expected_generic)
             and len({check["name"] for check in receipt["checks"]}) == 39,
             "package must contain distinct checks including every generic nominal command")
        for name, output in expected_generic.items():
            need(generic_checks[name].get("stdout", "").encode() == output
                 and generic_checks[name].get("stderr") == "",
                 f"package generic nominal check output differs: {name}")
        need(checksum(self.package_dir / "reproduction.json") == receipt["reproduction_sha256"]
             == checksum(self.output_dir / "clean-build/clean-build.json"), "packaged reproduction differs")
        for filename, key in (("axeon", "compiler_sha256"), ("libaxeon_tensor_runtime.a", "tensor_runtime_sha256")):
            need(checksum(self.package_dir / "bin" / filename) == receipt[key]
                 == build["sha256"]["target/release/" + filename], "packaged binary bytes differ")
        hashes, listed = tree_hashes(self.package_dir), {}
        for line in (self.package_dir / "SHA256SUMS").read_text().splitlines():
            digest, relative = line.split("  ", 1)
            need(re.fullmatch(r"[0-9a-f]{64}", digest) and relative not in listed, "invalid package manifest")
            listed[relative] = digest
        need(listed == {p: h for p, h in hashes.items() if p != "SHA256SUMS"}, "package manifest/hash mismatch")
        return receipt, hashes

    def package(self):
        self.delivery_build()
        fresh(self.package_dir)
        run([sys.executable, "scripts/package-toolchain.py", "--compiler", BINARY_KEYS[0],
             "--tensor-runtime", BINARY_KEYS[1], "--reproduction",
             str(self.output_dir / "clean-build/clean-build.json"), "--output-dir", str(self.package_dir)],
            self.snapshot, self.env, timeout=240)
        self.package_receipt()

    def copy_verified(self, source, destination):
        digest = checksum(source)
        fresh(destination)
        with source.open("rb") as incoming, destination.open("xb") as outgoing:
            shutil.copyfileobj(incoming, outgoing)
        need(checksum(source) == checksum(destination) == digest, f"copy changed: {source}")
        return digest

    def export(self):
        package, files = self.package_receipt()
        copies = [(self.output_dir / "clean-build/clean-build.json", "clean-build.json"),
                  (self.output_dir / "clean-build/clean-build.log", "clean-build.log"),
                  (self.output_dir / "delivery-build.json", "delivery-build.json"),
                  (self.package_dir / "package-receipt.json", "package.json")]
        for path in (self.export_dir, self.archive, self.evidence_dir / "delivery-export.json",
                     *(self.evidence_dir / name for _, name in copies)):
            fresh(path)
        shutil.copytree(self.package_dir, self.export_dir, symlinks=False)
        need(tree_hashes(self.export_dir) == files, "exported package bytes differ")
        with self.archive.open("xb") as raw:
            with gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=0) as compressed:
                with tarfile.open(fileobj=compressed, mode="w|", format=tarfile.PAX_FORMAT) as archive:
                    for path in [self.package_dir, *sorted(self.package_dir.rglob("*"))]:
                        need(not path.is_symlink() and (path.is_dir() or path.is_file()), f"invalid archive entry: {path}")
                        info = archive.gettarinfo(str(path), arcname=(Path(self.package_dir.name)
                                                   / path.relative_to(self.package_dir)).as_posix())
                        info.uid = info.gid = info.mtime = 0
                        info.uname = info.gname = ""
                        if path.is_file():
                            with path.open("rb") as content:
                                archive.addfile(info, content)
                        else:
                            archive.addfile(info)
        archived = {}
        with tarfile.open(self.archive, "r:gz") as archive:
            for member in archive:
                relative = Path(member.name).relative_to(self.package_dir.name).as_posix()
                need(member.isdir() or member.isfile(), f"unexpected archive member: {member.name}")
                if member.isfile():
                    need(relative not in archived, f"duplicate archive member: {relative}")
                    with archive.extractfile(member) as content:
                        archived[relative] = hashlib.file_digest(content, "sha256").hexdigest()
        need(archived == files == tree_hashes(self.package_dir) == tree_hashes(self.export_dir),
             "archive/package/export bytes changed")
        copied = {name: self.copy_verified(source, self.evidence_dir / name) for source, name in copies}
        write_new(self.evidence_dir / "delivery-export.json", {
            "status": "passed", "source_commit": self.commit, "linux_package": str(self.package_dir),
            "windows_package": str(self.export_dir), "archive": str(self.archive),
            "archive_sha256": checksum(self.archive), "archive_bytes": self.archive.stat().st_size,
            "copied_file_count": len(files), "actual_package_checks": len(package["checks"]),
            "all_copy_hashes_match": True, "archive_member_hashes_match": True,
            "manifest_verified": True, "repository_copy_sha256": copied,
            "scope": "Local Linux x86-64 package copied to Windows storage; no publication or native Windows claim."})

    def installed(self):
        package, package_hashes = self.package_receipt()
        export = read_json(self.evidence_dir / "delivery-export.json")
        need(export.get("status") == "passed" and export.get("source_commit") == self.commit
             and export.get("all_copy_hashes_match") is True, "export evidence missing or inconsistent")
        fresh(self.installed_dir)
        fresh(self.evidence_dir / "installed-workflow.json")
        self.installed_dir.mkdir()
        original = {}
        for destination, example in (("traits", "trait-modules"), ("generics", "generic-modules"),
                                     ("nominals", "generic-nominals")):
            original[destination] = tree_hashes(self.package_dir / "examples" / example)
            shutil.copytree(self.package_dir / "examples" / example, self.installed_dir / destination)
            need(tree_hashes(self.installed_dir / destination) == original[destination], "installed source copy differs")
        checks = []
        compiler = str(self.package_dir / "bin/axeon")
        starter_test = (b"running 2 tests (reference interpreter; fuel 1000000 per test)\n"
                        b"test test_negative ...\nPASS test_negative\n"
                        b"test test_twice ...\nPASS test_twice\n"
                        b"test result: 2 passed; 0 failed\n")
        trait_test = (b"running 1 tests (reference interpreter; fuel 1000000 per test)\n"
                      b"test test_trait_pipeline ...\nPASS test_trait_pipeline\n"
                      b"test result: 1 passed; 0 failed\n")
        generic_test = (b"running 3 tests (reference interpreter; fuel 1000000 per test)\n"
                        b"test test_empty_packet ...\nPASS test_empty_packet\n"
                        b"test test_nested_packet ...\nPASS test_nested_packet\n"
                        b"test test_scalar_and_unit ...\nPASS test_scalar_and_unit\n"
                        b"test result: 3 passed; 0 failed\n")
        sources, standalone, formatted_sources = {}, {}, {}
        try:
            run([compiler, "--version"], self.installed_dir, self.env, expected=VERSION_OUTPUT, record=checks)
            run([compiler, "new", "project"], self.installed_dir, self.env,
                expected=b"created project project (main.ax, math.ax, tests.ax)\n", record=checks)
            original["project"] = tree_hashes(self.installed_dir / "project")
            for project, expected, tests in (("project", ORDINARY_OUTPUT, starter_test),
                                             ("traits", ORDINARY_OUTPUT, trait_test),
                                             ("generics", GENERIC_OUTPUT, generic_test),
                                             ("nominals", GENERIC_NOMINAL_OUTPUT, GENERIC_NOMINAL_TEST_OUTPUT)):
                main = f"{project}/main.ax"
                commands = [(["check", main], b""), (["interpret", main], expected),
                            *[(["run", main, "--opt", str(opt)], expected) for opt in (0, 2, 3)],
                            (["test", f"{project}/tests.ax"], tests),
                            (["build", main, "--opt", "3", "-o", f"{project}-program"], b"")]
                for args, output in commands:
                    run([compiler, *args], self.installed_dir, self.env, timeout=45, expected=output, record=checks)
                executable = self.installed_dir / f"{project}-program"
                run([str(executable)], self.installed_dir, self.env, timeout=10, expected=expected, record=checks)
                sidecar = read_json(executable.with_name(executable.name + ".json"))
                need(sidecar.get("artifact_sha256") == checksum(executable), "standalone metadata/executable mismatch")
                standalone[project] = {"executable_sha256": checksum(executable),
                                       "metadata_sha256": checksum(executable.with_name(executable.name + ".json"))}
                # Native runs may leave generated build receipts; compare every
                # copied source byte, retaining the complete original source set.
                sources[project] = {name: checksum(self.installed_dir / project / name) for name in original[project]}
                need(sources[project] == original[project], f"installed {project} source changed")

                # Shipped examples intentionally retain their authored layout.
                # Exercise the formatter on a separate complete project copy,
                # preserving the exact original bytes used above for acceptance.
                formatted_name, formatted_before = formatted_copy(
                    compiler, self.installed_dir, project, original[project], self.env, checks)
                formatted_dir = self.installed_dir / formatted_name
                formatted_main = f"{formatted_name}/main.ax"
                format_commands = [(["check", formatted_main], b""),
                                   (["interpret", formatted_main], expected),
                                   (["test", f"{formatted_name}/tests.ax"], tests),
                                   *[(["run", formatted_main, "--opt", str(opt)], expected)
                                     for opt in (0, 2, 3)]]
                for args, output in format_commands:
                    run([compiler, *args], self.installed_dir, self.env, timeout=45,
                        expected=output, record=checks)
                need(tree_hashes(formatted_dir) == formatted_before,
                     f"formatted {project} sources changed during behavior checks")
                formatted_sources[project] = {"directory": formatted_name,
                    "source_sha256": formatted_before, "format_check_passed": True,
                    "formatting_idempotent": True, "behavior_preserved": True,
                    "native_optimization_levels": [0, 2, 3]}
            for project, hashes in sources.items():
                need(hashes == {name: checksum(self.installed_dir / project / name) for name in hashes},
                     f"original {project} sources changed during formatting checks")
            need(tree_hashes(self.package_dir) == package_hashes, "installed package changed during workflow")
        except BaseException as error:
            write_new(self.installed_dir / "failure.json", {"status": "failed", "source_commit": self.commit,
                       "checks": checks, "error": str(error)})
            raise
        write_new(self.evidence_dir / "installed-workflow.json", {
            "status": "passed", "source_commit": self.commit, "cwd": str(self.installed_dir),
            "compiler": compiler, "compiler_sha256": package["compiler_sha256"],
            "source_copy_sha256": sources, "standalone": standalone,
            "formatted_copies": formatted_sources, "original_sources_preserved": True,
            "package_sha256": package_hashes, "package_unchanged_after_checks": True, "checks": checks,
            "scope": "Installed Linux ELF compiler and fresh sources on ext4. Starter, traits, bounded generic functions and finite generic nominals; no Windows or device qualification."})

    def stage(self, name):
        receipt_path = self.output_dir / f"stage-{name}.json"
        if name == "preflight":
            fresh(self.output_dir)
        fresh(receipt_path)
        current = self.preconditions()
        if name != "preflight":
            preflight = read_json(self.output_dir / "delivery-preconditions.json")
            need(preflight.get("source_commit") == self.commit
                 and preflight.get("helper_sha256") == checksum(Path(__file__)), "preflight identity mismatch")
            need(preflight.get("destinations") == current["destinations"]
                 and preflight.get("source_comparison") == current["source_comparison"],
                 "stage destinations/source allowances differ from preflight")
        started = time.monotonic()
        try:
            getattr(self, name)()
            self.preconditions()
        except BaseException as error:
            if self.output_dir.is_dir():
                write_new(receipt_path, {"status": "failed", "stage": name, "source_commit": self.commit,
                           "wall_seconds": time.monotonic() - started, "error": str(error)})
            raise
        write_new(receipt_path, {"status": "passed", "stage": name, "source_commit": self.commit,
                   "wall_seconds": time.monotonic() - started})
        print(json.dumps({"status": "passed", "stage": name, "receipt": str(receipt_path)}), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("preflight", "reproduce", "build", "package", "export", "installed", "all"))
    for name in ("source", "snapshot", "functional-snapshot", "output-dir", "package-dir",
                 "export-dir", "archive", "installed-dir", "evidence-dir"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--allow-post-validation", action="append", default=[], metavar="EXACT_RELATIVE_PATH",
                        help="explicit changed prose/evidence or the reviewed delivery helper/test; never compiler or language tests/build inputs")
    args = parser.parse_args()
    delivery = Delivery(args)
    # Retain the preceding delivery helper's lock name so both generations
    # serialize when given the same receipt parent. Different output parents
    # and other compute gates still require the caller's launch guard.
    need(delivery.output_dir.parent.is_dir(), "output parent must already exist")
    lock_path = delivery.output_dir.parent / ".generic-functions-delivery.lock"
    plain_path(lock_path)
    with lock_path.open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        stages = ("preflight", "reproduce", "build", "package", "export", "installed") if args.stage == "all" else (args.stage,)
        for stage in stages:
            delivery.stage(stage)
        if args.stage == "all":
            write_new(delivery.output_dir / "delivery-pipeline.json", {
                "status": "passed", "source_commit": delivery.commit, "checkpoint": CHECKPOINT,
                "compiler": VERSION, "language_subset": SUBSET, "stages": list(stages),
                "scope": "Local finite generic-nominal delivery; the full architecture remains incomplete."})


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--_supervise":
        status = supervise(sys.argv[2:])
        if status < 0:
            # Preserve Popen's negative signal return code after descendants
            # have been reaped, rather than changing it to shell-style 128+n.
            if -status != signal.SIGKILL:
                signal.signal(-status, signal.SIG_DFL)
            os.kill(os.getpid(), -status)
        raise SystemExit(status)
    main()

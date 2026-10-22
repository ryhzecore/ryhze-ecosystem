"""Run one command with a fresh retained receipt and unmodified exit status."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import select
import subprocess
import sys
import time

from output_paths import evidence_directory, evidence_path


def cmake_list(value: str) -> list[str]:
    """Decode an evaluated CMake list without shell parsing or slash rewriting."""
    if not value:
        return []
    values, current = [], []
    brackets = 0
    index = 0
    while index < len(value):
        character = value[index]
        if character == "\\" and index + 1 < len(value) and value[index + 1] == ";":
            current.append(";")
            index += 2
            continue
        if character == "[":
            brackets += 1
        elif character == "]":
            brackets -= 1
        if character == ";" and brackets == 0:
            values.append("".join(current))
            current = []
        else:
            current.append(character)
        index += 1
    values.append("".join(current))
    return values


class CommandInterrupted(Exception):
    def __init__(self, signum: int):
        super().__init__(f"Command interrupted by signal {signum}")
        self.signum = signum


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def linux_process(pid: int):
    try:
        fields = (Path("/proc") / str(pid) / "stat").read_text().rsplit(")", 1)[1].split()
        return int(fields[1]), int(fields[19]), fields[0]
    except (OSError, ValueError, IndexError):
        return None


def linux_pids():
    return [int(entry.name) for entry in Path("/proc").iterdir() if entry.name.isdigit()]


def pidfd_ready(fd: int) -> bool:
    poller = select.poll()
    poller.register(fd, select.POLLIN)
    return bool(poller.poll(0))


def wait_pidfds(fds) -> None:
    pending = set(fds)
    poller = select.poll()
    for fd in pending:
        poller.register(fd, select.POLLIN)
    while pending:
        for fd, _events in poller.poll():
            pending.discard(fd)
            poller.unregister(fd)


def stop_owned_tree(process: subprocess.Popen, on_failure=None) -> None:
    """Cancellation owns only this child tree; always join the direct child."""
    if process.poll() is not None:
        return
    if os.name == "nt":
        result = subprocess.run(
            [str(Path(os.environ["SystemRoot"]) / "System32" / "taskkill.exe"),
             "/PID", str(process.pid), "/T", "/F"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=False)
        if result.returncode and process.poll() is None:
            raise RuntimeError("Unable to terminate owned command tree: " +
                               result.stderr.decode(errors="replace"))
        process.wait()
        return
    if not all((hasattr(os, "pidfd_open"), hasattr(signal, "pidfd_send_signal"), hasattr(select, "poll"))):
        raise RuntimeError("Safe Linux tree cancellation requires pidfd_open, pidfd_send_signal and poll")
    # Hold stable handles before signalling. Revalidate start ticks and parent
    # after opening each descendant handle so a reused PID cannot be frozen.
    known = {}
    acquired = set()
    pending = {process.pid: linux_process(process.pid)}
    try:
        while pending:
            for pid, info in pending.items():
                if info is None:
                    continue
                try:
                    fd = os.pidfd_open(pid, 0)
                except ProcessLookupError:
                    continue
                acquired.add(fd)
                observed = linux_process(pid)
                parent_owned = pid == process.pid or (info[0] in known and not pidfd_ready(known[info[0]]))
                if observed is None or observed[:2] != info[:2] or not parent_owned:
                    os.close(fd)
                    acquired.remove(fd)
                    continue
                known[pid] = fd
                if not pidfd_ready(fd):
                    try:
                        signal.pidfd_send_signal(fd, signal.SIGSTOP)
                    except ProcessLookupError:
                        pass
            pending = {}
            for pid in linux_pids():
                info = linux_process(pid)
                if (info is not None and info[0] in known and pid not in known
                        and not pidfd_ready(known[info[0]])):
                    pending[pid] = info
        for fd in reversed(tuple(known.values())):
            if not pidfd_ready(fd):
                try:
                    signal.pidfd_send_signal(fd, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        process.wait()
        wait_pidfds(known.values())
    except BaseException as error:
        if on_failure is not None:
            on_failure(error)
        # A failed safe cancellation must not abandon a frozen owned process.
        # Resume only the handles already acquired and retain them until exit.
        for fd in known.values():
            if not pidfd_ready(fd):
                try:
                    signal.pidfd_send_signal(fd, signal.SIGCONT)
                except ProcessLookupError:
                    pass
        process.wait()
        wait_pidfds(known.values())
        raise
    finally:
        for fd in acquired:
            os.close(fd)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True)
    parser.add_argument("--cmake-launcher", default="")
    parser.add_argument("--cmake-emulator", default="")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("a command after -- is required")
    command = cmake_list(args.cmake_launcher) + cmake_list(args.cmake_emulator) + command
    run_name = args.run.replace("_", "-")
    inherited = os.environ.get("RACE_EVIDENCE_RUN_DIR")
    parent = evidence_path(Path(inherited)) / "tests" / run_name if inherited else None
    output = evidence_directory("ctest-" + run_name, parent=parent)
    command = [str(value).replace("{evidence}", str(output)) for value in command]
    record = {"schema": 1, "kind": "race-command-evidence", "command": command,
              "working_directory": str(Path.cwd()), "evidence_directory": str(output),
              "started_utc": datetime.now(timezone.utc).isoformat(), "status": "running"}
    receipt = output / "command.json"

    def write_receipt() -> None:
        receipt.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")

    received_signal = None

    def interrupted(signum, _frame):
        nonlocal received_signal
        received_signal = signum

    signals = [signal.SIGINT, signal.SIGTERM]
    if hasattr(signal, "SIGBREAK"):
        signals.append(signal.SIGBREAK)
    handlers = {signum: signal.signal(signum, interrupted) for signum in signals}
    write_receipt()
    started = time.monotonic()
    environment = dict(os.environ, RACE_EVIDENCE_RUN_DIR=str(output))
    logs = [output / "stdout.log", output / "stderr.log"]
    code = 127
    process = None
    try:
        with logs[0].open("xb") as stdout, logs[1].open("xb") as stderr:
            if received_signal is not None:
                raise CommandInterrupted(received_signal)
            # Defer asynchronous cancellation until the child handle is owned.
            # This closes the signal window between process creation and assignment.
            process = subprocess.Popen(command, stdout=stdout, stderr=stderr, env=environment)
            record["child_pid"] = process.pid
            write_receipt()
            while process.poll() is None and received_signal is None:
                try:
                    process.wait(timeout=0.2)
                except subprocess.TimeoutExpired:
                    pass
            if received_signal is not None:
                raise CommandInterrupted(received_signal)
            code = process.returncode
        record.update(status="passed" if code == 0 else "failed", exit_code=code)
    except CommandInterrupted as error:
        for signum in signals:
            signal.signal(signum, signal.SIG_IGN)
        record.update(status="interrupted", signal=error.signum)
        code = 128 + error.signum
        if process is not None:
            def cleanup_failed(failure):
                record.update(status="cancellation-failed", error=f"{type(failure).__name__}: {failure}",
                              child_quiescent=False)
                write_receipt()

            try:
                stop_owned_tree(process, on_failure=cleanup_failed)
            except BaseException as failure:
                cleanup_failed(failure)
                # Unsupported safe cleanup retains the direct child until exit.
                process.wait()
                record["direct_child_joined"] = True
                raise
        record.update(exit_code=code, child_quiescent=True)
    except OSError as error:
        record.update(status="failed", error=f"{type(error).__name__}: {error}", exit_code=code)
    finally:
        record.update(finished_utc=datetime.now(timezone.utc).isoformat(),
                      elapsed_seconds=time.monotonic() - started,
                      logs=[{"path": path.name, "sha256": file_sha256(path)}
                            for path in logs if path.is_file()])
        write_receipt()
        for signum, handler in handlers.items():
            signal.signal(signum, handler)
    for stream, path in zip((sys.stdout, sys.stderr), logs):
        if path.is_file():
            with path.open("rb") as source:
                shutil.copyfileobj(source, stream.buffer, 1024 * 1024)
            stream.flush()
    print(f"Command evidence: {output}", flush=True)
    return code


if __name__ == "__main__":
    raise SystemExit(main())

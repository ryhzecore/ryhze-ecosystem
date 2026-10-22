"""Run a finite phase with its deadline plus at most 10 seconds of cleanup.

A private supervisor remains the live session/group leader until the final
signal. Never signal a saved group ID after reaping its leader: besides PID
reuse, Darwin can reject signaling a group containing only zombies with EPERM.
The parent only reaps this supervisor AFTER signaling has finished.

The phase owns descendants remaining in that group, including those retaining
output pipes after the command exits. Intentionally persistent group members
are unsupported. Escaped sessions and system-managed daemons are not targeted.
Callers must reserve the cleanup allowance outside the command's time budget.
No waitid/WNOWAIT dependency is required on older macOS Python installations.
"""

import errno
import json
import math
import os
import select
import signal
import subprocess
import sys
import time


_MAX_MESSAGE = 65536
_SUPERVISION_FAILURE = 125


class _ProtocolError(Exception):
    pass


class _Channel:
    """Bounded private control/status messages, separate from command output."""

    def __init__(self, read_descriptor, write_descriptor):
        self.read_descriptor = read_descriptor
        self.write_descriptor = write_descriptor
        for descriptor in (read_descriptor, write_descriptor):
            os.set_blocking(descriptor, False)
            os.set_inheritable(descriptor, False)
        self.buffer = bytearray()

    def close(self):
        try:
            os.close(self.read_descriptor)
        finally:
            os.close(self.write_descriptor)

    def send(self, message, deadline):
        data = json.dumps(message, ensure_ascii=True, separators=(",", ":")).encode() + b"\n"
        if len(data) > _MAX_MESSAGE:
            raise _ProtocolError("Control message exceeds limit")
        pending = memoryview(data)
        while pending:
            try:
                written = os.write(self.write_descriptor, pending)
                if written == 0:
                    raise _ProtocolError("Control channel closed")
                pending = pending[written:]
            except (BlockingIOError, InterruptedError):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError
                select.select([], [self.write_descriptor], [], remaining)

    def receive(self, deadline):
        while True:
            separator = self.buffer.find(b"\n")
            if separator >= 0:
                frame = bytes(self.buffer[:separator])
                del self.buffer[:separator + 1]
                try:
                    message = json.loads(frame)
                except (UnicodeError, ValueError) as error:
                    raise _ProtocolError("Invalid control message") from error
                if not isinstance(message, dict):
                    raise _ProtocolError("Invalid control message")
                return message
            if len(self.buffer) >= _MAX_MESSAGE:
                raise _ProtocolError("Control message exceeds limit")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError
            if not select.select([self.read_descriptor], [], [], remaining)[0]:
                raise TimeoutError
            try:
                data = os.read(self.read_descriptor, min(4096, _MAX_MESSAGE - len(self.buffer)))
            except (BlockingIOError, InterruptedError):
                continue
            if not data:
                raise _ProtocolError("Control channel closed")
            self.buffer.extend(data)


def _signal_group(process, signal_number):
    try:
        os.killpg(process.pid, signal_number)
    except ProcessLookupError:
        # Nothing remains in the owned group. Its leader has not been reaped,
        # so this cannot accidentally target a reused PID/group ID.
        return False
    return True


def _request_self_kill(channel, deadline):
    try:
        channel.send({"event": "self_kill"}, deadline)
        return True
    except (OSError, _ProtocolError, TimeoutError):
        return False


def _cleanup(process, channel, seconds, *, interrupted):
    """Finish all group signals before the first supervisor wait/reap."""
    deadline = time.monotonic() + seconds
    succeeded = True
    try:
        if interrupted:
            exists = _signal_group(process, signal.SIGTERM)
            if exists is not False:
                # Keep the supervisor live during TERM grace. It has a callable
                # TERM handler; the exec'd command retains default TERM behavior.
                grace_end = deadline - seconds * 0.2
                while True:
                    remaining = grace_end - time.monotonic()
                    if remaining <= 0:
                        break
                    time.sleep(min(0.05, remaining))
                _signal_group(process, signal.SIGKILL)
        else:
            # The command is already finished (or could not start). Avoid a
            # grace delay on every successful phase; remove remaining children.
            _signal_group(process, signal.SIGKILL)
    except (OSError, KeyboardInterrupt):
        # In particular EPERM is a real cleanup failure, NEVER success. The
        # live supervisor can still kill its OWN current group without relying
        # on a parent-side numeric ID or a per-PID fallback.
        succeeded = False
        _request_self_kill(channel, deadline)
    finally:
        # Closing the private control channel is another self-cleanup request.
        # Command descendants do not inherit it, so EOF cannot be held by them.
        try:
            channel.close()
        except OSError:
            succeeded = False
        try:
            process.wait(timeout=max(0, deadline - time.monotonic()))
        except (OSError, subprocess.TimeoutExpired, KeyboardInterrupt):
            succeeded = False
    return succeeded


def _kill_own_group():
    # Only the internal session leader calls this. No cached external PID/PGID.
    if not os.getpid() == os.getpgrp() == os.getsid(0):
        raise _ProtocolError("Supervisor does not own its session")
    os.killpg(0, signal.SIGKILL)


def _supervise(read_descriptor, write_descriptor):
    # Refuse an accidental direct internal-mode invocation in a caller's group.
    if not os.getpid() == os.getpgrp() == os.getsid(0):
        return _SUPERVISION_FAILURE
    channel = _Channel(read_descriptor, write_descriptor)
    # A callable handler is reset by exec; SIG_IGN would instead leak into the
    # command and prevent ordinary timeout TERM from taking effect.
    signal.signal(signal.SIGTERM, lambda _number, _frame: None)
    try:
        channel.send({"event": "ready", "pid": os.getpid(), "pgid": os.getpgrp()},
                     time.monotonic() + 10)
        command = None
        worker = None
        result_sent = False
        while True:
            try:
                message = channel.receive(time.monotonic() + 0.05)
            except TimeoutError:
                message = None
            if message is not None:
                if message == {"event": "self_kill"}:
                    _kill_own_group()
                if (set(message) != {"event", "command"}
                        or message["event"] != "start" or command is not None
                        or not isinstance(message["command"], list)
                        or not message["command"]
                        or any(not isinstance(part, str) for part in message["command"])):
                    raise _ProtocolError("Invalid start request")
                command = message["command"]
                try:
                    # Keep the worker in this owned group. Neither control FD
                    # reaches it (or its descendants), even across exec.
                    worker = subprocess.Popen(command, close_fds=True)
                except OSError as error:
                    channel.send({"event": "launch_error", "errno": error.errno or errno.EIO},
                                 time.monotonic() + 1)
                else:
                    channel.send({"event": "started"}, time.monotonic() + 1)
            if worker is not None and not result_sent:
                result = worker.poll()
                if result is not None:
                    channel.send({"event": "result", "code": result}, time.monotonic() + 1)
                    result_sent = True
            # Even after the worker was reaped, remain the live group anchor.
    except (Exception, KeyboardInterrupt):
        # Parent EOF, startup/protocol failure and unexpected supervisor errors
        # cannot leave the owned command group behind.
        return _SUPERVISION_FAILURE
    finally:
        try:
            channel.close()
        finally:
            _kill_own_group()


def run(seconds, command, *, cleanup_seconds=10):
    if not math.isfinite(seconds) or seconds < 0:
        raise ValueError("Command deadline must be finite and nonnegative")
    if not math.isfinite(cleanup_seconds) or not 0 <= cleanup_seconds <= 10:
        raise ValueError("Cleanup allowance must be between 0 and 10 seconds")
    if isinstance(command, (str, bytes, os.PathLike)):
        command = [command]
    command = [os.fsdecode(os.fspath(part)) for part in command]
    if not command or any("\0" in part for part in command):
        raise ValueError("Command must contain executable arguments without NUL")
    # Validate the bounded start message before creating a process.
    start = {"event": "start", "command": command}
    if len(json.dumps(start, ensure_ascii=True).encode()) + 1 > _MAX_MESSAGE:
        raise ValueError("Command exceeds control message limit")
    deadline = time.monotonic() + seconds
    command_read, command_write = os.pipe()
    try:
        status_read, status_write = os.pipe()
    except BaseException:
        os.close(command_read)
        os.close(command_write)
        raise
    channel = _Channel(status_read, command_write)
    try:
        process = subprocess.Popen(
            [sys.executable, os.path.abspath(__file__), "--supervisor",
             str(command_read), str(status_write)],
            pass_fds=(command_read, status_write), start_new_session=True, close_fds=True)
    except BaseException:
        channel.close()
        raise
    finally:
        os.close(command_read)
        os.close(status_write)

    result = _SUPERVISION_FAILURE
    supervision_failed = True
    interrupted = False
    start_requested = False
    launch_errno = None
    try:
        ready = channel.receive(deadline)
        if ready != {"event": "ready", "pid": process.pid, "pgid": process.pid}:
            raise _ProtocolError("Invalid supervisor identity")
        # Popen has created the new session, and its live leader confirmed the
        # identity before any user command can start. Never poll/wait it here.
        # Set before writing: even a partially delivered request may reach the
        # supervisor before an interruption. Before this point no worker exists.
        start_requested = True
        channel.send(start, deadline)
        status = channel.receive(deadline)
        if (set(status) == {"event", "errno"} and status["event"] == "launch_error"
                and type(status["errno"]) is int and 0 < status["errno"] < 4096):
            launch_errno = status["errno"]
        else:
            if status != {"event": "started"}:
                raise _ProtocolError("Invalid command-start acknowledgement")
            status = channel.receive(deadline)
            if (set(status) != {"event", "code"} or status["event"] != "result"
                    or type(status["code"]) is not int or not -128 <= status["code"] <= 255):
                raise _ProtocolError("Invalid command result")
            result = status["code"]
            supervision_failed = False
    except (TimeoutError, KeyboardInterrupt):
        result = 124
        interrupted = True
        supervision_failed = False
    except (OSError, _ProtocolError):
        result = _SUPERVISION_FAILURE
    finally:
        cleaned = _cleanup(process, channel, cleanup_seconds,
                           interrupted=interrupted and start_requested)

    if not cleaned:
        result = _SUPERVISION_FAILURE
        supervision_failed = True
        launch_errno = None
    if launch_errno is not None:
        raise OSError(launch_errno, os.strerror(launch_errno))
    if supervision_failed:
        print("Native verification phase supervision or cleanup failed", file=sys.stderr)
    elif interrupted:
        print("Native verification phase interrupted or exceeded its deadline", file=sys.stderr)
    return result


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--supervisor":
        sys.exit(_supervise(int(sys.argv[2]), int(sys.argv[3])))
    sys.exit(run(int(sys.argv[1]), sys.argv[2:]))

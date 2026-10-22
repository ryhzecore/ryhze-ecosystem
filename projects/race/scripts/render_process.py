"""Bounded process/pipe ownership for the test-only software graphics harness."""
from contextlib import contextmanager
import math
import os
import select
import subprocess
import time

if os.name == 'nt':
    import ctypes
    from ctypes import wintypes
    import msvcrt
    _kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    _peek_named_pipe = _kernel32.PeekNamedPipe
    _peek_named_pipe.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                                ctypes.POINTER(wintypes.DWORD),
                                ctypes.POINTER(wintypes.DWORD),
                                ctypes.POINTER(wintypes.DWORD)]
    _peek_named_pipe.restype = wintypes.BOOL


def _windows_pipe_available(fd):
    """A synchronous anonymous pipe with one reader; never consume or block."""
    available = wintypes.DWORD()
    handle = msvcrt.get_osfhandle(fd)
    if not _peek_named_pipe(handle, None, 0, None, ctypes.byref(available), None):
        error = ctypes.get_last_error()
        if error in (109, 232):  # ERROR_BROKEN_PIPE / ERROR_NO_DATA: EOF
            return None
        raise ctypes.WinError(error)
    return available.value


def wait_fixture_readable(proc, timeout):
    """Bounded readiness wait, including EOF. No reader thread or mode mutation."""
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError('readiness timeout must be finite and positive')
    deadline = time.monotonic() + timeout
    fd = proc.stdout.fileno()
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        if os.name == 'nt':
            available = _windows_pipe_available(fd)
            if available is None or available > 0:
                return time.monotonic() < deadline
            time.sleep(min(remaining, 0.005))
        else:
            try:
                ready, _, _ = select.select([fd], [], [], remaining)
                return bool(ready) and time.monotonic() < deadline
            except InterruptedError:
                continue


FIXTURE_LINE_BYTES = 65536  # Includes the terminating newline; UTF-8 BYTE cap.


class FixtureStartupTimeout(TimeoutError):
    def __init__(self, partial):
        self.partial = bytes(partial)
        super().__init__(f'native fixture startup deadline expired after {len(partial)} bytes')


class FixtureStartupError(ValueError):
    def __init__(self, reason, partial):
        self.partial = bytes(partial)
        super().__init__(reason)


def read_fixture_line(proc, timeout=30):
    """Read one complete record without a buffered read or deadline reset.

    This function runs BEFORE graphics starts. It never acknowledges completion.
    At most 64 KiB are retained; a full buffer without a newline cannot be valid.
    """
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError('startup timeout must be finite and positive')
    fd = proc.stdout.fileno()
    blocking = os.get_blocking(fd) if os.name != 'nt' else None
    deadline = time.monotonic() + timeout
    partial = bytearray()
    if os.name != 'nt':
        os.set_blocking(fd, False)
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise FixtureStartupTimeout(partial)
            try:
                if not wait_fixture_readable(proc, remaining) or time.monotonic() >= deadline:
                    raise FixtureStartupTimeout(partial)
                count = min(4096, FIXTURE_LINE_BYTES - len(partial))
                if os.name == 'nt':
                    # Peek avoids select-on-pipe (unsupported on Windows) and
                    # ensures the sole raw reader never issues a blocking read.
                    available = _windows_pipe_available(fd)
                    if available == 0:
                        continue
                    chunk = b'' if available is None else os.read(fd, min(count, available))
                else:
                    chunk = os.read(fd, count)
            except (BlockingIOError, InterruptedError):
                continue
            if not chunk:
                raise FixtureStartupError('native fixture EOF before complete JSON line', partial)
            partial.extend(chunk)
            newline = partial.find(b'\n')
            if newline >= 0:
                if newline != len(partial) - 1:
                    raise FixtureStartupError('unexpected bytes after native fixture JSON line', partial)
                return bytes(partial)
            if len(partial) == FIXTURE_LINE_BYTES:
                raise FixtureStartupError('native fixture JSON line exceeds 64 KiB', partial)
    finally:
        if os.name != 'nt':
            os.set_blocking(fd, blocking)


def close_fixture(proc, timeout=5):
    """Abort/reap the direct native child with a bounded wait; never drain to EOF.

    Native fixture pipes are unbuffered, so closing them cannot flush pending
    buffered input. No communicate-without-timeout or wait-without-timeout occurs.
    This is forced test cleanup, not proof of native quiescence/leak-clean shutdown.
    Graphics must already be reaped before calling this after graphics has begun.
    """
    try:
        if proc.poll() is None:
            proc.kill()
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f'native fixture PID {proc.pid} did not reap within {timeout}s') from error
    finally:
        for pipe in (proc.stdin, proc.stdout, proc.stderr):
            if pipe is not None:
                pipe.close()


@contextmanager
def native_fixture(command):
    # No Python text/buffered reader can consume bytes before the raw startup read.
    proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, bufsize=0)
    try:
        yield proc
    finally:
        close_fixture(proc)

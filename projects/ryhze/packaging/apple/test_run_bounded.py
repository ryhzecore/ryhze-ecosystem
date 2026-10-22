"""Real local process checks; no Apple tools, credentials or builds run."""

from contextlib import redirect_stderr
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

import run_bounded


class BoundedProcessTests(unittest.TestCase):
    def test_normal_failure_and_signalled_command_status_are_preserved(self):
        for expected, code in (
                (0, 'pass'),
                (7, 'raise SystemExit(7)'),
                (125, 'raise SystemExit(125)'),
                (-signal.SIGTERM, 'import os, signal; os.kill(os.getpid(), signal.SIGTERM)')):
            with self.subTest(expected=expected):
                stderr = io.StringIO()
                with redirect_stderr(stderr):
                    self.assertEqual(run_bounded.run(5, [sys.executable, '-c', code]), expected)
                self.assertEqual(stderr.getvalue(), '')

    def test_launch_failure_preserves_oserror_after_owned_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            started = time.monotonic()
            with self.assertRaises(FileNotFoundError):
                run_bounded.run(2, [str(Path(temporary) / 'missing-command')], cleanup_seconds=0.5)
            self.assertLess(time.monotonic() - started, 5)

    def test_exhausted_startup_deadline_does_not_launch_the_command(self):
        with tempfile.TemporaryDirectory() as temporary:
            marker = Path(temporary) / 'must-not-be-created'
            stderr = io.StringIO()
            started = time.monotonic()
            with redirect_stderr(stderr):
                result = run_bounded.run(
                    0, [sys.executable, '-c',
                        'import pathlib, sys; pathlib.Path(sys.argv[1]).write_text("started")',
                        str(marker)], cleanup_seconds=0.4)
            self.assertEqual(result, 124)
            self.assertFalse(marker.exists())
            self.assertLess(time.monotonic() - started, 2)
            self.assertIn('exceeded its deadline', stderr.getvalue())

    def test_repeated_fast_completion_signals_only_unreaped_owned_supervisor(self):
        original = run_bounded._signal_group
        signals = []

        def checked_signal(process, number):
            # Reading returncode does not call poll()/wait() or reap the anchor.
            # A completed command must not release the supervisor's PID before
            # its final group signal, even when the command exits immediately.
            self.assertIsNone(process.returncode)
            self.assertEqual(os.getpgid(process.pid), process.pid)
            self.assertNotEqual(process.pid, os.getpid())
            signals.append(number)
            return original(process, number)

        with patch.object(run_bounded, '_signal_group', side_effect=checked_signal):
            for _ in range(12):
                self.assertEqual(run_bounded.run(5, [sys.executable, '-c', 'pass']), 0)
        self.assertGreaterEqual(signals.count(signal.SIGKILL), 12)

    def test_cleanup_allowance_is_bounded_before_launch(self):
        for seconds in (-0.1, 10.1):
            with self.subTest(seconds=seconds), \
                 patch.object(run_bounded.subprocess, 'Popen') as launch, \
                 self.assertRaises(ValueError):
                run_bounded.run(1, [sys.executable, '-c', 'pass'], cleanup_seconds=seconds)
            launch.assert_not_called()

    def test_group_exit_race_after_owned_kill_preserves_completion(self):
        original = run_bounded._signal_group

        def completed_group(process, number):
            self.assertIsNone(process.returncode)
            self.assertEqual(number, signal.SIGKILL)
            original(process, number)
            # The real owned group has been killed, while its supervisor remains
            # unreaped. Inject only the kernel's gone-group race; never signal a
            # released numeric ID or fabricate ownership of another process.
            with patch.object(run_bounded.os, 'killpg', side_effect=ProcessLookupError):
                self.assertFalse(original(process, number))
            return False

        with patch.object(run_bounded, '_signal_group', side_effect=completed_group):
            self.assertEqual(run_bounded.run(5, [sys.executable, '-c', 'pass']), 0)

    def test_internal_supervisor_refuses_unowned_session_without_any_signal(self):
        with tempfile.TemporaryDirectory(prefix='ryhze-supervisor-guard-') as temporary:
            marker = Path(temporary) / 'signal-attempted'
            command_read, command_write = os.pipe()
            status_read, status_write = os.pipe()
            code = (
                'import os, pathlib, runpy, sys\n'
                'helper, marker, read_fd, write_fd = sys.argv[1:]\n'
                'def forbidden_signal(*args):\n'
                '    pathlib.Path(marker).write_text("unexpected group signal")\n'
                '    raise AssertionError("must not signal an unowned group")\n'
                'os.killpg = forbidden_signal\n'
                'sys.argv = [helper, "--supervisor", read_fd, write_fd]\n'
                'runpy.run_path(helper, run_name="__main__")\n')
            try:
                # Deliberately inherit the caller's session/group. The signal
                # interceptor makes a guard regression harmless to that caller.
                result = subprocess.run(
                    [sys.executable, '-c', code, str(Path(run_bounded.__file__).resolve()),
                     str(marker), str(command_read), str(status_write)],
                    pass_fds=(command_read, status_write), capture_output=True,
                    text=True, timeout=5)
                self.assertEqual(result.returncode, 125, result.stdout + result.stderr)
                self.assertFalse(marker.exists())
                self.assertEqual(result.stdout, '')
            finally:
                for descriptor in (command_read, command_write, status_read, status_write):
                    os.close(descriptor)

    def test_successful_command_cleans_term_resistant_descendant_and_keeps_zero(self):
        self.assert_owned_descendant_cleaned('success')

    def test_timeout_cleans_term_resistant_descendant_and_returns_124(self):
        self.assert_owned_descendant_cleaned('timeout')

    def test_live_group_permission_denial_returns_125_and_supervisor_self_cleans(self):
        for mode in ('permission_denied', 'permission_denied_timeout'):
            with self.subTest(mode=mode):
                self.assert_owned_descendant_cleaned(mode)

    def test_parent_death_closes_control_channel_and_supervisor_cleans_its_group(self):
        self.assert_owned_descendant_cleaned('parent_death')

    def assert_owned_descendant_cleaned(self, mode):
        with tempfile.TemporaryDirectory(prefix='ryhze-owned-group-') as temporary:
            root = Path(temporary).resolve()
            control = root / 'fixture-release.fifo'
            os.mkfifo(control)
            child_pid = root / 'child.pid'
            child = root / 'child.py'
            child.write_text(
                'import os, pathlib, signal, sys\n'
                'signal.signal(signal.SIGTERM, signal.SIG_IGN)\n'
                'release = open(sys.argv[2], "rb")\n'
                'print("descendant ready", flush=True)\n'
                'ready = pathlib.Path(sys.argv[1])\n'
                'pending = ready.with_suffix(".pending")\n'
                'pending.write_text(str(os.getpid()))\n'
                'pending.replace(ready)\n'
                'release.read()\n')
            worker = root / 'worker.py'
            worker.write_text(
                'import pathlib, subprocess, sys, time\n'
                'subprocess.Popen([sys.executable, sys.argv[2], sys.argv[3], sys.argv[4]])\n'
                'while not pathlib.Path(sys.argv[3]).exists(): time.sleep(0.01)\n'
                'if sys.argv[1] in ("success", "permission_denied"): raise SystemExit(0)\n'
                'with open(sys.argv[4], "rb") as release: release.read()\n')
            events = root / 'signals.json'
            wrapper_source = root / 'wrapper.py'
            wrapper_source.write_text(
                'import errno, json, os, pathlib, sys\n'
                'import run_bounded\n'
                'mode, record, child_pid = sys.argv[1:4]\n'
                'original = run_bounded._signal_group\n'
                'events = []\n'
                'def signal_group(process, number):\n'
                '    assert process.returncode is None, "supervisor was reaped before group signal"\n'
                '    assert os.getpgid(process.pid) == process.pid\n'
                '    events.append({"signal": number, "unreaped": True})\n'
                '    if mode.startswith("permission_denied"):\n'
                '        assert pathlib.Path(child_pid).exists(), "live fixture must be ready"\n'
                '        raise PermissionError(errno.EPERM, "synthetic parent group denial")\n'
                '    return original(process, number)\n'
                'run_bounded._signal_group = signal_group\n'
                'try:\n'
                '    result = run_bounded.run(30 if mode == "parent_death" else 2,\n'
                '                             sys.argv[4:], cleanup_seconds=0.5)\n'
                'finally:\n'
                '    pathlib.Path(record).write_text(json.dumps(events))\n'
                'raise SystemExit(result)\n')
            environment = {
                **os.environ, 'PYTHONDONTWRITEBYTECODE': '1',
                'PYTHONPATH': str(Path(__file__).resolve().parent),
            }
            # The test owns this writer; children only open readers. Closing it
            # in finally releases fixtures cooperatively if an assertion fails.
            # It stays open during every assertion, so successful pipe closure
            # must come from the helper's cleanup, not the fixture escape.
            release_fd = os.open(control, os.O_RDWR | os.O_NONBLOCK)
            wrapper = None
            started = time.monotonic()
            try:
                wrapper = subprocess.Popen(
                    [sys.executable, str(wrapper_source), mode, str(events), str(child_pid),
                     sys.executable, str(worker), mode, str(child), str(child_pid), str(control)],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=environment)
                if mode == 'parent_death':
                    ready_deadline = time.monotonic() + 5
                    while not child_pid.exists() and time.monotonic() < ready_deadline:
                        self.assertIsNone(wrapper.poll(), 'Fixture wrapper exited before readiness')
                        time.sleep(0.01)
                    self.assertTrue(child_pid.exists(), 'Fixture must be live before killing its owner')
                    # Popen retains ownership of its unreaped direct child. No
                    # saved command/supervisor PID or process group is targeted.
                    wrapper.kill()
                stdout, stderr = wrapper.communicate(timeout=8)
                expected = {'success': 0, 'timeout': 124, 'permission_denied': 125,
                            'permission_denied_timeout': 125, 'parent_death': -signal.SIGKILL}[mode]
                self.assertEqual(wrapper.returncode, expected, stdout + stderr)
                self.assertIn('descendant ready', stdout)
                self.assertTrue(child_pid.exists())
                self.assertLess(time.monotonic() - started, 10)
                if mode == 'timeout':
                    self.assertIn('exceeded its deadline', stderr)
                if mode == 'success':
                    self.assertNotIn('exceeded its deadline', stderr)
                if mode != 'parent_death':
                    recorded = json.loads(events.read_text())
                    self.assertTrue(recorded)
                    self.assertTrue(all(event['unreaped'] is True for event in recorded))
                # A dead orphan may remain a zombie until init reaps it. It
                # cannot execute or retain inherited stdout/stderr descriptors.
                status = subprocess.run(
                    ['ps', '-o', 'stat=', '-p', child_pid.read_text()],
                    capture_output=True, text=True, timeout=2, check=False)
                self.assertIn(status.returncode, (0, 1))
                self.assertTrue(not status.stdout.strip() or status.stdout.strip().startswith('Z'),
                                status.stdout)
            finally:
                os.close(release_fd)
                if wrapper is not None:
                    # No numeric PID/group cleanup after reap. If still owned,
                    # killing the wrapper closes the supervisor control socket;
                    # the supervisor EOF path cleans its own anchored group.
                    if wrapper.poll() is None:
                        wrapper.kill()
                    wrapper.communicate(timeout=5)


if __name__ == '__main__':
    unittest.main()

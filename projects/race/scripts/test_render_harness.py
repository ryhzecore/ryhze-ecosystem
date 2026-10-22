#!/usr/bin/env python3
"""Device-free regressions for bounded startup and the graphics pixel oracle."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

sys.dont_write_bytecode = True
from render_process import (FIXTURE_LINE_BYTES, FixtureStartupError,
                            FixtureStartupTimeout, native_fixture, read_fixture_line,
                            wait_fixture_readable)
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/graphics'))
from vulkan_triangle import BACKGROUND, validate_input, verify_pixels

POSITIONS = [[-.7,.7,0,1],[.7,.7,0,1],[0,-.7,0,1]]
COLOR = [64/255,160/255,240/255,1]


def partial_stall_scenario():
    # Atomic short write, then a blocking input gate: no sleep and no newline.
    command = [sys.executable, '-u', '-c',
               "import os; os.write(1,b'{\"partial\":'); os.read(0,1)"]
    started = time.monotonic()
    proc = None
    try:
        with native_fixture(command) as proc:
            assert wait_fixture_readable(proc, 5), 'child readiness watchdog'
            blocking = os.get_blocking(proc.stdout.fileno()) if os.name != 'nt' else None
            try:
                read_fixture_line(proc, timeout=1)
            except FixtureStartupTimeout as error:
                assert error.partial == b'{"partial":', error.partial
                assert proc.poll() is None, 'stub must still be stalled, not self-exited'
                if os.name != 'nt':
                    assert os.get_blocking(proc.stdout.fileno()) == blocking
                raise
            raise AssertionError('partial line must not succeed')
    except FixtureStartupTimeout:
        expected_returncode = 1 if os.name == 'nt' else -signal.SIGKILL
        assert proc.returncode == expected_returncode, proc.returncode
        assert all(p.closed for p in (proc.stdin, proc.stdout, proc.stderr))
        if os.name == 'nt':
            assert proc.wait(timeout=0) == expected_returncode  # native handle is signalled
        else:
            try:
                os.waitpid(proc.pid, os.WNOHANG)
            except ChildProcessError:
                pass  # Popen already reaped it: no zombie remains.
            else:
                raise AssertionError('native fixture was not reaped')
    else:
        raise AssertionError('startup did not time out')
    return {'partial_json_timeout': True, 'child_reaped': True, 'pipes_closed': True,
            'returncode': proc.returncode, 'elapsed_seconds': time.monotonic()-started,
            'startup_deadline_seconds': 1, 'cleanup_wait_limit_seconds': 5,
            'graphics_started': False, 'completion_acknowledgement_sent': False}


class StartupTests(unittest.TestCase):
    def read_child(self, code):
        with native_fixture([sys.executable, '-u', '-c', code]) as proc:
            return read_fixture_line(proc, timeout=5)

    def test_partial_json_then_stall_deadline_and_cleanup(self):
        # Independent generous watchdog: a regression to blocking readline fails
        # this test instead of hanging CTest. Kill the isolated process group on
        # watchdog failure so the deliberately stalled grandchild cannot survive.
        with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
            options = ({'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP}
                       if os.name == 'nt' else {'start_new_session': True})
            proc = subprocess.Popen([sys.executable, __file__, '--partial-stall-child'],
                                    stdout=out, stderr=err, **options)
            try:
                code = proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                if os.name == 'nt':
                    # The watchdog helper owns a deliberately stalled child.
                    # Reap its entire tree on regression, not only the helper.
                    subprocess.run(['taskkill', '/PID', str(proc.pid), '/T', '/F'],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   timeout=5, check=False)
                else:
                    os.killpg(proc.pid, signal.SIGKILL)
                proc.wait(timeout=5)
                self.fail('outer 20s watchdog: startup or cleanup did not terminate')
            out.seek(0); err.seek(0)
            self.assertEqual(code, 0, err.read().decode(errors='replace'))
            result = json.loads(out.read())
        self.assertTrue(result['partial_json_timeout'])
        self.assertTrue(result['child_reaped'])
        self.assertTrue(result['pipes_closed'])
        self.assertFalse(result['graphics_started'])
        self.assertFalse(result['completion_acknowledgement_sent'])
        print('partial-stall receipt:', json.dumps(result), flush=True)

    def test_partial_eof_rejects(self):
        with self.assertRaisesRegex(FixtureStartupError, 'EOF'):
            self.read_child("import os; os.write(1,b'{')")

    def test_exact_cap_and_incremental_reads(self):
        # At least16 os.read chunks are required; the terminating newline is byte65536.
        result = self.read_child("import sys; sys.stdout.buffer.write(b'\"'+b'x'*65533+b'\"\\n'); sys.stdout.flush()")
        self.assertEqual(len(result), FIXTURE_LINE_BYTES)
        self.assertEqual(len(json.loads(result)), 65533)

    def test_over_cap_rejects(self):
        with self.assertRaisesRegex(FixtureStartupError, '64 KiB'):
            self.read_child("import sys; sys.stdout.buffer.write(b'x'*65536+b'\\n'); sys.stdout.flush()")

    def test_utf8_cap_counts_bytes(self):
        with self.assertRaisesRegex(FixtureStartupError, '64 KiB'):
            self.read_child("import sys; sys.stdout.buffer.write(('\"'+'é'*32767+'\"\\n').encode()); sys.stdout.flush()")

    def test_trailing_bytes_rejects(self):
        with self.assertRaisesRegex(FixtureStartupError, 'after'):
            self.read_child("import os; os.write(1,b'{}\\nextra')")

    def test_complete_record_cleanup(self):
        with native_fixture([sys.executable, '-u', '-c',
                             "import os; os.write(1,b'{}\\n'); os.read(0,1)"]) as proc:
            self.assertEqual(read_fixture_line(proc, 5), b'{}\n')
        self.assertIsNotNone(proc.returncode)
        self.assertTrue(all(p.closed for p in (proc.stdin,proc.stdout,proc.stderr)))


class PixelOracleTests(unittest.TestCase):
    def test_matching_and_tolerance_clear_colors_reject(self):
        for rgb in [(8,12,20),(9,11,21),(7,13,19)]:
            with self.subTest(rgb=rgb):
                color = [x/255 for x in rgb]+[1]
                with self.assertRaisesRegex(ValueError, 'indistinguishable'):
                    validate_input(128,96,POSITIONS,color)
                with self.assertRaisesRegex(ValueError, 'indistinguishable'):
                    verify_pixels(128,96,POSITIONS,color,bytes(BACKGROUND)*(128*96))

    def test_old_fixture_remains_valid(self):
        validate_input(128,96,POSITIONS,COLOR)

    def test_clear_only_image_cannot_pass(self):
        with self.assertRaisesRegex(RuntimeError, 'interior'):
            verify_pixels(128,96,POSITIONS,COLOR,bytes(BACKGROUND)*(128*96))

    def test_foreground_only_image_cannot_pass(self):
        foreground = bytes(round(x*255) for x in COLOR)
        with self.assertRaisesRegex(RuntimeError, 'background'):
            verify_pixels(128,96,POSITIONS,COLOR,foreground*(128*96))

    def test_readback_size_rejects(self):
        with self.assertRaisesRegex(RuntimeError, 'byte count'):
            verify_pixels(128,96,POSITIONS,COLOR,b'')


if __name__ == '__main__':
    if sys.argv[1:] == ['--partial-stall-child']:
        print(json.dumps(partial_stall_scenario()))
    else:
        unittest.main(verbosity=2)

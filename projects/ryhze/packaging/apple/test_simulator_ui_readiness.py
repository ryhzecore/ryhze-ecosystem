"""Host receipt fixtures only; no simulator, Flutter engine or visual acceptance."""

import json
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

import simulator_ui_readiness as probe


class ReadinessTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.request = self.root / 'request.json'
        self.nonce = probe.prepare(self.request)
        self.launch = self.root / 'launch.log'
        self.launch.write_text('com.ryhze.ryhze: 1234\n')
        self.container = self.root / 'container.txt'
        self.container.write_text(str(self.root) + '\n')
        self.marker = self.root / probe.MARKER
        self.marker.parent.mkdir(parents=True)
        self.arguments = (self.request, self.launch, self.container)
        self.record = {'schemaVersion': 1, 'nonce': self.nonce, 'processID': 1234,
                       'state': 'ready', 'bootstrap': 'healthy', 'shellLayout': True,
                       'firstFrameRasterized': True, 'failureCode': None}
        self.screenshot = self.root / 'screen.png'
        # A valid synthetic header, deliberately NOT claimed to be visual proof.
        header = b'IHDR' + struct.pack('>IIBBBBB', 390, 844, 8, 6, 0, 0, 0)
        self.screenshot.write_bytes(b'\x89PNG\r\n\x1a\n' + struct.pack('>I', 13)
                                    + header + struct.pack('>I', zlib.crc32(header))
                                    + b'\x00\x00\x00\x00IEND\xaeB`\x82')

    def write(self, **changes):
        self.marker.write_text(json.dumps({**self.record, **changes}))

    def test_request_exclusive_and_ready_bound_to_exact_launch(self):
        with self.assertRaises(FileExistsError):
            probe.prepare(self.request)
        self.write()
        result = probe.wait_ready(*self.arguments, 0.1)
        self.assertTrue(result['launchBound'])
        self.assertEqual(result['processID'], 1234)
        self.assertTrue(result['visualReviewRequired'])
        self.assertFalse(result['physicalDeviceAcceptance'])

    def test_wait_observes_condition_without_fixed_startup_delay(self):
        elapsed = [0.0]
        self.write(state='starting', bootstrap='pending', shellLayout=False,
                   firstFrameRasterized=False)

        def pause(seconds):
            self.assertLessEqual(seconds, 0.1)
            elapsed[0] += seconds
            self.write()

        result = probe.wait_ready(*self.arguments, 80,
                                  clock=lambda: elapsed[0], pause=pause)
        self.assertEqual(result['state'], 'ready')
        self.assertEqual(elapsed[0], 0.1)

    def test_missing_pending_stale_nonce_and_wrong_pid_expire(self):
        for edit, reason in ((None, 'missing'), ({'nonce': '0' * 32}, 'stale'),
                             ({'processID': 1235}, 'stale'),
                             ({'state': 'starting'}, 'starting')):
            with self.subTest(edit=edit):
                self.marker.unlink(missing_ok=True)
                if edit is not None:
                    self.write(**edit)
                elapsed = [0.0]
                with self.assertRaisesRegex(TimeoutError, 'readiness_timeout_' + reason):
                    probe.wait_ready(*self.arguments, 0.25,
                                     clock=lambda: elapsed[0],
                                     pause=lambda seconds: elapsed.__setitem__(0, elapsed[0] + seconds))
                self.assertEqual(elapsed[0], 0.25)

    def test_failures_and_inconsistent_ready_are_never_success(self):
        for failure in probe.FAILURES:
            self.write(state='failed', failureCode=failure)
            with self.subTest(failure=failure), self.assertRaisesRegex(probe.ProbeFailure, 'app_' + failure):
                probe.wait_ready(*self.arguments, 0.1)
        for edit in ({'bootstrap': 'degraded'}, {'bootstrap': 'pending'},
                     {'shellLayout': False}, {'firstFrameRasterized': False},
                     {'failureCode': 'flutter-error'}):
            self.write(**edit)
            with self.subTest(edit=edit), self.assertRaisesRegex(probe.ProbeFailure, 'inconsistent'):
                probe.wait_ready(*self.arguments, 0.1)

    def test_invalid_types_and_unknown_fields_fail_closed(self):
        for edit in ({'schemaVersion': True}, {'processID': True}, {'processID': -1},
                     {'shellLayout': 1}, {'firstFrameRasterized': 'true'},
                     {'nonce': 'unsafe'}, {'state': 'success'}, {'failureCode': 'secret'},
                     {'bootstrap': 'unknown'}, {'extra': 'private data'}):
            self.write(**edit)
            with self.subTest(edit=edit), self.assertRaises(probe.ProbeFailure):
                probe.wait_ready(*self.arguments, 0.1)
        self.marker.write_text('{"state":"failed","state":"ready"}')
        with self.assertRaisesRegex(probe.ProbeFailure, 'ambiguous_evidence'):
            probe.wait_ready(*self.arguments, 0.1)

    def test_launch_must_have_one_correct_positive_pid(self):
        self.write()
        for text in ('', 'com.other.app: 1234', 'com.ryhze.ryhze: 0',
                     'com.ryhze.ryhze: -1', 'com.ryhze.ryhze: 2147483648',
                     'com.ryhze.ryhze: 1234\ncom.ryhze.ryhze: 1234\n'):
            self.launch.write_text(text)
            with self.subTest(text=text), self.assertRaisesRegex(probe.ProbeFailure, 'invalid_launch_identity'):
                probe.wait_ready(*self.arguments, 0.1)

    def test_late_failure_after_capture_rejects_previously_ready_app(self):
        self.write()
        probe.wait_ready(*self.arguments, 0.1)
        self.write(state='failed', failureCode='flutter-error')
        with self.assertRaisesRegex(probe.ProbeFailure, 'app_flutter-error'):
            probe.capture_report(*self.arguments, self.screenshot)

    def test_capture_receipt_preserves_visual_review_boundary(self):
        self.write()
        result = probe.capture_report(*self.arguments, self.screenshot)
        self.assertEqual((result['captureWidth'], result['captureHeight']), (390, 844))
        self.assertEqual(len(result['captureSHA256']), 64)
        self.assertTrue(result['visualReviewRequired'])
        self.assertIn('content needs visual review', result['captureCheck'])
        self.screenshot.write_bytes(b'blank or failed screenshot output')
        with self.assertRaisesRegex(probe.ProbeFailure, 'invalid_capture'):
            probe.capture_report(*self.arguments, self.screenshot)

    def test_bounded_marker_read_rejects_symlinks_and_oversize(self):
        self.marker.symlink_to(self.request)
        with self.assertRaises(OSError):
            probe.wait_ready(*self.arguments, 0.1)
        self.marker.unlink()
        self.marker.write_bytes(b'x' * 4097)
        with self.assertRaisesRegex(probe.ProbeFailure, 'invalid_evidence_file'):
            probe.wait_ready(*self.arguments, 0.1)
        for timeout in (0, -1, 81):
            with self.subTest(timeout=timeout), self.assertRaisesRegex(probe.ProbeFailure, 'invalid_wait_bound'):
                probe.wait_ready(*self.arguments, timeout)


if __name__ == '__main__':
    unittest.main()

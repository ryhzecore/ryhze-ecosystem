import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from capture_test_evidence import capture


class CaptureEvidenceTests(unittest.TestCase):
    def test_quarantined_candidates_keep_full_paths_and_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = root / 'build/apple-unsigned-validation/golden-candidates/macos'
            for name in ['apple/menu.png', 'v060-shell/menu.png', 'capture.json']:
                target = base / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(name.encode())
            receipt = capture(root, root / 'capture.zip')
            self.assertEqual(len(receipt['files']), 3)
            with zipfile.ZipFile(root / 'capture.zip') as archive:
                for name in ['apple/menu.png', 'v060-shell/menu.png', 'capture.json']:
                    full = 'build/apple-unsigned-validation/golden-candidates/macos/' + name
                    self.assertEqual(archive.read(full), name.encode())

    def test_failure_and_geometry_bytes_keep_paths_without_visual_acceptance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            failure = root / 'test/failures/menu_testImage.png'
            geometry = root / 'test/golden-diagnostics/menu.json'
            failure.parent.mkdir(parents=True)
            geometry.parent.mkdir(parents=True)
            failure.write_bytes(b'synthetic image bytes')
            geometry.write_text('{"fixture":true}')
            output = root / 'capture.zip'
            receipt = capture(root, output)
            self.assertEqual(len(receipt['files']), 2)
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(archive.read('test/failures/menu_testImage.png'), failure.read_bytes())
                self.assertEqual(archive.read('test/golden-diagnostics/menu.json'), geometry.read_bytes())
                self.assertEqual(json.loads(archive.read('manifest.json')), receipt)
            with self.assertRaises(FileExistsError):
                capture(root, output)

    def test_linked_evidence_cannot_export_an_unrelated_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / 'unrelated'
            target.write_bytes(b'preserve')
            failure = root / 'test/failures/menu_testImage.png'
            failure.parent.mkdir(parents=True)
            failure.symlink_to(target)
            with self.assertRaises(ValueError):
                capture(root, root / 'capture.zip')
            self.assertFalse((root / 'capture.zip').exists())
            self.assertEqual(target.read_bytes(), b'preserve')

    def test_size_limit_is_checked_before_creating_archive(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            failure = root / 'test/failures/menu_testImage.png'
            failure.parent.mkdir(parents=True)
            failure.write_bytes(b'12345')
            with self.assertRaises(ValueError):
                capture(root, root / 'capture.zip', byte_limit=4)
            self.assertFalse((root / 'capture.zip').exists())

    def test_file_growth_after_preflight_removes_only_new_partial_archive(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            failure = root / 'test/failures/menu_testImage.png'
            failure.parent.mkdir(parents=True)
            failure.write_bytes(b'1')
            constructor = zipfile.ZipFile

            def grow_then_open(*args, **kwargs):
                failure.write_bytes(b'123456')
                return constructor(*args, **kwargs)

            with patch('capture_test_evidence.zipfile.ZipFile', grow_then_open):
                with self.assertRaises(ValueError):
                    capture(root, root / 'capture.zip', byte_limit=4)
            self.assertFalse((root / 'capture.zip').exists())
            self.assertEqual(failure.read_bytes(), b'123456')

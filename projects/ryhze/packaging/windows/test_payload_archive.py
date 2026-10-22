import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
import zipfile
from payload_archive import archive_payload


class PayloadArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bundle = self.root / "bundle"
        self.bundle.mkdir()
        # Labeled synthetic input; this is NOT a Windows executable/build test.
        (self.bundle / "ryhze.exe").write_bytes(b"synthetic-test-input")
        (self.bundle / "data").mkdir()
        (self.bundle / "data" / "asset.txt").write_text("asset")
        self.lock = self.root / "pubspec.lock"
        self.lock.write_text("packages: {}\n")

    def create(self, output):
        return archive_payload(self.bundle, output, version="0.6.0+3",
                               source="a" * 40, lockfile=self.lock)

    def test_identical_payloads_ignore_input_mtime_and_host_mode(self):
        first = self.root / "first.zip"
        second = self.root / "second.zip"
        self.create(first)
        os.utime(self.bundle / "ryhze.exe", (1234567890, 1234567890))
        (self.bundle / "ryhze.exe").chmod(0o700)
        self.create(second)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        with zipfile.ZipFile(first) as archive:
            manifest = json.loads(archive.read("provenance.json"))
            self.assertEqual(manifest["artifact_kind"], "unsigned-application-payload")
            for entry in manifest["files"]:
                data = archive.read("app/" + entry["path"])
                self.assertEqual(entry["sha256"], hashlib.sha256(data).hexdigest())

    def test_refuses_existing_output_and_output_inside_payload(self):
        output = self.root / "existing.zip"
        output.write_bytes(b"preserved")
        with self.assertRaises(ValueError):
            self.create(output)
        self.assertEqual(output.read_bytes(), b"preserved")
        with self.assertRaises(ValueError):
            self.create(self.bundle / "archive.zip")

    def test_refuses_links_and_unapproved_version(self):
        try:
            (self.bundle / "linked").symlink_to(self.lock)
        except OSError as error:
            self.skipTest(f"Symlink fixture unavailable; native reparse gate remains: {error}")
        with self.assertRaises(ValueError):
            self.create(self.root / "unsafe.zip")
        with self.assertRaises(ValueError):
            archive_payload(self.bundle, self.root / "wrong.zip", version="0.0.6",
                            source="a" * 40, lockfile=self.lock)

    def test_source_revision_is_required(self):
        with self.assertRaises(ValueError):
            archive_payload(self.bundle, self.root / "unknown.zip", version="0.6.0+3",
                            source="HEAD", lockfile=self.lock)

    def test_root_symlink_is_not_resolved_before_validation(self):
        linked = self.root / "linked-bundle"
        try:
            linked.symlink_to(self.bundle, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"Symlink fixture unavailable; native reparse gate remains: {error}")
        with self.assertRaises(ValueError):
            archive_payload(linked, self.root / "linked.zip", version="0.6.0+3",
                            source="a" * 40, lockfile=self.lock)

    def test_output_link_preserves_its_target(self):
        target = self.root / "preserved"
        target.write_bytes(b"preserved")
        linked = self.root / "linked-output.zip"
        try:
            linked.symlink_to(target)
        except OSError as error:
            self.skipTest(f"Symlink fixture unavailable; native reparse gate remains: {error}")
        with self.assertRaises(ValueError):
            self.create(linked)
        self.assertEqual(target.read_bytes(), b"preserved")

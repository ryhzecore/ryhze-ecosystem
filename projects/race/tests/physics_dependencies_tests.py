"""Adversarial dependency verification fixtures, without running third-party code."""
import hashlib
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from output_paths import evidence_directory
from physics_dependencies import relative_name, verify_files
from physics_build_overlay import LOCK, PATCH, REPLACEMENTS, SOURCE, derive, generate
import physics_epa_overlay as epa


class DependencyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence = evidence_directory("p04-dependency-tests")

    def setUp(self):
        self.source = self.evidence / self._testMethodName
        self.source.mkdir()
        (self.source / "file.h").write_bytes(b"pinned")
        self.records = [{"path": "file.h", "bytes": 6,
                         "sha256": hashlib.sha256(b"pinned").hexdigest()}]

    def test_exact_bytes_and_file_set(self):
        self.assertEqual(verify_files(self.source, self.records), (1, 6))

    def test_same_size_modified_bytes_rejected(self):
        (self.source / "file.h").write_bytes(b"edited")
        with self.assertRaisesRegex(ValueError, "content differs"):
            verify_files(self.source, self.records)

    def test_additional_source_rejected(self):
        (self.source / "extra.cpp").write_bytes(b"extra")
        with self.assertRaisesRegex(ValueError, "file set differs"):
            verify_files(self.source, self.records)

    def test_missing_source_rejected(self):
        self.records[0]["path"] = "missing.h"
        with self.assertRaises(FileNotFoundError):
            verify_files(self.source, self.records)

    def test_truncated_source_rejected(self):
        (self.source / "file.h").write_bytes(b"pin")
        with self.assertRaisesRegex(ValueError, "size differs"):
            verify_files(self.source, self.records)

    def test_case_alias_rejected(self):
        self.records.append(dict(self.records[0], path="FILE.H"))
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            verify_files(self.source, self.records)

    def test_paths_cannot_escape_or_redirect(self):
        for name in ("../file", "/file", "C:/file", "file:stream", "dir\\file",
                     "./file", "a/../file", "a//file", "file.", "file ", "a\x00b"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                relative_name(name)


class OverlayTests(unittest.TestCase):
    # A small synthetic upstream exercises the transform, rather than requiring
    # an acquired external SDK for portable rejection tests.
    def setUp(self):
        self.raw = b"// Copyright unchanged\n" + b"\n".join(old for old, _ in REPLACEMENTS) + b"\n"
        derived = self.raw
        for old, new in REPLACEMENTS:
            derived = derived.replace(old, new)
        self.expected = derived
        self.lock = dict(schema=1, patch=PATCH, source=SOURCE, maximum_bodies=256,
                         upstream_commit="e77f175595e64cb44218cc9d9d56fc365ad0e36a",
                         upstream_sha256=hashlib.sha256(self.raw).hexdigest(),
                         derived_sha256=hashlib.sha256(derived).hexdigest())

    def test_exact_two_replacements_preserve_other_bytes(self):
        self.assertEqual(derive(self.raw, self.lock), self.expected)
        self.assertTrue(self.expected.startswith(b"// Copyright unchanged\n"))

    def test_modified_upstream_rejected(self):
        with self.assertRaisesRegex(ValueError, "upstream content differs"):
            derive(self.raw + b" ", self.lock)

    def test_missing_and_duplicate_anchors_rejected(self):
        for value in (self.raw.replace(REPLACEMENTS[0][0], b"changed"),
                      self.raw + REPLACEMENTS[0][0]):
            lock = dict(self.lock, upstream_sha256=hashlib.sha256(value).hexdigest())
            with self.assertRaisesRegex(ValueError, "anchor count differs"):
                derive(value, lock)

    def test_wrong_derived_identity_rejected(self):
        with self.assertRaisesRegex(ValueError, "derived content differs"):
            derive(self.raw, dict(self.lock, derived_sha256="0"*64))

    def test_profile_or_patch_drift_rejected(self):
        for key, value in (("maximum_bodies", 257), ("patch", "other"), ("schema", 2), ("upstream_commit", "0"*40)):
            with self.assertRaisesRegex(ValueError, "Unsupported"):
                derive(self.raw, dict(self.lock, **{key: value}))

    def test_output_cannot_escape_build_cache(self):
        root = Path(__file__).resolve().parents[1]
        for output in (root / "scripts" / "QuadTree.cpp", root / "out" / ".." / "QuadTree.cpp",
                       root / "out" / "Wrong.cpp"):
            with self.assertRaisesRegex(ValueError, "inside the checkout build cache"):
                generate(root, output)

    def test_tracked_lock_declares_same_capacity_and_transform(self):
        lock = json.loads(LOCK.read_text(encoding="utf-8"))
        self.assertEqual((lock["patch"], lock["maximum_bodies"]), (PATCH, 256))


class EPAOverlayTests(unittest.TestCase):
    def setUp(self):
        self.raw = b'// Preserved licence\r\n' + b'\r\n'.join(a for a, _ in epa.REPLACEMENTS)
        self.expected = self.raw
        for old, new in epa.REPLACEMENTS:
            self.expected = self.expected.replace(old, new)
        self.lock = dict(schema=1, patch=epa.PATCH, source=epa.SOURCE, maximum_triangles=256, maximum_edges=256, maximum_points=128,
                         upstream_commit='e77f175595e64cb44218cc9d9d56fc365ad0e36a',
                         upstream_sha256=hashlib.sha256(self.raw).hexdigest(),
                         derived_sha256=hashlib.sha256(self.expected).hexdigest())

    def test_pinned_three_site_transform(self):
        self.assertEqual(epa.derive(self.raw, self.lock), self.expected)
        lock = json.loads(epa.LOCK.read_text())
        self.assertEqual((lock['patch'], lock['maximum_triangles'], lock['maximum_edges'], lock['maximum_points']), (epa.PATCH, 256, 256, 128))
        self.assertEqual(len(epa.REPLACEMENTS), 3)

    def test_modified_upstream_or_derived_rejected(self):
        with self.assertRaisesRegex(ValueError, 'upstream content differs'):
            epa.derive(self.raw + b' ', self.lock)
        with self.assertRaisesRegex(ValueError, 'derived content differs'):
            epa.derive(self.raw, dict(self.lock, derived_sha256='0'*64))

    def test_ambiguous_anchors_rejected(self):
        for anchor,_ in epa.REPLACEMENTS:
            for raw in (self.raw.replace(anchor, b'missing'), self.raw + anchor):
                with self.assertRaisesRegex(ValueError, 'anchor count differs'):
                    epa.derive(raw, dict(self.lock, upstream_sha256=hashlib.sha256(raw).hexdigest()))

    def test_profile_and_source_drift_rejected(self):
        for key, value in [('maximum_triangles', 128), ('maximum_edges', 128), ('maximum_points', 256), ('source', 'Other.h'), ('patch', 'other'), ('upstream_commit', '0'*40)]:
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'Unsupported'):
                epa.derive(self.raw, dict(self.lock, **{key: value}))

    def test_output_location_rejected(self):
        root = Path(__file__).resolve().parents[1]
        for output in (root/'scripts/EPAConvexHullBuilder.h', root/'out/../EPAConvexHullBuilder.h',
                       root/'out/EPAConvexHullBuilder.h', root/'out/Jolt/Geometry/Wrong.h'):
            with self.subTest(output=output), self.assertRaises(ValueError):
                epa.generate(root, output)


if __name__ == "__main__":
    unittest.main()

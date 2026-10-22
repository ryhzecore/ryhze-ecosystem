"""Synthetic recovery fixtures only: no installed app, registry or process calls."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import installer_recovery as recovery


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.fixture = tempfile.TemporaryDirectory()
        self.base = Path(self.fixture.name)
        self.root = self.base / "Ryhze-Installer-Acceptance"
        (self.root / "app" / "data").mkdir(parents=True)
        self.contents = {"ryhze.exe": b"synthetic fixture; never execute", "data/example.txt": b"owned data"}
        for name, data in self.contents.items():
            (self.root / "app" / name).write_bytes(data)
        self.unknown = self.root / "app" / "user-extra.txt"
        self.unknown.write_bytes(b"user intent: retain")
        self.user_data = self.base / "user-data"
        self.user_data.mkdir()
        (self.user_data / "sentinel").write_bytes(b"separate durable state")
        self.manifest = {"format": 1, "product": "Ryhze", "version": "0.6.0+3",
                         "source_revision": "a" * 40, "artifact_kind": "unsigned-application-payload",
                         "dependency_lock_sha256": "b" * 64,
                         "files": [{"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                                   for name, data in self.contents.items()]}
        self.manifest_bytes = json.dumps(self.manifest).encode()
        self.manifest_hash = hashlib.sha256(self.manifest_bytes).hexdigest()
        self.observation = {"product": "RyhzeInstallerAcceptance", "owner_id": "{11111111-2222-3333-4444-555555555555}",
                            "registered_owner": "{11111111-2222-3333-4444-555555555555}", "install_root": str(self.root.absolute()),
                            "installed_version": "0.6.0", "running": False, "registration_verified": True}

    def tearDown(self):
        self.fixture.cleanup()

    def plan(self, **kwargs):
        return recovery.validate_owned(self.root, self.manifest, self.observation,
                                       target_version=kwargs.get("target_version", "0.6.1+1"), mode=kwargs.get("mode", "upgrade"))

    def retained(self):
        output = self.base / "retained"
        digest = recovery.retain(self.root, output, self.plan(), self.manifest_hash, reader_min=1, reader_max=2)
        return output, digest

    def test_exact_manifest_identity_and_duplicate_json_fields(self):
        self.assertEqual(recovery.source_manifest(self.manifest_bytes, self.manifest_hash), self.manifest)
        with self.assertRaisesRegex(ValueError, "identity"):
            recovery.source_manifest(self.manifest_bytes, "0" * 64)
        duplicate = b'{"format":1,"format":1}'
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            recovery.source_manifest(duplicate, hashlib.sha256(duplicate).hexdigest())

    def test_manifest_rejects_escape_device_ads_case_collision_and_aliases(self):
        record = dict(self.manifest["files"][0])
        for name in ("../ryhze.exe", "/ryhze.exe", "C:/ryhze.exe", "data\\ryhze.exe", "NUL .txt", "data/file:ads", "data//file", "file\x85name"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                recovery.records([dict(record, path=name)])
        with self.assertRaisesRegex(ValueError, "colliding"):
            recovery.records([record, dict(record, path="RYHZE.EXE")])
        with self.assertRaisesRegex(ValueError, "identity collision"):
            recovery.records([record, dict(record, path="ryhze.exe/child")])

    def test_wrong_product_root_owner_and_unknown_process_fail_closed(self):
        for field, value in (("product", "RyhzeEntertainment"), ("registered_owner", "other"),
                             ("owner_id", "not-a-guid"), ("install_root", str(self.base)),
                             ("running", True), ("running", None), ("registration_verified", False),
                             ("installed_version", "0.5.0")):
            original = self.observation.copy()
            self.observation[field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                self.plan()
            self.observation = original

    def test_upgrade_and_repair_have_distinct_version_rules(self):
        self.assertFalse(self.plan()["activation_allowed"])
        self.assertEqual(self.plan(mode="repair", target_version="0.6.0+3")["mode"], "repair")
        for mode, target in (("upgrade", "0.6.0+3"), ("upgrade", "0.5.9+99"),
                             ("repair", "0.6.1+1"), ("repair", "0.6.0+4"), ("downgrade", "0.5.0")):
            with self.subTest(mode=mode, target=target), self.assertRaises(ValueError):
                self.plan(mode=mode, target_version=target)
        with self.assertRaises(ValueError):
            recovery.version("0.06.0+3")
        self.observation["binary_version"] = "0.6.0+3"
        self.assertFalse(self.plan()["activation_allowed"])
        self.observation["binary_version"] = "0.6.0+4"
        with self.assertRaisesRegex(ValueError, "executable build"):
            self.plan()

    def test_modified_owned_file_is_not_a_known_good_repair_backup(self):
        (self.root / "app" / "ryhze.exe").write_bytes(b"modified")
        with self.assertRaisesRegex(ValueError, "manifest"):
            self.plan(mode="repair", target_version="0.6.0+3")
        self.assertEqual(self.unknown.read_bytes(), b"user intent: retain")

    def test_retention_and_compatible_restore_preserve_existing_and_unknown_state(self):
        retained, digest = self.retained()
        self.assertFalse((retained / "app" / "user-extra.txt").exists())
        restored = self.base / "restored-review"
        result = recovery.restore(retained, restored, expected_sha256=digest, store_schema=2)
        self.assertFalse(result["activation_allowed"])
        self.assertFalse(result["existing_installation_changed"])
        for name, data in self.contents.items():
            self.assertEqual((restored / "app" / name).read_bytes(), data)
            self.assertEqual((self.root / "app" / name).read_bytes(), data)
        self.assertEqual(self.unknown.read_bytes(), b"user intent: retain")
        self.assertEqual((self.user_data / "sentinel").read_bytes(), b"separate durable state")

    def test_future_schema_or_changed_recovery_metadata_refused_before_output(self):
        retained, digest = self.retained()
        output = self.base / "must-not-exist"
        for expected, schema in ((digest, 3), ("0" * 64, 2), (digest, True)):
            with self.subTest(schema=schema), self.assertRaises(ValueError):
                recovery.restore(retained, output, expected_sha256=expected, store_schema=schema)
            self.assertFalse(output.exists())

    def test_corrupt_backup_and_missing_completion_record_refused(self):
        retained, digest = self.retained()
        (retained / "app" / "ryhze.exe").write_bytes(b"corrupt")
        output = self.base / "must-not-exist"
        with self.assertRaises(ValueError):
            recovery.restore(retained, output, expected_sha256=digest, store_schema=2)
        self.assertFalse(output.exists())
        (retained / "recovery.json").unlink()
        with self.assertRaises(FileNotFoundError):
            recovery.restore(retained, output, expected_sha256=digest, store_schema=2)
        self.assertFalse(output.exists())

    def test_existing_outputs_and_installation_nested_outputs_refused(self):
        plan = self.plan()
        for output in (self.root, self.root / "backup", self.user_data):
            with self.subTest(output=output), self.assertRaises(ValueError):
                recovery.retain(self.root, output, plan, self.manifest_hash, reader_min=2, reader_max=2)
        retained, digest = self.retained()
        with self.assertRaises(ValueError):
            recovery.restore(retained, retained / "nested", expected_sha256=digest, store_schema=2)

    def test_changed_source_after_preflight_keeps_partial_evidence_without_completion(self):
        plan = self.plan()
        (self.root / "app" / "data" / "example.txt").write_bytes(b"changed after preflight")
        output = self.base / "partial"
        with self.assertRaises(ValueError):
            recovery.retain(self.root, output, plan, self.manifest_hash, reader_min=2, reader_max=2)
        self.assertTrue((output / "app" / "ryhze.exe").exists())
        self.assertFalse((output / "recovery.json").exists())
        self.assertEqual(self.unknown.read_bytes(), b"user intent: retain")

    def test_interrupted_output_is_retained_and_not_treated_as_complete(self):
        output = self.base / "interrupted"
        with mock.patch.object(recovery.os, "fsync", side_effect=OSError("synthetic flush failure")):
            with self.assertRaisesRegex(OSError, "synthetic"):
                recovery.retain(self.root, output, self.plan(), self.manifest_hash, reader_min=2, reader_max=2)
        self.assertTrue(output.exists())
        self.assertFalse((output / "recovery.json").exists())

    def test_metadata_does_not_grant_execution_or_other_product_authority(self):
        forged = copy.deepcopy(self.manifest)
        forged["product"] = "OtherProduct"
        data = json.dumps(forged).encode()
        with self.assertRaises(ValueError):
            recovery.source_manifest(data, hashlib.sha256(data).hexdigest())
        forged = copy.deepcopy(self.manifest)
        forged["execute"] = "anything"
        data = json.dumps(forged).encode()
        with self.assertRaises(ValueError):
            recovery.source_manifest(data, hashlib.sha256(data).hexdigest())


if __name__ == "__main__":
    unittest.main()

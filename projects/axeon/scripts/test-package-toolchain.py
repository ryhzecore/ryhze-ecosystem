#!/usr/bin/env python3
"""Pure local packaging-contract tests; never execute an Axeon compiler."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    "axeon_packager", Path(__file__).with_name("package-toolchain.py"))
PACKAGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGER)
COMMIT = "1234567890abcdef1234567890abcdef12345678"
COMPILER_SHA = "a" * 64
RUNTIME_SHA = "b" * 64
GENERIC_NOMINAL_OUTPUT = "42\n-9223372036854775808\nresult: 0\n"


def reproduction(compiler=COMPILER_SHA, runtime=RUNTIME_SHA):
    hashes = {PACKAGER.COMPILER_KEY: compiler, PACKAGER.RUNTIME_KEY: runtime}
    hashes.update({name: "c" * 64 for name in
                   ("generic-nominals.o", "generic-nominals.o.json",
                    "generic-nominals", "generic-nominals.json")})
    return {
        "schema_version": 3, "exit_code": 0, "source_commit": COMMIT,
        "compiler_objects_executables_and_metadata_byte_identical": True,
        "native_execution_verified": True, "includes_tensor_runtime_archive": True,
        "includes_local_modules": True, "independent_builds": 2,
        "includes_local_records": True, "includes_local_unions": True,
        "includes_aggregate_interfaces": True,
        "includes_nested_aggregates": True,
        "includes_unit_values": True, "includes_static_traits": True,
        "includes_generic_functions": True,
        "includes_generic_nominals": True,
        "shared_target_directory": False,
        "examples": {"generic-nominals": {
            "source": "examples/generic-nominals/main.ax",
            "expected_stdout": GENERIC_NOMINAL_OUTPUT,
            "expected_stderr": "", "expected_exit_code": 0}},
        "builds": [{"build": index, "sha256": hashes.copy(),
                    "native_outputs": {"module": {"exit_code": 0, "stdout": "fixture"},
                                       "generic-nominals": {"exit_code": 0,
                                                            "stdout": GENERIC_NOMINAL_OUTPUT,
                                                            "stderr": ""}}}
                   for index in range(2)],
    }


class ReproductionTests(unittest.TestCase):
    def test_exact_two_build_agreement_returns_only_receipt_commit(self):
        self.assertEqual(PACKAGER.validate_reproduction(reproduction(), COMPILER_SHA, RUNTIME_SHA), COMMIT)

    def test_missing_failed_or_ambiguous_evidence_is_rejected(self):
        mutations = [
            ("schema_version", 2), ("schema_version", 3.0), ("exit_code", 1), ("exit_code", False),
            ("source_commit", "unknown"), ("source_commit", "a" * 39),
            ("source_commit", "z" * 40), ("independent_builds", 1),
            ("shared_target_directory", True), ("native_execution_verified", False),
            ("compiler_objects_executables_and_metadata_byte_identical", False),
            ("includes_tensor_runtime_archive", False), ("includes_local_modules", False),
            ("includes_local_records", False), ("includes_local_unions", False),
            ("includes_aggregate_interfaces", False),
            ("includes_nested_aggregates", False),
            ("includes_unit_values", False), ("includes_static_traits", False),
            ("includes_generic_functions", False),
            ("includes_generic_nominals", False),
        ]
        for key, value in mutations:
            receipt = reproduction()
            receipt[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(PACKAGER.PackageError):
                PACKAGER.validate_reproduction(receipt, COMPILER_SHA, RUNTIME_SHA)
        with self.assertRaises(PACKAGER.PackageError):
            PACKAGER.validate_reproduction([], COMPILER_SHA, RUNTIME_SHA)

    def test_both_artifacts_must_match_both_builds(self):
        for index in (0, 1):
            for artifact in (PACKAGER.COMPILER_KEY, PACKAGER.RUNTIME_KEY):
                receipt = reproduction()
                receipt["builds"][index]["sha256"][artifact] = "c" * 64
                with self.subTest(build=index, artifact=artifact), self.assertRaises(PACKAGER.PackageError):
                    PACKAGER.validate_reproduction(receipt, COMPILER_SHA, RUNTIME_SHA)
        receipt = reproduction()
        receipt["builds"][1]["sha256"]["other-artifact"] = "d" * 64
        with self.assertRaisesRegex(PACKAGER.PackageError, "maps differ"):
            PACKAGER.validate_reproduction(receipt, COMPILER_SHA, RUNTIME_SHA)

    def test_distinct_ids_valid_hashes_and_native_records_are_required(self):
        for change in (lambda r: r["builds"].pop(),
                       lambda r: r["builds"][1].update(build=0),
                       lambda r: r["builds"][1].update(build=[]),
                       lambda r: r["builds"][0].update(build=False),
                       lambda r: r["builds"][1].update(native_outputs={}),
                       lambda r: r["builds"][0]["sha256"].update(other="not-a-hash")):
            receipt = reproduction()
            change(receipt)
            with self.assertRaises(PACKAGER.PackageError):
                PACKAGER.validate_reproduction(receipt, COMPILER_SHA, RUNTIME_SHA)

    def test_generic_nominal_flag_requires_actual_reproduction_evidence(self):
        for value in (None, False, 1, "true"):
            receipt = reproduction()
            if value is None:
                del receipt["includes_generic_nominals"]
            else:
                receipt["includes_generic_nominals"] = value
            with self.subTest(flag=value), self.assertRaisesRegex(PACKAGER.PackageError, "includes_generic_nominals"):
                PACKAGER.validate_reproduction(receipt, COMPILER_SHA, RUNTIME_SHA)
        for change in (lambda receipt: receipt.pop("examples"),
                       lambda receipt: receipt["examples"].clear(),
                       lambda receipt: receipt["examples"]["generic-nominals"].update(source="other.ax"),
                       lambda receipt: receipt["examples"]["generic-nominals"].update(expected_stdout="42\n"),
                       lambda receipt: receipt["examples"]["generic-nominals"].update(expected_exit_code=False)):
            receipt = reproduction()
            change(receipt)
            with self.assertRaisesRegex(PACKAGER.PackageError, "reproduction example"):
                PACKAGER.validate_reproduction(receipt, COMPILER_SHA, RUNTIME_SHA)

    def test_both_builds_must_include_all_generic_nominal_artifacts_and_exact_results(self):
        # Alter both builds identically so map equality alone cannot accept
        # a receipt that falsely claims this feature was reproduced.
        for artifact in ("generic-nominals.o", "generic-nominals.o.json",
                         "generic-nominals", "generic-nominals.json"):
            receipt = reproduction()
            for build in receipt["builds"]:
                del build["sha256"][artifact]
            with self.subTest(artifact=artifact), self.assertRaisesRegex(PACKAGER.PackageError, "artifact is missing"):
                PACKAGER.validate_reproduction(receipt, COMPILER_SHA, RUNTIME_SHA)
        for output in (None, {}, {"exit_code": False, "stdout": GENERIC_NOMINAL_OUTPUT, "stderr": ""},
                       {"exit_code": 0, "stdout": "42\n0\nresult: 0\n", "stderr": ""},
                       {"exit_code": 0, "stdout": GENERIC_NOMINAL_OUTPUT, "stderr": "warning"}):
            receipt = reproduction()
            for build in receipt["builds"]:
                if output is None:
                    del build["native_outputs"]["generic-nominals"]
                else:
                    build["native_outputs"]["generic-nominals"] = output
            with self.subTest(output=output), self.assertRaisesRegex(PACKAGER.PackageError, "exact generic nominal native result"):
                PACKAGER.validate_reproduction(receipt, COMPILER_SHA, RUNTIME_SHA)


@unittest.skipUnless(sys.platform == "linux", "Linux regular-file admission required")
class FileAndStagingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="axeon-package-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data if isinstance(data, bytes) else data.encode("utf-8"))
        return path

    def test_bounded_copy_preserves_bytes_and_explicit_permissions(self):
        source = self.write("source", b"known bytes")
        target = self.root / "out/tool"
        expected = hashlib.sha256(b"known bytes").hexdigest()
        self.assertEqual(PACKAGER.copy_regular(source, target, 20, expected, 0o755), expected)
        self.assertEqual(target.read_bytes(), source.read_bytes())
        self.assertEqual(target.stat().st_mode & 0o777, 0o755)
        with self.assertRaises(FileExistsError):
            PACKAGER.copy_regular(source, target, 20)
        self.assertEqual(target.read_bytes(), b"known bytes")

    def test_symlinks_ancestor_links_nonregular_files_and_size_limits_are_rejected(self):
        source = self.write("directory/file", b"12345")
        (self.root / "link").symlink_to(source)
        (self.root / "parent-link").symlink_to(self.root / "directory", target_is_directory=True)
        fifo = self.root / "fifo"
        os.mkfifo(fifo)
        for path, limit in ((self.root / "link", 10), (self.root / "parent-link/file", 10),
                            (self.root / "directory", 10), (fifo, 10), (source, 4)):
            with self.subTest(path=path), self.assertRaises(PACKAGER.PackageError):
                PACKAGER.read_regular(path, limit)

    def test_changed_copy_is_preserved_but_not_accepted(self):
        source = self.write("source", b"actual")
        destination = self.root / "destination"
        with self.assertRaisesRegex(PACKAGER.PackageError, "reproduction"):
            PACKAGER.copy_regular(source, destination, 20, "0" * 64)
        self.assertEqual(destination.read_bytes(), b"actual")
        self.assertEqual(source.read_bytes(), b"actual")

    def source_fixture(self):
        for path in ("docs/language.md", "examples/records.ax", "runtime/runtime.c",
                     "runtime/runtime.h", "toolchain-lock.json", "README.md"):
            self.write("source/" + path, "fixture\n")
        self.write("source/Cargo.toml", '[package]\nversion = "0.6.0"\n')
        self.write("source/crates/axeon-tensor-runtime/Cargo.toml", '[package]\nversion = "0.1.0"\n')
        return self.root / "source"

    def test_ancillary_listing_rejects_symlinks_and_manifest_unsafe_names(self):
        root = self.source_fixture()
        paths = PACKAGER.ancillary_files(root)
        self.assertEqual(len(paths), 6)
        self.assertEqual(paths, sorted(paths))
        bad = root / "docs" / "bad\nname"
        bad.write_bytes(b"bad")
        with self.assertRaisesRegex(PACKAGER.PackageError, "newlines"):
            PACKAGER.ancillary_files(root)
        bad.unlink()
        (root / "docs" / "linked").symlink_to(root / "README.md")
        with self.assertRaisesRegex(PACKAGER.PackageError, "symlink"):
            PACKAGER.ancillary_files(root)

    def test_ancillary_entry_limit_stops_enumeration_before_materializing_directory(self):
        root = self.source_fixture()
        entry = mock.Mock(path=str(root / "docs/language.md"))
        entry.is_symlink.return_value = False
        entry.is_dir.return_value = False
        observed = []

        def entries():
            for index in range(3):
                observed.append(index)
                yield entry
            self.fail("directory iterator was consumed past the entry limit")

        with (mock.patch.object(PACKAGER, "MAX_ANCILLARY_FILES", 2),
              mock.patch.object(PACKAGER.os, "scandir", return_value=contextlib.nullcontext(entries())),
              self.assertRaisesRegex(PACKAGER.PackageError, "entry limit")):
            PACKAGER.ancillary_files(root)
        self.assertEqual(observed, [0, 1, 2])

    def test_json_write_failure_preserves_complete_prior_receipt(self):
        path = self.write("package-receipt.json", '{"status": "staging"}\n')
        previous = path.read_bytes()

        def fail_mid_write(value, output, **options):
            output.write('{"status": "pas')
            raise OSError("injected write failure")

        with (mock.patch.object(PACKAGER.json, "dump", side_effect=fail_mid_write),
              self.assertRaisesRegex(OSError, "injected write failure")):
            PACKAGER.write_json(path, {"status": "passed"})
        self.assertEqual(path.read_bytes(), previous)
        self.assertEqual(list(self.root.iterdir()), [path])

    def test_json_publish_failure_and_exclusive_create_preserve_prior_receipt(self):
        path = self.write("package-receipt.json", '{"status": "staging"}\n')
        previous = path.read_bytes()
        with (mock.patch.object(PACKAGER.os, "replace", side_effect=OSError("injected publish failure")),
              self.assertRaisesRegex(OSError, "injected publish failure")):
            PACKAGER.write_json(path, {"status": "passed"})
        self.assertEqual(path.read_bytes(), previous)
        with self.assertRaises(FileExistsError):
            PACKAGER.write_json(path, {"status": "passed"}, exclusive=True)
        self.assertEqual(path.read_bytes(), previous)
        PACKAGER.write_json(path, {"status": "failed", "failure": "recorded"})
        self.assertEqual(json.loads(path.read_text()), {"status": "failed", "failure": "recorded"})
        self.assertEqual(list(self.root.iterdir()), [path])

    def test_manifest_write_failure_never_publishes_valid_but_incomplete_prefix(self):
        class FailingManifest:
            def items(self):
                yield "first", "a" * 64
                raise OSError("injected manifest write failure")

        path = self.root / "SHA256SUMS"
        with self.assertRaisesRegex(OSError, "injected manifest write failure"):
            PACKAGER.write_manifest(path, FailingManifest())
        self.assertFalse(path.exists())
        self.assertEqual(list(self.root.iterdir()), [])
        with (mock.patch.object(PACKAGER.os, "fsync", side_effect=OSError("injected flush failure")),
              self.assertRaisesRegex(OSError, "injected flush failure")):
            PACKAGER.write_manifest(path, {"first": "a" * 64})
        self.assertFalse(path.exists())
        self.assertEqual(list(self.root.iterdir()), [])

    def test_complete_manifest_is_published_exclusively(self):
        path = self.root / "SHA256SUMS"
        PACKAGER.write_manifest(path, {"first": "a" * 64, "second": "b" * 64})
        expected = "a" * 64 + "  first\n" + "b" * 64 + "  second\n"
        self.assertEqual(path.read_text(), expected)
        with self.assertRaises(FileExistsError):
            PACKAGER.write_manifest(path, {"replacement": "c" * 64})
        self.assertEqual(path.read_text(), expected)
        self.assertEqual(list(self.root.iterdir()), [path])

    def test_failed_smoke_retains_partial_receipt_but_no_complete_manifest(self):
        root = self.source_fixture()
        header = bytearray(64)
        header[:6] = b"\x7fELF\x02\x01"
        header[18:20] = b"\x3e\x00"
        compiler = self.write("compiler", bytes(header))
        runtime = self.write("runtime.a", b"!<arch>\nfixture")
        receipt_path = self.write("reproduction.json", json.dumps(reproduction(
            hashlib.sha256(header).hexdigest(), hashlib.sha256(runtime.read_bytes()).hexdigest())))
        package = self.root / "package"
        package.mkdir()
        # A forced failure validates publication ordering, never a fake success.
        with (mock.patch.object(PACKAGER, "run_smokes",
                                side_effect=PACKAGER.PackageError("forced smoke failure")) as smoke,
              contextlib.redirect_stderr(io.StringIO())):
            result = PACKAGER.package_toolchain(root, compiler, runtime, receipt_path, package)
        smoke.assert_called_once()
        self.assertEqual(result, 1)
        receipt = json.loads((package / "package-receipt.json").read_text())
        self.assertEqual(receipt["status"], "failed")
        self.assertEqual(receipt["source_commit"], COMMIT)
        self.assertTrue((package / "bin/axeon").is_file())
        self.assertEqual((package / "bin/axeon").read_bytes(), bytes(header))
        self.assertFalse((package / "SHA256SUMS").exists())
        self.assertFalse((package / "README.md").exists())

    def test_existing_package_directory_is_never_reused(self):
        compiler = self.write("compiler", b"input")
        runtime = self.write("runtime", b"input")
        evidence = self.write("receipt", b"{}")
        self.write("existing/keep", b"prior package")
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            PACKAGER.main(["--compiler", str(compiler), "--tensor-runtime", str(runtime),
                           "--reproduction", str(evidence), "--output-dir", str(self.root / "existing")])
        self.assertEqual(error.exception.code, 2)
        self.assertEqual(list((self.root / "existing").iterdir()), [self.root / "existing/keep"])
        self.assertEqual((self.root / "existing/keep").read_bytes(), b"prior package")

    def test_portable_readme_records_actual_version_commit_and_limitations(self):
        receipt = {"compiler_version": "0.6.0", "source_commit": COMMIT,
                   "version_output": "axeon 0.6.0; subset 6; LLVM/Clang 19.1.7; hosted ABI fixture\n"}
        readme = PACKAGER.package_readme(receipt)
        for expected in (COMMIT, "0.1.0", "Linux x86-64 ELF", "Debian/WSL",
                         "docs/language.md", "SHA256SUMS", "architecture remains unfinished",
                         "docs/generic-nominals.md", "axeon test examples/generic-nominals/tests.ax",
                         "--include-generic-nominals", "generic\ntraits/implementations"):
            self.assertIn(expected, readme)
        self.assertNotIn("Recursive value layouts, generics,", readme)
        self.assertNotIn("C:/Projects", readme)
        self.assertNotIn("/home/Leo", readme)


if __name__ == "__main__":
    unittest.main()

"""Host-side packaging tests. These do not claim native NSIS validation."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("prepare_bundle", Path(__file__).with_name("prepare_bundle.py"))
prepare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prepare)


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.bundle = Path(self.temp.name)
        (self.bundle / "ryhze.exe").write_bytes(b"fixture only; never executed")

    def tearDown(self):
        self.temp.cleanup()

    def test_sorted_file_manifest_uses_exact_paths_and_keeps_unknown_files(self):
        directory = self.bundle / "data" / "flutter assets"
        directory.mkdir(parents=True)
        (directory / "a file.txt").write_text("fixture")
        files = prepare.bundle_files(self.bundle)
        manifest = prepare.render_manifest(files)
        self.assertIn('File "/oname=a file.txt" "${BUNDLE_DIR}\\data\\flutter assets\\a file.txt"', manifest)
        self.assertIn('Delete "$INSTDIR\\app\\data\\flutter assets\\a file.txt"', manifest)
        self.assertNotIn("RMDir /r", manifest)
        self.assertNotIn('Delete "$INSTDIR\\*', manifest)
        self.assertIn('!insertmacro AssertNoReparsePoint "$INSTDIR\\app\\data\\flutter assets"', manifest)
        self.assertIn('!insertmacro AssertNoReparsePoint "$INSTDIR\\app\\data\\flutter assets\\a file.txt"', manifest)
        self.assertLess(manifest.index('RMDir "$INSTDIR\\app\\data\\flutter assets"'), manifest.index('RMDir "$INSTDIR\\app\\data"'))

    def test_missing_executable_is_rejected(self):
        (self.bundle / "ryhze.exe").unlink()
        with self.assertRaisesRegex(ValueError, "ryhze.exe"):
            prepare.bundle_files(self.bundle)

    def test_symbolic_link_is_rejected(self):
        (self.bundle / "linked.txt").symlink_to(self.bundle / "ryhze.exe")
        with self.assertRaisesRegex(ValueError, "Links and junctions"):
            prepare.bundle_files(self.bundle)

    def test_windows_reserved_and_script_sensitive_names_are_rejected(self):
        for value in ['CON.txt', 'nul', 'COM0', 'LPT¹.txt', 'NUL .txt', 'CONIN$',
                      'a\x7fb', 'a\x85b', 'a$b', 'a"b', 'a*b', 'a:b',
                      'trailing.', 'trailing ', '../bad']:
            with self.subTest(value=value):
                self.assertFalse(prepare._safe_component(value))
        self.assertTrue(prepare._safe_component("résumé name.txt"))

    def test_case_insensitive_duplicates_are_rejected(self):
        (self.bundle / "RYHZE.EXE").write_text("collision")
        if len(list(self.bundle.iterdir())) != 2:
            self.skipTest("The filesystem already prevents case collisions")
        with self.assertRaisesRegex(ValueError, "collision"):
            prepare.bundle_files(self.bundle)

    def test_artwork_has_expected_bmp_header_and_length(self):
        bitmap = prepare.wizard_bitmap()
        self.assertEqual(bitmap[:2], b"BM")
        self.assertEqual(struct.unpack_from("<I", bitmap, 2)[0], len(bitmap))
        self.assertEqual(struct.unpack_from("<ii", bitmap, 18), (164, 314))
        # Width 164 has no BMP row padding: verify every pixel uses the shared palette.
        actual_bgr = {tuple(bitmap[i:i + 3]) for i in range(54, len(bitmap), 3)}
        self.assertEqual(actual_bgr, {(250, 91, 22), (255, 182, 132), (20, 18, 17)})
        nsis = Path(__file__).with_name("ryhze-setup.nsi").read_text()
        for expected in ['InstallColors 165BFA 111214', 'MUI_BGCOLOR "111214"',
                         'MUI_TEXTCOLOR "F7F7F8"', 'MUI_DIRECTORYPAGE_BGCOLOR "18191C"',
                         'MUI_INSTFILESPAGE_COLORS "84B6FF 111214"']:
            self.assertIn(expected, nsis)


if __name__ == "__main__":
    unittest.main()

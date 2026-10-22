"""Installer source contract tests; actual Win32 execution is a separate gate.

These checks prevent dangerous packaging changes that portable tests can detect.
They do not simulate registry/COM semantics or establish native acceptance.
"""
from pathlib import Path
import re
import unittest

HERE = Path(__file__).parent


class InstallerLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.setup = (HERE / "ryhze-setup.nsi").read_text(encoding="utf-8")
        cls.identity = (HERE / "installer_identity.nsh").read_text(encoding="utf-8")
        cls.registration = (HERE / "installer_registration.nsh").read_text(encoding="utf-8")
        cls.shortcut = (HERE / "installer_shortcut.nsh").read_text(encoding="utf-8")
        cls.harness = (HERE / "installer_acceptance.ps1").read_text(encoding="utf-8")
        cls.diagnostics = (HERE / "installer_diagnostics.nsh").read_text(encoding="utf-8")
        cls.paths = (HERE / "installer_paths.nsh").read_text(encoding="utf-8")
        cls.path_fixture = (HERE / "test_installer_paths.nsi").read_text(encoding="utf-8")

    def test_acceptance_profile_has_disjoint_authority(self):
        acceptance, production = self.identity.split("!else", 1)
        keys = ("PRODUCT_ID", "INSTALL_BASENAME", "PRODUCT_KEY", "SHORTCUT_NAME",
                "URI_SCHEME", "APP_USER_MODEL_ID", "TOAST_ACTIVATOR_CLSID")
        for key in keys:
            with self.subTest(key=key):
                a = re.search(rf'!define {key} "([^"]+)"', acceptance).group(1)
                b = re.search(rf'!define {key} "([^"]+)"', production).group(1)
                self.assertNotEqual(a.casefold(), b.casefold())
        self.assertIn('!if "${INSTALLER_ACCEPTANCE}" != "1"', acceptance)
        self.assertIn('!ifndef INSTALLER_ACCEPTANCE\n', self.setup)
        self.assertNotIn("MUI_FINISHPAGE_RUN", acceptance)
        self.assertNotIn("LocalServer32", self.registration)

    def test_fresh_install_still_refuses_existing_authority(self):
        self.assertIn('RequireAbsentRegistryKey 0x80000000 "${URI_SCHEME}"', self.registration)
        for key in ("PRODUCT_KEY", "UNINSTALL_KEY"):
            self.assertIn(f'RequireAbsentRegistryKey 0x80000001 "${{{key}}}"', self.registration)
        self.assertIn('GetFileAttributesW(w "$SMPROGRAMS\\${SHORTCUT_NAME}")', self.registration)
        self.assertIn('RegCreateKeyExW', self.registration)
        self.assertIn('${OrIf} $2 != 1', self.registration)  # REG_CREATED_NEW_KEY
        self.assertIn('The selected folder is not empty', self.setup)
        self.assertIn('SetOverwrite off', self.setup)
        self.assertIn('RequestExecutionLevel user', self.setup)
        self.assertIn('CoCreateGuid', self.setup)
        self.assertNotRegex(self.registration, r'WriteReg\w+\s+HK(?:LM|CR)\b')

    def test_unregistration_checks_owner_root_and_exact_command(self):
        removal = self.registration.split('Function un.UnregisterApplication', 1)[1]
        protocol = removal.split('ReadRegStr $1 HKCU "${UNINSTALL_KEY}"', 1)[0]
        for condition in ('${If} $1 == $InstallId', '${AndIf} $2 == $INSTDIR',
                          '${AndIf} $3 == "${PROTOCOL_COMMAND}"'):
            self.assertIn(condition, protocol)
        self.assertIn('DeleteMatchingString', protocol)
        self.assertIn('DeleteEmptyRegistryKey', protocol)
        self.assertNotRegex(self.registration, r'(?m)^\s*DeleteRegKey\s+(?!/ifempty)',
                            "Broad registry removal must never be introduced")
        emptiness = self.registration.split('!macro DeleteEmptyRegistryKey', 1)[1].split('!macroend', 1)[0]
        self.assertLess(emptiness.index('EnumRegValue'), emptiness.index('DeleteRegKey'))
        self.assertLess(emptiness.index('EnumRegKey'), emptiness.index('DeleteRegKey'))
        after_key_enumeration = emptiness.split('EnumRegKey $0 HKCU "${KEY}" 0', 1)[1]
        self.assertIn('${IfNot} ${Errors}\n    ${AndIf} $0 == ""', after_key_enumeration)
        self.assertNotIn('${If} ${Errors}', after_key_enumeration)
        self.assertNotIn('UserChoice', self.registration)

    def test_protocol_command_is_single_argument_without_shell(self):
        command = next(line for line in self.identity.splitlines() if line.startswith('!define PROTOCOL_COMMAND'))
        self.assertIn('app\\ryhze.exe$\\" $\\"%1$\\"', command)
        for forbidden in ('cmd.exe', 'powershell', 'rundll32', '%*', '--execute'):
            self.assertNotIn(forbidden, command.lower())

    def test_shortcut_delete_requires_all_owned_fields(self):
        for contract in ('IShellLink::GetPath', 'IShellLink::GetArguments',
                         'IShellLink::GetDescription', 'PKEY_AppUserModel_ID',
                         'PKEY_AppUserModel_ToastActivatorCLSID', 'VT_LPWSTR', 'VT_CLSID'):
            self.assertIn(contract, self.shortcut)
        self.assertIn('${If} $ShortcutOwned == 1', self.registration)
        self.assertIn('${PRODUCT_ID}|$InstallId', self.shortcut)
        self.assertIn('GetFileAttributesW', self.shortcut)
        self.assertIn('0x410', self.shortcut)  # reparse point or directory
        self.assertNotIn('IShellLink::Resolve', self.shortcut)
        self.assertEqual(self.registration.count('StrCpy $ShortcutFile "$SMPROGRAMS\\${SHORTCUT_NAME}"'), 2)

    def test_propvariant_allocation_cannot_be_replaced_by_empty_argument_list(self):
        self.assertNotIn('*${SYSSTRUCT_PROPVARIANT}()', self.shortcut)
        self.assertIn("'*${SYSSTRUCT_PROPVARIANT} p .r4'", self.shortcut)
        self.assertEqual(self.shortcut.count('!insertmacro AllocateShortcutPropertyBuffers '), 2)
        for contract in ('GlobalSize(p r3)', 'GlobalSize(p r4)', '$5 < ${SYSSIZEOF_PROPERTYKEY}', '$5 < ${SYSSIZEOF_PROPVARIANT}'):
            self.assertIn(contract, self.shortcut)
        fixture = (HERE / 'test_installer_shortcut.nsi').read_text(encoding='utf-8')
        self.assertIn('!include "installer_shortcut.nsh"', fixture)
        self.assertIn('${LoopUntil} $FixtureCycle == 100', fixture)
        self.assertIn('old-propvariant-size', fixture)
        self.assertIn('fixed-propvariant-size', fixture)
        self.assertIn('${If} $1 <> 0x10', fixture)
        self.assertNotIn('${If} $1 != 0x10', fixture)
        self.assertNotRegex(fixture, r'(?im)^\s*(WriteReg\w+|Delete|RMDir|Exec\w*)\b')

    def test_removal_checks_ancestors_before_reading_authority(self):
        removal = self.setup.split('Function un.ValidateInstallRoot', 1)[1].split('FunctionEnd', 1)[0]
        self.assertLess(removal.index('AssertNoReparseAncestors'), removal.index('ReadINIStr'))
        self.assertIn('AssertNoReparsePoint "$INSTDIR\\${OWNER_MARKER}"', removal)
        self.assertIn('ValidatePayloadDirectories', removal)
        self.assertNotRegex(self.setup, r'(?i)RMDir\s+/r\b')

    def test_harness_is_opt_in_and_preserves_failed_fixtures(self):
        self.assertIn('[switch]$Execute', self.harness)
        self.assertIn('if (-not $Execute)', self.harness)
        self.assertIn("ProductName -cne 'Ryhze Installer Acceptance'", self.harness)
        self.assertIn('InstallerSha256', self.harness)
        for scenario in ('fresh-install', 'repeat-refused', 'locked-removal-refused', 'uninstall-retry'):
            self.assertIn(scenario, self.harness)
        self.assertIn('production-before.json', self.harness)
        self.assertIn('production-after.json', self.harness)
        self.assertIn('unknown-user-file.txt', self.harness)
        self.assertIn('SyntheticSuccessor', self.harness)
        self.assertNotRegex(self.harness, r'(?im)^\s*(Remove-Item|Stop-Process|Invoke-Expression)\b')

    def test_silent_diagnostics_use_only_redirected_bounded_stage_codes(self):
        self.assertIn('!ifdef INSTALLER_ACCEPTANCE', self.diagnostics)
        self.assertIn('GetStdHandle(i -11)', self.diagnostics)
        self.assertIn('stage=${STAGE};code=${CODE}', self.diagnostics)
        for private in ('$INSTDIR', '$InstallId', '$SMPROGRAMS', '$CMDLINE', 'FileOpen'):
            self.assertNotIn(private, self.diagnostics)
        self.assertIn('-RedirectStandardOutput $trace', self.harness)
        self.assertIn('-RedirectStandardError $stderr', self.harness)
        self.assertIn('.Length -gt 65536', self.harness)
        for stage in ('setup-init', 'setup-mutex-status', 'acceptance-target-mismatch',
                      'target-drive-type', 'preflight-complete'):
            self.assertIn('"' + stage + '"', self.setup)
        for stage in ('registry-product-status', 'registry-uninstall-status',
                      'registry-protocol-status', 'shortcut-attribute-status'):
            self.assertIn('"' + stage + '"', self.registration)

    def test_fresh_path_normalization_refuses_empty_or_nonlocal_results(self):
        target = self.setup.split('Function ValidateInstallTarget', 1)[1].split('FunctionEnd', 1)[0]
        self.assertNotIn('GetFullPathName ', target)
        self.assertEqual(target.count('Call NormalizeInstallTargetPath'), 2)
        self.assertEqual(target.count('${If} $TargetPathError != 0'), 2)
        self.assertIn('${OrIf} $INSTDIR != $TargetPathNormalized', target)
        self.assertIn('GetFullPathNameW', self.paths)
        self.assertIn('PathGetDriveNumberW', self.paths)
        self.assertIn('IntCmp $1 ${NSIS_MAX_STRLEN}', self.paths)
        self.assertLess(self.paths.index('StrCmp $0'), self.paths.index('System::Call'))
        self.assertIn('StrCmp $1 $2 0 normalize_path_done', self.paths)
        self.assertIn('!include "installer_paths.nsh"', self.path_fixture)
        removal = self.setup.split('Function un.ValidateInstallRoot', 1)[1].split('FunctionEnd', 1)[0]
        self.assertNotIn('GetFullPathName ', removal)
        self.assertLess(removal.index('Call un.NormalizeInstallTargetPath'), removal.index('ReadINIStr'))
        self.assertIn('${If} $TargetPathError != 0', removal)
        self.assertIn('!insertmacro DefineNormalizeInstallTargetPath "un."', self.setup)
        for case in ('acceptance-path', 'missing-nested-path', 'local-lowercase',
                     'empty', 'relative', 'drive-relative', 'root-relative',
                     'unc', 'device', 'extended', 'invalid-drive', 'forward-slash'):
            self.assertIn('"' + case + '"', self.path_fixture)
        self.assertNotRegex(self.path_fixture, r'(?im)^\s*(CreateDirectory|WriteReg\w+|Delete|RMDir|Exec\w*)\b')

    def test_optional_recovery_gate_validates_before_install_and_remains_inert(self):
        for parameter in ('RecoveryPython', 'RecoveryManifest', 'RecoveryManifestSha256'):
            self.assertIn('[string]$' + parameter, self.harness)
        self.assertIn('$recoveryInputs.Count -ne 0 -and $recoveryInputs.Count -ne 3', self.harness)
        self.assertLess(self.harness.index('validate-manifest --manifest'), self.harness.index('[IO.Directory]::CreateDirectory($evidence)'))
        self.assertLess(self.harness.index("Run-Recovery 'recovery-restore'"), self.harness.index("$sentinel = Join-Path"))
        for contract in ("'--mode', 'repair'", "'--target-version', '0.6.0+3'", "'--store-schema', '2'", "'--reader-schema-min', '2'", "'--reader-schema-max', '2'", 'activation_allowed -ne $false', 'copies=3; matched=$true'):
            self.assertIn(contract, self.harness)
        schema_source = (HERE.parent.parent / 'lib/data/store_schema.dart').read_text(encoding='utf-8')
        self.assertRegex(schema_source, r'const currentStoreSchema = 2;')
        self.assertIn('readerSchemaMin=2; readerSchemaMax=2; storeSchema=2', self.harness)
        self.assertIn('Assert-RecoveryPayloads $recoveryManifestData', self.harness)
        self.assertIn('-B $recoveryHelper validate-manifest', self.harness)


if __name__ == "__main__":
    unittest.main()

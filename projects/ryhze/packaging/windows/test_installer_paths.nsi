; No-install native regression fixture. Compile with /DOUTPUT_FILE=<new exe>.
; Performs string/API checks only; never creates an install root or registry key.
Unicode true
!include "LogicLib.nsh"
!ifndef OUTPUT_FILE
  !error "Pass /DOUTPUT_FILE=<new evidence executable>"
!endif
Name "Ryhze installer path regression"
OutFile "${OUTPUT_FILE}"
RequestExecutionLevel user
SilentInstall silent
AutoCloseWindow true
!define INSTALLER_ACCEPTANCE 1
!include "installer_diagnostics.nsh"
!include "installer_paths.nsh"
Var PathTestFailures

!macro CheckPath ID INPUT EXPECTED
  StrCpy $TargetPathInput "${INPUT}"
  Call NormalizeInstallTargetPath
  StrCpy $0 0
  ${If} $TargetPathError != 0
    StrCpy $0 1
  ${EndIf}
  !insertmacro AcceptanceTrace "${ID}" "$TargetPathError"
  ${If} $0 != ${EXPECTED}
    IntOp $PathTestFailures $PathTestFailures + 1
  ${EndIf}
!macroend

Function .onInit
  StrCpy $PathTestFailures 0
  ; Reproduce the previous in-place NSIS instruction without mutating disk.
  StrCpy $0 "$LOCALAPPDATA\Programs\Ryhze-Installer-Acceptance"
  GetFullPathName $1 "$0"
  StrLen $2 "$1"
  !insertmacro AcceptanceTrace "old-independent-length" "$2"
  GetFullPathName $0 "$0"
  StrLen $2 "$0"
  !insertmacro AcceptanceTrace "old-in-place-length" "$2"
  StrCpy $0 "C:\Synthetic\Ryhze-Entertainment" 2 1
  StrCpy $2 1
  ${If} $0 == ":\"
    StrCpy $2 0
  ${EndIf}
  !insertmacro AcceptanceTrace "prefix-literal-failed" "$2"
  IntOp $PathTestFailures $PathTestFailures + $2
  ; The exact absent destination that previously failed fresh installation.
  !insertmacro CheckPath "acceptance-path" "$LOCALAPPDATA\Programs\Ryhze-Installer-Acceptance" 0
  ${If} $TargetPathNormalized == ""
    IntOp $PathTestFailures $PathTestFailures + 1
  ${EndIf}
  !insertmacro CheckPath "missing-nested-path" "$LOCALAPPDATA\Ryhze-Path-Probe-Never-Created\nested\Ryhze-Entertainment" 0
  !insertmacro CheckPath "local-uppercase" "C:\Synthetic\Ryhze-Entertainment" 0
  !insertmacro CheckPath "local-lowercase" "c:\Synthetic\Ryhze-Entertainment" 0
  !insertmacro CheckPath "empty" "" 1
  !insertmacro CheckPath "relative" "Synthetic\Ryhze-Entertainment" 1
  !insertmacro CheckPath "drive-relative" "C:Synthetic\Ryhze-Entertainment" 1
  !insertmacro CheckPath "root-relative" "\Synthetic\Ryhze-Entertainment" 1
  !insertmacro CheckPath "unc" "\\server\share\Ryhze-Entertainment" 1
  !insertmacro CheckPath "device" "\\.\C:\Synthetic\Ryhze-Entertainment" 1
  !insertmacro CheckPath "extended" "\\?\C:\Synthetic\Ryhze-Entertainment" 1
  !insertmacro CheckPath "invalid-drive" "1:\Synthetic\Ryhze-Entertainment" 1
  !insertmacro CheckPath "forward-slash" "C:/Synthetic/Ryhze-Entertainment" 1
  !insertmacro AcceptanceTrace "path-tests-failed" "$PathTestFailures"
  SetErrorLevel $PathTestFailures
  Quit
FunctionEnd
Section
SectionEnd

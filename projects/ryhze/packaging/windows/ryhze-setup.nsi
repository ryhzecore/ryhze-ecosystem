; Fresh, isolated Windows installer foundation. See README.md before compiling.
; BUNDLE_DIR, APP_VERSION and PACKAGE_SUPPORT_DIR are mandatory build inputs.
Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

!ifndef BUNDLE_DIR
  !error "Pass /DBUNDLE_DIR=<verified Windows release bundle>"
!endif
!ifndef APP_VERSION
  !error "Pass /DAPP_VERSION=<approved release version>; no default is supplied"
!endif
!if "${APP_VERSION}" == ""
  !error "APP_VERSION must not be empty"
!endif
!ifndef PACKAGE_SUPPORT_DIR
  !error "Run prepare_bundle.py and pass /DPACKAGE_SUPPORT_DIR=<its output folder>"
!endif
!ifndef OUTPUT_FILE
  !define OUTPUT_FILE "Ryhze-${APP_VERSION}-Windows-Setup.exe"
!endif

!include "installer_identity.nsh"
!include "${PACKAGE_SUPPORT_DIR}\bundle.nsh"

Name "${DISPLAY_NAME}"
Caption "${DISPLAY_NAME} setup"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\${INSTALL_BASENAME}"
VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "${DISPLAY_NAME}"
VIAddVersionKey /LANG=1033 "FileDescription" "${DISPLAY_NAME} installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${APP_VERSION}"
RequestExecutionLevel user
AllowRootDirInstall false
SetCompressor /SOLID lzma
SetOverwrite off
ShowInstDetails show
ShowUninstDetails show
BrandingText "Ryhze  |  Entertainment"
; Shared website palette: surface, raised, text, accent and primary blue.
InstallColors 165BFA 111214

!define MUI_BGCOLOR "111214"
!define MUI_TEXTCOLOR "F7F7F8"
!define MUI_DIRECTORYPAGE_BGCOLOR "18191C"
!define MUI_INSTFILESPAGE_COLORS "84B6FF 111214"
!define MUI_INSTFILESPAGE_PROGRESSBAR "colored"
!define MUI_WELCOMEFINISHPAGE_BITMAP "${PACKAGE_SUPPORT_DIR}\wizard.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "${PACKAGE_SUPPORT_DIR}\wizard.bmp"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Welcome to Ryhze"
!define MUI_WELCOMEPAGE_TEXT "Choose a folder, install Ryhze for your account, and start exploring.$\r$\n$\r$\nThis setup does not require administrator access. Existing libraries, downloads and preferences are kept separate."
!insertmacro MUI_PAGE_WELCOME
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE CheckDirectoryPage
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TITLE "Ryhze is ready"
!define MUI_FINISHPAGE_TEXT "Ryhze has been installed for your account.$\r$\n$\r$\nYou can open it from the Start menu."
!ifndef INSTALLER_ACCEPTANCE
  !define MUI_FINISHPAGE_RUN "$INSTDIR\app\ryhze.exe"
  !define MUI_FINISHPAGE_RUN_TEXT "Launch Ryhze"
!endif
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var InstallError
Var RemovalFailed
Var PathCheck
Var PathParent
!include "installer_diagnostics.nsh"
!include "installer_paths.nsh"
!insertmacro DefineNormalizeInstallTargetPath "un."

; No removal may traverse an application directory replaced by a junction/link.
!macro AssertNoReparsePoint DIRECTORY
  System::Call 'kernel32::GetFileAttributesW(w "${DIRECTORY}") i .r8 ?e'
  Pop $7
  ${If} $8 != -1
    IntOp $9 $8 & 0x400
    ${If} $9 != 0
      !insertmacro AcceptanceTrace "path-reparse-refused" 1
      MessageBox MB_ICONSTOP "This installation contains a linked path. Setup will not modify it." /SD IDOK
      SetErrorLevel 1
      Abort
    ${EndIf}
  ${ElseIf} $7 != 2
  ${AndIf} $7 != 3
    !insertmacro AcceptanceTrace "path-inspection-failed" "$7"
    MessageBox MB_ICONSTOP "Setup cannot verify a path safely. No further files will be changed." /SD IDOK
    SetErrorLevel 1
    Abort
  ${EndIf}
!macroend

!macro AssertNoReparseAncestors DIRECTORY
  StrCpy $PathCheck "${DIRECTORY}"
  ${Do}
    !insertmacro AssertNoReparsePoint "$PathCheck"
    ${GetParent} "$PathCheck" $PathParent
    ${If} $PathParent == ""
    ${OrIf} $PathParent == $PathCheck
      ${ExitDo}
    ${EndIf}
    StrCpy $PathCheck $PathParent
  ${Loop}
!macroend

!include "installer_registration.nsh"
!include "installer_shortcut.nsh"

Function .onInit
  SetShellVarContext current
  !insertmacro AcceptanceTrace "setup-init" 0
  ${IfNot} ${RunningX64}
    !insertmacro AcceptanceTrace "platform-refused" 20
    MessageBox MB_ICONSTOP "This package requires 64-bit Windows." /SD IDOK
    SetErrorLevel 20
    Abort
  ${EndIf}
  System::Call 'kernel32::CreateMutexW(p 0, i 0, w "Local\${PRODUCT_ID}.Installer") p .r0 ?e'
  Pop $1
  StrCpy $InstallerMutex $0
  !insertmacro AcceptanceTrace "setup-mutex-status" "$1"
  ${If} $0 P= 0
  ${OrIf} $1 == 183
    MessageBox MB_ICONSTOP "Another setup with this identity is already running, or setup cannot acquire its lock." /SD IDOK
    !insertmacro AcceptanceTrace "setup-mutex-refused" 21
    SetErrorLevel 21
    Abort
  ${EndIf}
  Call CheckRegistrationAvailable
  !insertmacro AcceptanceTrace "setup-init-complete" 0
FunctionEnd

Function ValidateInstallTarget
  StrCpy $InstallError ""
  !insertmacro AcceptanceTrace "target-check" 0
  StrCpy $TargetPathInput "$INSTDIR"
  Call NormalizeInstallTargetPath
  !insertmacro AcceptanceTrace "target-normalization-status" "$TargetPathError"
  ${If} $TargetPathError != 0
    StrCpy $InstallError "Choose a dedicated folder on a local drive. Setup could not verify this path."
    Return
  ${EndIf}
  StrCpy $INSTDIR "$TargetPathNormalized"
!ifdef INSTALLER_ACCEPTANCE
  StrCpy $TargetPathInput "$LOCALAPPDATA\Programs\Ryhze-Installer-Acceptance"
  Call NormalizeInstallTargetPath
  !insertmacro AcceptanceTrace "acceptance-normalization-status" "$TargetPathError"
  ${If} $TargetPathError != 0
  ${OrIf} $INSTDIR != $TargetPathNormalized
    !insertmacro AcceptanceTrace "acceptance-target-mismatch" 26
    StrCpy $InstallError "The acceptance package uses only its isolated per-user Programs folder."
    Return
  ${EndIf}
!endif
  ; The shared normalizer already rejected UNC/device/relative roots.
  StrCpy $0 "$INSTDIR" 3
  System::Call 'kernel32::GetDriveTypeW(w r0) i .r1'
  !insertmacro AcceptanceTrace "target-drive-type" "$1"
  ${If} $1 != 3
  ${AndIf} $1 != 2
    StrCpy $InstallError "Choose a local fixed or removable drive."
    Return
  ${EndIf}
  ${GetFileName} "$INSTDIR" $0
  ${If} $0 != "${INSTALL_BASENAME}"
    !insertmacro AcceptanceTrace "target-basename-mismatch" 28
    StrCpy $InstallError "Choose a dedicated folder named ${INSTALL_BASENAME}."
    Return
  ${EndIf}
  !insertmacro AssertNoReparseAncestors "$INSTDIR"
  ClearErrors
  FindFirst $0 $1 "$INSTDIR\*"
  IfErrors empty_directory
  scan_directory:
    StrCmp $1 "." next_entry
    StrCmp $1 ".." next_entry
    FindClose $0
    !insertmacro AcceptanceTrace "target-nonempty" 29
    StrCpy $InstallError "The selected folder is not empty. Choose a new, empty ${INSTALL_BASENAME} folder."
    Return
  next_entry:
    ClearErrors
    FindNext $0 $1
    IfErrors end_scan scan_directory
  end_scan:
    FindClose $0
  empty_directory:
    !insertmacro AcceptanceTrace "target-empty" 0
FunctionEnd

Function CheckDirectoryPage
  Call ValidateInstallTarget
  ${If} $InstallError != ""
    !insertmacro AcceptanceTrace "target-refused" 26
    MessageBox MB_ICONEXCLAMATION "$InstallError" /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}
FunctionEnd

Section "Ryhze application"
  !insertmacro AcceptanceTrace "install-section" 0
  ; Repeat validation for silent installs and changes since the directory page.
  Call CheckDirectoryPage
  Call CheckRegistrationAvailable
  !insertmacro AcceptanceTrace "preflight-complete" 0
  ClearErrors
  CreateDirectory "$INSTDIR"
  ${If} ${Errors}
    !insertmacro AcceptanceTrace "target-create-failed" 30
    MessageBox MB_ICONSTOP "The selected folder is not writable by your account." /SD IDOK
    SetErrorLevel 3
    Abort
  ${EndIf}
  ; Write an ownership marker and recovery uninstaller before extracting files.
  System::Call 'ole32::CoCreateGuid(g .r0) i .r1'
  ${If} $1 != 0
    !insertmacro AcceptanceTrace "owner-create-failed" "$1"
    SetErrorLevel 3
    Abort
  ${EndIf}
  StrCpy $InstallId $0
  ClearErrors
  WriteINIStr "$INSTDIR\${OWNER_MARKER}" "Install" "Root" "$INSTDIR"
  WriteINIStr "$INSTDIR\${OWNER_MARKER}" "Install" "Product" "${PRODUCT_ID}"
  WriteINIStr "$INSTDIR\${OWNER_MARKER}" "Install" "OwnerId" "$InstallId"
  WriteINIStr "$INSTDIR\${OWNER_MARKER}" "Install" "Version" "${APP_VERSION}"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  ${If} ${Errors}
    !insertmacro AcceptanceTrace "recovery-metadata-failed" 31
    MessageBox MB_ICONSTOP "Setup could not create its recovery files. No application files were installed." /SD IDOK
    SetErrorLevel 3
    Abort
  ${EndIf}
  ClearErrors
  !insertmacro AcceptanceTrace "payload-copy-start" 0
  ClearErrors
  !insertmacro InstallPayload
  ${If} ${Errors}
    !insertmacro AcceptanceTrace "payload-copy-failed" 32
    MessageBox MB_ICONSTOP "Setup could not copy all application files. Close any running copy and use Uninstall.exe in the selected folder to remove this partial installation before retrying." /SD IDOK
    SetErrorLevel 1
    Abort
  ${EndIf}
  Call RegisterApplication
  !insertmacro AcceptanceTrace "install-complete" 0
SectionEnd

Function un.ValidateInstallRoot
  StrCpy $TargetPathInput "$INSTDIR"
  Call un.NormalizeInstallTargetPath
  !insertmacro AcceptanceTrace "removal-normalization-status" "$TargetPathError"
  ${If} $TargetPathError != 0
    MessageBox MB_ICONSTOP "Setup cannot verify this installation path. No files will be removed." /SD IDOK
    SetErrorLevel 1
    Abort
  ${EndIf}
  StrCpy $INSTDIR "$TargetPathNormalized"
  !insertmacro AssertNoReparseAncestors "$INSTDIR"
  !insertmacro AssertNoReparseAncestors "$SMPROGRAMS"
  !insertmacro AssertNoReparsePoint "$INSTDIR\${OWNER_MARKER}"
  ReadINIStr $0 "$INSTDIR\${OWNER_MARKER}" "Install" "Root"
  ReadINIStr $1 "$INSTDIR\${OWNER_MARKER}" "Install" "Product"
  ReadINIStr $InstallId "$INSTDIR\${OWNER_MARKER}" "Install" "OwnerId"
  ${If} $0 != $INSTDIR
  ${OrIf} $1 != "${PRODUCT_ID}"
  ${OrIf} $InstallId == ""
    MessageBox MB_ICONSTOP "The installation ownership record does not match this folder. Nothing will be removed." /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}
  !insertmacro AssertNoReparsePoint "$INSTDIR"
  !insertmacro ValidatePayloadDirectories
FunctionEnd

Function un.onInit
  SetShellVarContext current
  !insertmacro AcceptanceTrace "uninstall-init" 0
  Call un.ValidateInstallRoot
FunctionEnd

Section "Uninstall"
  !insertmacro AcceptanceTrace "uninstall-section" 0
  Call un.ValidateInstallRoot
  StrCpy $RemovalFailed 0
  SetOutPath "$TEMP"
  !insertmacro RemovePayload
  ${If} $RemovalFailed != 0
    !insertmacro AcceptanceTrace "payload-removal-incomplete" 33
    MessageBox MB_ICONEXCLAMATION "Some application files are in use or could not be removed. Close Ryhze and run this uninstaller again. Your preferences and downloads have not been removed." /SD IDOK
    SetErrorLevel 1
    Abort
  ${EndIf}
  Call un.UnregisterApplication
  Delete "$INSTDIR\${OWNER_MARKER}"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  !insertmacro AcceptanceTrace "uninstall-complete" 0
  ; No recursive deletion, preference cleanup, cache cleanup or download cleanup.
SectionEnd

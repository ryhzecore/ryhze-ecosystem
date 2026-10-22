; Current-user registration only. Unknown/reassigned state is never reclaimed.
!include "Win\COM.nsh"
!include "Win\Propkey.nsh"
Var InstallId
Var ShortcutOwned
Var ShortcutFile
Var RegistrationError
Var InstallerMutex

!macro RequireAbsentRegistryKey ROOT KEY STAGE
  System::Call 'advapi32::RegOpenKeyExW(p ${ROOT}, w "${KEY}", i 0, i 0x20019, *p .r1) i .r0'
  !insertmacro AcceptanceTrace "${STAGE}" "$0"
  ${If} $0 == 0
    System::Call 'advapi32::RegCloseKey(p r1)'
    StrCpy $RegistrationError "An existing registration must be preserved: ${KEY}"
  ${ElseIf} $0 != 2
    StrCpy $RegistrationError "Setup cannot inspect registration ownership."
  ${EndIf}
!macroend

Function CheckRegistrationAvailable
  StrCpy $RegistrationError ""
  !insertmacro AcceptanceTrace "registration-preflight" 0
  !insertmacro AssertNoReparseAncestors "$SMPROGRAMS"
  !insertmacro RequireAbsentRegistryKey 0x80000001 "${PRODUCT_KEY}" "registry-product-status"
  !insertmacro RequireAbsentRegistryKey 0x80000001 "${UNINSTALL_KEY}" "registry-uninstall-status"
  ; The merged view also refuses a pre-existing machine-wide scheme handler.
  !insertmacro RequireAbsentRegistryKey 0x80000000 "${URI_SCHEME}" "registry-protocol-status"
  System::Call 'kernel32::GetFileAttributesW(w "$SMPROGRAMS\${SHORTCUT_NAME}") i .r0 ?e'
  Pop $1
  !insertmacro AcceptanceTrace "shortcut-attribute-status" "$1"
  ${If} $0 != -1
    StrCpy $RegistrationError "An existing Start-menu shortcut must be preserved."
  ${ElseIf} $1 != 2
  ${AndIf} $1 != 3
    StrCpy $RegistrationError "Setup cannot inspect the Start-menu shortcut safely."
  ${EndIf}
  ${If} $RegistrationError != ""
    !insertmacro AcceptanceTrace "registration-refused" 22
    MessageBox MB_ICONSTOP "$RegistrationError" /SD IDOK
    SetErrorLevel 22
    Abort
  ${EndIf}
  !insertmacro AcceptanceTrace "registration-preflight-complete" 0
FunctionEnd

; Disposition is checked before any value is written; another installer cannot
; turn an absent-key observation into permission to overwrite its registration.
!macro CreateFreshRegistryKey KEY
  System::Call 'advapi32::RegCreateKeyExW(p 0x80000001, w "${KEY}", i 0, p 0, i 0, i 0x2001f, p 0, *p .r1, *i .r2) i .r0'
  ${If} $0 == 0
    System::Call 'advapi32::RegCloseKey(p r1)'
  ${EndIf}
  ${If} $0 != 0
  ${OrIf} $2 != 1
    MessageBox MB_ICONSTOP "Registration changed or is not writable. The partial installation is retained with its recovery uninstaller." /SD IDOK
    SetErrorLevel 3
    Abort
  ${EndIf}
  WriteRegStr HKCU "${KEY}" "RyhzeOwnerId" "$InstallId"
!macroend

!macro DeleteMatchingString KEY NAME EXPECTED
  ClearErrors
  ReadRegStr $0 HKCU "${KEY}" "${NAME}"
  ${IfNot} ${Errors}
    ${If} $0 == "${EXPECTED}"
      DeleteRegValue HKCU "${KEY}" "${NAME}"
    ${EndIf}
  ${EndIf}
!macroend
!macro DeleteMatchingDword KEY NAME EXPECTED
  ClearErrors
  ReadRegDWORD $0 HKCU "${KEY}" "${NAME}"
  ${IfNot} ${Errors}
    ${If} $0 == ${EXPECTED}
      DeleteRegValue HKCU "${KEY}" "${NAME}"
    ${EndIf}
  ${EndIf}
!macroend
!macro DeleteEmptyRegistryKey KEY
  ; Explicitly check values and subkeys before /ifempty, including for older
  ; NSIS variants whose /ifempty guarded only subkeys.
  ClearErrors
  EnumRegValue $0 HKCU "${KEY}" 0
  ${If} ${Errors}
    ClearErrors
    EnumRegKey $0 HKCU "${KEY}" 0
    ; EnumRegKey ends with an empty name WITHOUT Errors. EnumRegValue above
    ; instead sets Errors when no values remain. An actual key-read error
    ; never establishes emptiness and must preserve the registration.
    ${IfNot} ${Errors}
    ${AndIf} $0 == ""
      DeleteRegKey /ifempty HKCU "${KEY}"
    ${EndIf}
  ${EndIf}
!macroend

Function RegisterApplication
  !insertmacro AcceptanceTrace "registration-write-start" 0
  Call CheckRegistrationAvailable
  ClearErrors
  !insertmacro CreateFreshRegistryKey "${PRODUCT_KEY}"
  WriteRegStr HKCU "${PRODUCT_KEY}" "InstallDir" "$INSTDIR"
  !insertmacro CreateFreshRegistryKey "${UNINSTALL_KEY}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" "${UNINSTALL_COMMAND}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${DISPLAY_NAME}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\app\ryhze.exe"
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
  !insertmacro CreateFreshRegistryKey "${PROTOCOL_KEY}"
  WriteRegStr HKCU "${PROTOCOL_KEY}" "RyhzeInstallRoot" "$INSTDIR"
  WriteRegStr HKCU "${PROTOCOL_KEY}" "" "URL:${DISPLAY_NAME} Protocol"
  WriteRegStr HKCU "${PROTOCOL_KEY}" "URL Protocol" ""
  WriteRegStr HKCU "${PROTOCOL_KEY}\DefaultIcon" "" '$\"$INSTDIR\app\ryhze.exe$\",0'
  WriteRegStr HKCU "${PROTOCOL_KEY}\shell\open\command" "" "${PROTOCOL_COMMAND}"
  ${If} ${Errors}
    MessageBox MB_ICONSTOP "Setup could not complete registration. Use the retained recovery uninstaller before retrying." /SD IDOK
    SetErrorLevel 3
    Abort
  ${EndIf}
  StrCpy $ShortcutFile "$SMPROGRAMS\${SHORTCUT_NAME}"
  Call CreateOwnedShortcut
  ${If} $RegistrationError != ""
    !insertmacro AcceptanceTrace "shortcut-create-failed" 34
    MessageBox MB_ICONSTOP "$RegistrationError" /SD IDOK
    SetErrorLevel 3
    Abort
  ${EndIf}
  !insertmacro AcceptanceTrace "registration-write-complete" 0
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
FunctionEnd

Function un.UnregisterApplication
  ReadRegStr $1 HKCU "${PROTOCOL_KEY}" "RyhzeOwnerId"
  ReadRegStr $2 HKCU "${PROTOCOL_KEY}" "RyhzeInstallRoot"
  ReadRegStr $3 HKCU "${PROTOCOL_KEY}\shell\open\command" ""
  ${If} $1 == $InstallId
  ${AndIf} $2 == $INSTDIR
  ${AndIf} $3 == "${PROTOCOL_COMMAND}"
    !insertmacro DeleteMatchingString "${PROTOCOL_KEY}\shell\open\command" "" "${PROTOCOL_COMMAND}"
    !insertmacro DeleteEmptyRegistryKey "${PROTOCOL_KEY}\shell\open\command"
    !insertmacro DeleteEmptyRegistryKey "${PROTOCOL_KEY}\shell\open"
    !insertmacro DeleteEmptyRegistryKey "${PROTOCOL_KEY}\shell"
    !insertmacro DeleteMatchingString "${PROTOCOL_KEY}\DefaultIcon" "" '$\"$INSTDIR\app\ryhze.exe$\",0'
    !insertmacro DeleteEmptyRegistryKey "${PROTOCOL_KEY}\DefaultIcon"
    !insertmacro DeleteMatchingString "${PROTOCOL_KEY}" "" "URL:${DISPLAY_NAME} Protocol"
    !insertmacro DeleteMatchingString "${PROTOCOL_KEY}" "URL Protocol" ""
    !insertmacro DeleteMatchingString "${PROTOCOL_KEY}" "RyhzeInstallRoot" "$INSTDIR"
    !insertmacro DeleteMatchingString "${PROTOCOL_KEY}" "RyhzeOwnerId" "$InstallId"
    !insertmacro DeleteEmptyRegistryKey "${PROTOCOL_KEY}"
  ${EndIf}
  ReadRegStr $1 HKCU "${UNINSTALL_KEY}" "RyhzeOwnerId"
  ReadRegStr $2 HKCU "${UNINSTALL_KEY}" "InstallLocation"
  ReadRegStr $3 HKCU "${UNINSTALL_KEY}" "UninstallString"
  ${If} $1 == $InstallId
  ${AndIf} $2 == $INSTDIR
  ${AndIf} $3 == "${UNINSTALL_COMMAND}"
    !insertmacro DeleteMatchingString "${UNINSTALL_KEY}" "DisplayName" "${DISPLAY_NAME}"
    !insertmacro DeleteMatchingString "${UNINSTALL_KEY}" "DisplayVersion" "${APP_VERSION}"
    !insertmacro DeleteMatchingString "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\app\ryhze.exe"
    !insertmacro DeleteMatchingDword "${UNINSTALL_KEY}" "NoModify" 1
    !insertmacro DeleteMatchingDword "${UNINSTALL_KEY}" "NoRepair" 1
    !insertmacro DeleteMatchingString "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    !insertmacro DeleteMatchingString "${UNINSTALL_KEY}" "UninstallString" "${UNINSTALL_COMMAND}"
    !insertmacro DeleteMatchingString "${UNINSTALL_KEY}" "RyhzeOwnerId" "$InstallId"
    !insertmacro DeleteEmptyRegistryKey "${UNINSTALL_KEY}"
  ${EndIf}
  ReadRegStr $1 HKCU "${PRODUCT_KEY}" "RyhzeOwnerId"
  ReadRegStr $2 HKCU "${PRODUCT_KEY}" "InstallDir"
  ${If} $1 == $InstallId
  ${AndIf} $2 == $INSTDIR
    !insertmacro DeleteMatchingString "${PRODUCT_KEY}" "InstallDir" "$INSTDIR"
    !insertmacro DeleteMatchingString "${PRODUCT_KEY}" "RyhzeOwnerId" "$InstallId"
    !insertmacro DeleteEmptyRegistryKey "${PRODUCT_KEY}"
  ${EndIf}
  StrCpy $ShortcutFile "$SMPROGRAMS\${SHORTCUT_NAME}"
  Call un.CheckOwnedShortcut
  ${If} $ShortcutOwned == 1
    Delete "$SMPROGRAMS\${SHORTCUT_NAME}"
  ${EndIf}
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
FunctionEnd

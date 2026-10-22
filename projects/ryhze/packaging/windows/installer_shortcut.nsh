; Standard NSIS COM support only; no shell/plugin download or COM registration.
; Protocol-only toast activator is a stub CLSID, never a LocalServer32 command.
; Registration callers set the fixed Start-menu path; the native stress fixture
; injects only its new private fixture path, without changing shell folders.
!macro AllocateShortcutPropertyBuffers FAILURE_LABEL
  System::Call '*${SYSSTRUCT_PROPERTYKEY}(${PKEY_AppUserModel_ID}) p .r3'
  ${If} $3 P= 0
    Goto ${FAILURE_LABEL}
  ${EndIf}
  System::Call 'kernel32::GlobalSize(p r3) p .r5'
  ${If} $5 < ${SYSSIZEOF_PROPERTYKEY}
    System::Free $3
    StrCpy $3 0
    Goto ${FAILURE_LABEL}
  ${EndIf}
  ; Do NOT append empty (). System parses a second argument list as a new
  ; parameter count: () would request zero bytes (GlobalSize observed1), then
  ; PROPVARIANT access overruns that buffer. The declared layout is complete.
  System::Call '*${SYSSTRUCT_PROPVARIANT} p .r4'
  ${If} $4 P= 0
    Goto ${FAILURE_LABEL}
  ${EndIf}
  System::Call 'kernel32::GlobalSize(p r4) p .r5'
  ${If} $5 < ${SYSSIZEOF_PROPVARIANT}
    System::Free $4
    StrCpy $4 0
    Goto ${FAILURE_LABEL}
  ${EndIf}
!macroend

Function CreateOwnedShortcut
  StrCpy $RegistrationError "Could not create and verify the owned Start-menu shortcut."
  StrCpy $0 0
  StrCpy $1 0
  StrCpy $2 0
  StrCpy $3 0
  StrCpy $4 0
  System::Call 'ole32::CoInitializeEx(p 0, i 2) i .r9'
  ${If} $9 < 0
    Return
  ${EndIf}
  !insertmacro ComHlpr_CreateInProcInstance ${CLSID_ShellLink} ${IID_IShellLink} r0 ".r5"
  ${If} $5 != 0
  ${OrIf} $0 P= 0
    Goto shortcut_create_done
  ${EndIf}
  ${IShellLink::SetPath} $0 '("$INSTDIR\app\ryhze.exe").r5'
  ${If} $5 != 0
    Goto shortcut_create_done
  ${EndIf}
  ${IShellLink::SetArguments} $0 '("").r5'
  ${If} $5 != 0
    Goto shortcut_create_done
  ${EndIf}
  ${IShellLink::SetDescription} $0 '("${PRODUCT_ID}|$InstallId").r5'
  ${If} $5 != 0
    Goto shortcut_create_done
  ${EndIf}
  ${IShellLink::SetWorkingDirectory} $0 '("$INSTDIR\app").r5'
  ${If} $5 != 0
    Goto shortcut_create_done
  ${EndIf}
  ${IUnknown::QueryInterface} $0 '("${IID_IPropertyStore}",.r2).r5'
  ${If} $5 != 0
  ${OrIf} $2 P= 0
    Goto shortcut_create_done
  ${EndIf}
  !insertmacro AllocateShortcutPropertyBuffers shortcut_create_done
  ; InitPropVariantFromString is inline in the SDK; SHStrDupW uses the allocator
  ; required by PropVariantClear for a VT_LPWSTR value.
  System::Call 'shlwapi::SHStrDupW(w "${APP_USER_MODEL_ID}", *p .r6) i .r5'
  ${If} $5 != 0
    Goto shortcut_create_done
  ${EndIf}
  System::Call '*$4(&i2 ${VT_LPWSTR}, &i6, p r6)'
  ${IPropertyStore::SetValue} $2 '($3,$4).r5'
  System::Call 'ole32::PropVariantClear(p r4)'
  ${If} $5 != 0
    Goto shortcut_create_done
  ${EndIf}
  System::Call '*$3${SYSSTRUCT_PROPERTYKEY}(${PKEY_AppUserModel_ToastActivatorCLSID})'
  System::Call 'ole32::CoTaskMemAlloc(p 16) p .r6'
  ${If} $6 P= 0
    Goto shortcut_create_done
  ${EndIf}
  System::Call '*$6(&g16 "${TOAST_ACTIVATOR_CLSID}")'
  System::Call '*$4(&i2 ${VT_CLSID}, &i6, p r6)'
  ${IPropertyStore::SetValue} $2 '($3,$4).r5'
  System::Call 'ole32::PropVariantClear(p r4)'
  ${If} $5 != 0
    Goto shortcut_create_done
  ${EndIf}
  ${IPropertyStore::Commit} $2 '().r5'
  ${If} $5 != 0
    Goto shortcut_create_done
  ${EndIf}
  ${IUnknown::QueryInterface} $0 '("${IID_IPersistFile}",.r1).r5'
  ${If} $5 != 0
  ${OrIf} $1 P= 0
    Goto shortcut_create_done
  ${EndIf}
  ; Recheck immediately before save; this installer never replaces a shortcut.
  System::Call 'kernel32::GetFileAttributesW(w "$ShortcutFile") i .r5 ?e'
  Pop $6
  ${If} $5 != -1
    Goto shortcut_create_done
  ${EndIf}
  ${If} $6 != 2
  ${AndIf} $6 != 3
    Goto shortcut_create_done
  ${EndIf}
  !insertmacro AcceptanceTrace "shortcut-save-start" 0
  ${IPersistFile::Save} $1 '("$ShortcutFile",1).r5'
  !insertmacro AcceptanceTrace "shortcut-save-status" "$5"
  ${If} $5 == 0
    StrCpy $RegistrationError ""
  ${EndIf}
  shortcut_create_done:
  !insertmacro ComHlpr_SafeRelease $1
  !insertmacro ComHlpr_SafeRelease $2
  !insertmacro ComHlpr_SafeRelease $0
  System::Free $3
  System::Free $4
  System::Call 'ole32::CoUninitialize()'
  ${If} $RegistrationError == ""
    Call CheckOwnedShortcut
    ${If} $ShortcutOwned != 1
      StrCpy $RegistrationError "The installed shortcut did not match its ownership record."
    ${EndIf}
  ${EndIf}
FunctionEnd

!macro DefineCheckOwnedShortcut PREFIX
Function ${PREFIX}CheckOwnedShortcut
  StrCpy $ShortcutOwned 0
  StrCpy $0 0
  StrCpy $1 0
  StrCpy $2 0
  StrCpy $3 0
  StrCpy $4 0
  System::Call 'kernel32::GetFileAttributesW(w "$ShortcutFile") i .r5'
  ${If} $5 == -1
    Return
  ${EndIf}
  IntOp $5 $5 & 0x410
  ${If} $5 != 0
    Return
  ${EndIf}
  System::Call 'ole32::CoInitializeEx(p 0, i 2) i .r9'
  ${If} $9 < 0
    Return
  ${EndIf}
  !insertmacro ComHlpr_CreateInProcInstance ${CLSID_ShellLink} ${IID_IShellLink} r0 ".r5"
  ${If} $5 != 0
  ${OrIf} $0 P= 0
    Goto shortcut_check_done
  ${EndIf}
  ${IUnknown::QueryInterface} $0 '("${IID_IPersistFile}",.r1).r5'
  ${If} $5 != 0
  ${OrIf} $1 P= 0
    Goto shortcut_check_done
  ${EndIf}
  ${IPersistFile::Load} $1 '("$ShortcutFile",${STGM_READ}).r5'
  ${If} $5 != 0
    Goto shortcut_check_done
  ${EndIf}
  ${IShellLink::GetPath} $0 '(.r6,${NSIS_MAX_STRLEN},0,4).r5'
  ${If} $5 != 0
  ${OrIf} $6 != "$INSTDIR\app\ryhze.exe"
    Goto shortcut_check_done
  ${EndIf}
  ${IShellLink::GetArguments} $0 '(.r6,${NSIS_MAX_STRLEN}).r5'
  ${If} $5 != 0
  ${OrIf} $6 != ""
    Goto shortcut_check_done
  ${EndIf}
  ${IShellLink::GetDescription} $0 '(.r6,${NSIS_MAX_STRLEN}).r5'
  ${If} $5 != 0
  ${OrIf} $6 != "${PRODUCT_ID}|$InstallId"
    Goto shortcut_check_done
  ${EndIf}
  ${IUnknown::QueryInterface} $0 '("${IID_IPropertyStore}",.r2).r5'
  ${If} $5 != 0
  ${OrIf} $2 P= 0
    Goto shortcut_check_done
  ${EndIf}
  !insertmacro AllocateShortcutPropertyBuffers shortcut_check_done
  ${IPropertyStore::GetValue} $2 '($3,$4).r5'
  ${If} $5 != 0
    Goto shortcut_check_done
  ${EndIf}
  System::Call '*$4(&i2 .r5,&i6,p .r6)'
  ${If} $5 != ${VT_LPWSTR}
  ${OrIf} $6 P= 0
    Goto shortcut_check_done
  ${EndIf}
  System::Call 'kernel32::lstrcpynW(w .r7,p r6,i ${NSIS_MAX_STRLEN}) p'
  ${If} $7 != "${APP_USER_MODEL_ID}"
    Goto shortcut_check_done
  ${EndIf}
  System::Call 'ole32::PropVariantClear(p r4)'
  System::Call '*$3${SYSSTRUCT_PROPERTYKEY}(${PKEY_AppUserModel_ToastActivatorCLSID})'
  ${IPropertyStore::GetValue} $2 '($3,$4).r5'
  ${If} $5 != 0
    Goto shortcut_check_done
  ${EndIf}
  System::Call '*$4(&i2 .r5,&i6,p .r6)'
  ${If} $5 != ${VT_CLSID}
  ${OrIf} $6 P= 0
    Goto shortcut_check_done
  ${EndIf}
  System::Call 'ole32::StringFromGUID2(p r6,w .r7,i 40) i .r5'
  ${If} $5 == 39
  ${AndIf} $7 == "${TOAST_ACTIVATOR_CLSID}"
    StrCpy $ShortcutOwned 1
  ${EndIf}
  shortcut_check_done:
  ${If} $4 P<> 0
    System::Call 'ole32::PropVariantClear(p r4)'
  ${EndIf}
  !insertmacro ComHlpr_SafeRelease $1
  !insertmacro ComHlpr_SafeRelease $2
  !insertmacro ComHlpr_SafeRelease $0
  System::Free $3
  System::Free $4
  System::Call 'ole32::CoUninitialize()'
FunctionEnd
!macroend
!insertmacro DefineCheckOwnedShortcut ""
!insertmacro DefineCheckOwnedShortcut "un."

; Bounded native COM allocation/save/load regression. No application launch,
; shell-folder changes, registration writes, deletion or overwriting.
Unicode true
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "Win\COM.nsh"
!include "Win\Propkey.nsh"
!ifndef OUTPUT_FILE
  !error "Supply a new /DOUTPUT_FILE=<evidence exe>"
!endif
!ifndef FIXTURE_DIR
  !error "Supply a new absolute /DFIXTURE_DIR=<private evidence folder>\Shortcuts"
!endif
Name "Ryhze shortcut regression"
OutFile "${OUTPUT_FILE}"
RequestExecutionLevel user
SilentInstall silent
AutoCloseWindow true
!define INSTALLER_ACCEPTANCE 1
!include "installer_identity.nsh"
!include "installer_diagnostics.nsh"
!include "installer_paths.nsh"
Var RegistrationError
Var InstallId
Var ShortcutOwned
Var ShortcutFile
Var FixtureDirectory
Var FixtureAncestor
Var FixtureParent
Var FixtureCycle
!include "installer_shortcut.nsh"

Function .onInit
  SetShellVarContext current
  StrCpy $FixtureDirectory "${FIXTURE_DIR}"
  StrCpy $TargetPathInput "$FixtureDirectory"
  Call NormalizeInstallTargetPath
  ${If} $TargetPathError != 0
  ${OrIf} $TargetPathNormalized != $FixtureDirectory
    Goto fixture_refused
  ${EndIf}
  ${GetFileName} "$FixtureDirectory" $0
  ${If} $0 != "Shortcuts"
    Goto fixture_refused
  ${EndIf}
  ; Refuse any existing root, including an empty one or a broken reparse point.
  System::Call 'kernel32::GetFileAttributesW(w "$FixtureDirectory") i .r0 ?e'
  Pop $1
  ${If} $0 != -1
    Goto fixture_refused
  ${EndIf}
  ${If} $1 != 2
  ${AndIf} $1 != 3
    Goto fixture_refused
  ${EndIf}
  ${GetParent} "$FixtureDirectory" $FixtureAncestor
  ${Do}
    System::Call 'kernel32::GetFileAttributesW(w "$FixtureAncestor") i .r0'
    ${If} $0 == -1
      Goto fixture_refused
    ${EndIf}
    IntOp $1 $0 & 0x410
    ; Compare numeric flags, not the decimal result string against "0x10".
    ${If} $1 <> 0x10
      Goto fixture_refused
    ${EndIf}
    ${GetParent} "$FixtureAncestor" $FixtureParent
    ${If} $FixtureParent == ""
    ${OrIf} $FixtureParent == $FixtureAncestor
      ${ExitDo}
    ${EndIf}
    StrCpy $FixtureAncestor "$FixtureParent"
  ${Loop}
  ; Observe the old zero-sized request WITHOUT writing or clearing its buffer.
  System::Call '*${SYSSTRUCT_PROPVARIANT}() p .r4'
  System::Call 'kernel32::GlobalSize(p r4) p .r5'
  !insertmacro AcceptanceTrace "old-propvariant-size" "$5"
  System::Free $4
  StrCpy $3 0
  StrCpy $4 0
  !insertmacro AllocateShortcutPropertyBuffers fixture_allocation_failed
  System::Call 'kernel32::GlobalSize(p r4) p .r5'
  !insertmacro AcceptanceTrace "fixed-propvariant-size" "$5"
  System::Call '*$4(&i2 .r5)'
  ${If} $5 != ${VT_EMPTY}
    Goto fixture_allocation_failed
  ${EndIf}
  System::Free $3
  System::Free $4
  StrCpy $3 0
  StrCpy $4 0
  ClearErrors
  CreateDirectory "$FixtureDirectory"
  ${If} ${Errors}
    Goto fixture_refused
  ${EndIf}
  StrCpy $INSTDIR "$FixtureDirectory"
  StrCpy $InstallId "{11111111-2222-3333-4444-555555555555}"
  StrCpy $FixtureCycle 0
  ${Do}
    IntOp $FixtureCycle $FixtureCycle + 1
    StrCpy $ShortcutFile "$FixtureDirectory\cycle-$FixtureCycle.lnk"
    Call CreateOwnedShortcut
    ${If} $RegistrationError != ""
      Goto fixture_failed
    ${EndIf}
    Call CheckOwnedShortcut
    ${If} $ShortcutOwned != 1
      Goto fixture_failed
    ${EndIf}
    ; The reader must reject a different claimed owner without modifying the link.
    StrCpy $InstallId "{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}"
    Call CheckOwnedShortcut
    ${If} $ShortcutOwned != 0
      Goto fixture_failed
    ${EndIf}
    StrCpy $InstallId "{11111111-2222-3333-4444-555555555555}"
  ${LoopUntil} $FixtureCycle == 100
  !insertmacro AcceptanceTrace "shortcut-cycles-passed" "$FixtureCycle"
  SetErrorLevel 0
  Quit
  fixture_allocation_failed:
    System::Free $3
    System::Free $4
  fixture_refused:
    !insertmacro AcceptanceTrace "shortcut-fixture-refused" 1
    SetErrorLevel 1
    Quit
  fixture_failed:
    !insertmacro AcceptanceTrace "shortcut-cycle-failed" "$FixtureCycle"
    SetErrorLevel 2
    Quit
FunctionEnd
Section
SectionEnd

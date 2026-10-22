; Acceptance-only, bounded diagnostic vocabulary. No paths, IDs, URI arguments,
; registry values, account data or exception text enter the redirected trace.
!ifdef INSTALLER_ACCEPTANCE
  Var AcceptanceTraceLine
!endif
!macro AcceptanceTrace STAGE CODE
!ifdef INSTALLER_ACCEPTANCE
  StrCpy $AcceptanceTraceLine "stage=${STAGE};code=${CODE}$\r$\n"
  Push $0
  Push $1
  Push $2
  Push $3
  System::Call 'kernel32::GetStdHandle(i -11) p .r0'
  ${If} $0 P<> 0
  ${AndIf} $0 != -1
    StrLen $1 $AcceptanceTraceLine
    System::Call 'kernel32::WriteFile(p r0,m "$AcceptanceTraceLine",i r1,*i .r2,p 0) i .r3'
  ${EndIf}
  Pop $3
  Pop $2
  Pop $1
  Pop $0
!endif
!macroend

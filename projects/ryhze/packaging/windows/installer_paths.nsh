; Lexical normalization for a fresh destination that need not exist yet.
; NSIS GetFullPathName may clear its output on lookup failure; two empty
; outputs must never count as an exact match. Keep input/output distinct.
Var TargetPathInput
Var TargetPathNormalized
Var TargetPathError

!macro DefineNormalizeInstallTargetPath PREFIX
Function ${PREFIX}NormalizeInstallTargetPath
  Push $0
  Push $1
  Push $2
  StrCpy $TargetPathNormalized ""
  StrCpy $TargetPathError 1
  ; Require a drive-qualified absolute input BEFORE normalization. In
  ; particular, GetFullPathNameW must not turn a relative/device/UNC input
  ; into an acceptable local path using the process's current directory.
  StrCpy $0 "$TargetPathInput" 2 1
  StrCmp $0 ":\" 0 normalize_path_done
  StrCpy $0 "$TargetPathInput" 3
  System::Call 'shlwapi::PathGetDriveNumberW(w r0) i .r1'
  IntCmp $1 0 drive_letter_valid normalize_path_done drive_letter_valid
  drive_letter_valid:
  IntCmp $1 25 normalize_path_call normalize_path_call normalize_path_done
  normalize_path_call:
  StrCpy $TargetPathError 2
  ; Win32 explicitly does not require the destination to exist. Its return
  ; value distinguishes failure/truncation from a complete nonempty path.
  System::Call 'kernel32::GetFullPathNameW(w "$TargetPathInput", i ${NSIS_MAX_STRLEN}, w .r0, p 0) i .r1'
  IntCmp $1 0 normalize_path_done normalize_path_done normalize_path_length
  normalize_path_length:
  StrCpy $TargetPathError 3
  IntCmp $1 ${NSIS_MAX_STRLEN} normalize_path_done normalize_path_root normalize_path_done
  normalize_path_root:
  StrCpy $TargetPathError 4
  StrCpy $1 "$TargetPathInput" 3
  StrCpy $2 "$0" 3
  StrCmp $1 $2 0 normalize_path_done
  StrCpy $TargetPathNormalized "$0"
  StrCpy $TargetPathError 0
  normalize_path_done:
  Pop $2
  Pop $1
  Pop $0
FunctionEnd
!macroend

!insertmacro DefineNormalizeInstallTargetPath ""

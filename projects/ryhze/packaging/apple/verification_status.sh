#!/usr/bin/env bash
# Shared result accounting. The caller supplies evidence and run_phase.
verification_failed=0

record_phase() {
  python3 - "$evidence/phase-status.json" "$1" "$2" "$3" "${4:-}" <<'PHASE_STATUS'
import json, pathlib, sys
path, name, conclusion, code, reason = sys.argv[1:]
target = pathlib.Path(path)
data = json.loads(target.read_text()) if target.exists() else {'schema': 1, 'phases': []}
data['phases'].append({'name': name, 'conclusion': conclusion,
                       'exit_code': int(code) if code else None, 'reason': reason})
target.write_text(json.dumps(data, indent=2) + '\n')
PHASE_STATUS
}

run_recorded() {
  local phase_exit
  if run_phase "$@"; then
    phase_exit=0
    record_phase "$1" passed 0 || exit 1
  else
    phase_exit=$?
    verification_failed=1
    if [[ "${phase_not_started:-0}" == 1 ]]; then
      record_phase "$1" not_run "$phase_exit" 'Script work budget exhausted.' || exit 1
    else
      record_phase "$1" failed "$phase_exit" || exit 1
    fi
  fi
  return "$phase_exit"
}

skip_phase() {
  verification_failed=1
  record_phase "$1" not_run '' "$2" || exit 1
}

record_final_status() {
  python3 - "$evidence/phase-status.json" "$1" <<'FINAL_STATUS'
import json, pathlib, sys
path = pathlib.Path(sys.argv[1])
data = json.loads(path.read_text()) if path.exists() else {'schema': 1, 'phases': []}
data['overall_exit_code'] = int(sys.argv[2])
data['scope'] = 'Shared checks and unsigned compilation only; not native XCTest or release acceptance.'
path.write_text(json.dumps(data, indent=2) + '\n')
FINAL_STATUS
}

finish_verification() {
  # This is the check aggregate only. EXIT capture records the effective final
  # process status after archiving, including early bootstrap/budget failures.
  if ((verification_failed != 0)); then
    echo "Unsigned checks retain unresolved failures; inspect phase-status.json." >&2
    exit 1
  fi
  echo "Unsigned checks passed; native XCTest, device behavior and release remain unverified."
}

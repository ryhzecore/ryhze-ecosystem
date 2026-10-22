"""Fresh native reimport fixture; preserve every attempt and its stage roots."""
from output_paths import evidence_path
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import uuid

p = argparse.ArgumentParser()
p.add_argument('--test', required=True)
p.add_argument('--worker', required=True)
p.add_argument('--root', required=True)
a = p.parse_args()
root = evidence_path(Path(a.root))
root.mkdir(parents=True, exist_ok=True)
run_root = root / str(uuid.uuid4())
command = [a.test, '--native', a.worker, str(run_root)]
# CTest bounds this test at 180 s (CMakeLists.txt). The Debug binary was observed above 120 s
# on battery power (2026-10-13, whole-suite slowdown of about 3x), so the inner bound sits just
# under the CTest bound instead of failing first; durations stay visible in ctest.log.
run = subprocess.run(command, capture_output=True, text=True, errors='replace', timeout=170)
record = dict(command=command, exit_code=run.returncode, stdout=run.stdout, stderr=run.stderr,
              binaries={name: hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in (a.test, a.worker)})
run_root.mkdir(exist_ok=True)
(run_root / 'command.json').write_text(json.dumps(record, indent=2))
print(json.dumps(dict(root=str(run_root), exit_code=run.returncode)))
print(run.stdout, end='')
print(run.stderr, end='')
raise SystemExit(run.returncode)

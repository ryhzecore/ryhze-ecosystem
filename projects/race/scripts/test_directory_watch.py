"""Retained native directory-watch fixture and command receipt."""
from output_paths import evidence_path
from evidence_manifest import host_path
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
host_path(root).mkdir(parents=True, exist_ok=True)
fixture = root / str(uuid.uuid4())
command = [a.test, str(fixture), a.worker]
record = dict(command=command,
              binaries={path: hashlib.sha256(host_path(Path(path).resolve()).read_bytes()).hexdigest()
                        for path in (a.test, a.worker)})


def save_record():
    host_path(fixture).mkdir(exist_ok=True)
    host_path(fixture / 'command.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    print(json.dumps(dict(root=str(fixture), exit_code=record.get('exit_code'),
                         timeout_seconds=record.get('timeout_seconds'))))
    print(record['stdout'], end='')
    print(record['stderr'], end='')


try:
    run = subprocess.run(command, capture_output=True, text=True, errors='replace', timeout=120)
except subprocess.TimeoutExpired as failure:
    readable = lambda value: value.decode('utf-8', errors='replace') if isinstance(value, bytes) else (value or '')
    record.update(timeout_seconds=120, stdout=readable(failure.stdout), stderr=readable(failure.stderr))
    save_record()
    raise
record.update(exit_code=run.returncode, stdout=run.stdout, stderr=run.stderr)
save_record()
raise SystemExit(run.returncode)

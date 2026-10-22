"""Run the real Qt editor's bounded layout/input checks and retain their outputs."""
from output_paths import evidence_directory
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--output',type=Path)
a=p.parse_args()
build=a.build.resolve();out=evidence_directory('editor-ui',a.output)
command=[str(build/'race_editor.exe'),'--ui-smoke',str(out/'editor')]
start=time.perf_counter()
try:
    result=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
    process=dict(command=command,exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr,seconds=time.perf_counter()-start)
except subprocess.TimeoutExpired as error:
    (out/'process.json').write_text(json.dumps(dict(command=command,timeout=60,stdout=str(error.stdout),stderr=str(error.stderr)),indent=2))
    raise
(out/'process.json').write_text(json.dumps(process,indent=2),encoding='utf-8')
assert result.returncode==0,process
receipt=json.loads((out/'editor/ui-receipt.json').read_text(encoding='utf-8-sig'))
assert receipt['passed'] and receipt['checks'] and all(c['passed'] for c in receipt['checks']),receipt
assert receipt['hardware'] and receipt['debug_layer'] and not receipt['debug_errors'] and not receipt['debug_warnings'],receipt
sha=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
record=dict(passed=True,checks=len(receipt['checks']),receipt=receipt,executable_sha256=sha(build/'race_editor.exe'),
            artifacts=[dict(path=str(f.relative_to(out)),sha256=sha(f)) for f in sorted(out.rglob('*')) if f.is_file()],
            scope='Actual Qt layout and event-handler checks with D3D12; synthetic input; layout-only captures omit native viewport. Physical window inspection is recorded separately.')
(out/'verification.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
print(json.dumps(dict(passed=True,checks=len(receipt['checks']),receipt=str(out/'verification.json'))))

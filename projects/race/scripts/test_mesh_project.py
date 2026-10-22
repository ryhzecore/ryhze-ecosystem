"""Independent pinned project-index and native tool acceptance; retain failures."""
from output_paths import evidence_path
from evidence_manifest import host_path
from test_content_project import binary_index, parse_binary_index
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import struct
import subprocess
import time
import uuid

p=argparse.ArgumentParser()
p.add_argument('--tool',required=True)
p.add_argument('--mesh-tool',required=True)
p.add_argument('--root',required=True)
a=p.parse_args()
root=evidence_path(Path(a.root))/str(uuid.uuid4());host_path(root).mkdir(parents=True)
source=root/'source';output=root/'output';host_path(source).mkdir();host_path(output).mkdir()
small=b'v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n'
changed=b'v 0 0 0\nv 2 0 0\nv 0 3 0\nf 1 2 3\n'
ids=['000000000000004d0000000000000001','000000000000004d0000000000000002']
commands=[];checks=[];measurements={};active=None
def sha(blob):return hashlib.sha256(blob).hexdigest()
def snapshot(path):return {f.name:sha(host_path(f).read_bytes()) for f in host_path(path).glob('*.rasset')}
def invoke(*args,success=True):
    command=list(map(str,args));begin=time.perf_counter_ns()
    try:run=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
    except subprocess.TimeoutExpired as failure:
        readable=lambda value:value.decode('utf-8',errors='replace') if isinstance(value,bytes) else (value or '')
        commands.append(dict(command=command,timeout_seconds=30,stdout=readable(failure.stdout),stderr=readable(failure.stderr),wall_ms=(time.perf_counter_ns()-begin)/1e6));raise
    commands.append(dict(command=command,exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr,wall_ms=(time.perf_counter_ns()-begin)/1e6))
    assert (run.returncode==0)==success,commands[-1]
    return json.loads(run.stdout) if success else None
def cook(leaf):return invoke(a.mesh_tool,'cook',output,leaf,output)
def project(leaf,identity,stage,success=True):return invoke(a.tool,'--project',source,leaf,identity,source,output,root/stage,success=success)

try:
    host_path(source/'source.obj').write_bytes(small)
    refs=[]
    for n,identity in enumerate(ids):
        name=f'recipe-{n}.json';invoke(a.tool,'--create-recipe',source,name,identity,'source.obj')
        refs.append(dict(asset=identity,kind='mesh',recipe=name,revision=sha(host_path(source/name).read_bytes())))
    created=invoke(a.tool,'--create-project',source,'project.raceproject','recipe-1.json','recipe-0.json')
    raw=host_path(source/'project.raceproject').read_bytes();expected=dict(schema=2,recipes=refs)
    assert raw==binary_index(refs) and parse_binary_index(raw)==expected and created['index_revision']==sha(raw) and created['entries']==2
    invoke(a.tool,'--create-project',source,'project.raceproject','recipe-0.json','recipe-1.json')
    invoke(a.tool,'--create-project',source,'project.raceproject','recipe-0.json',success=False)
    assert host_path(source/'project.raceproject').read_bytes()==raw
    empty=invoke(a.tool,'--create-project',source,'empty.raceproject');assert empty['entries']==0
    project('empty.raceproject',ids[0],'empty-stage',False);assert not host_path(root/'empty-stage').exists()
    before=snapshot(output)
    invoke(a.tool,'--create-project',source,'duplicate.raceproject','recipe-0.json','recipe-0.json',success=False)
    assert not host_path(source/'duplicate.raceproject').exists() and snapshot(output)==before
    checks.append('independent sorted canonical index; immutable creation/readback/collision; empty and duplicate indexes')

    products=[]
    for n,identity in enumerate(ids):
        direct=invoke(a.tool,source,'source.obj',output,root/f'direct-{n}',identity)
        admitted=project('project.raceproject',identity,f'project-{n}')
        assert admitted['leaf']==direct['leaf'] and admitted['recipe_revision']==refs[n]['revision'] and admitted['index_revision']==sha(raw) and admitted['asset']==identity
        imported=host_path(output/admitted['leaf']).read_bytes();cooked=cook(admitted['leaf']);cooked_bytes=host_path(output/cooked['leaf']).read_bytes()
        recipe=invoke(a.tool,'--recipe',source,refs[n]['recipe'],source,output,root/f'recipe-{n}')
        assert host_path(output/recipe['leaf']).read_bytes()==imported and host_path(output/cook(recipe['leaf'])['leaf']).read_bytes()==cooked_bytes
        products.append((admitted,cooked))
    # Canonical binary indexes have one ordering; a valid alias changes raw
    # identity while retaining the exact recipe bytes, source and cooked product.
    host_path(source/'alternate-recipe.json').write_bytes(host_path(source/refs[0]['recipe']).read_bytes())
    alternate=[{**refs[0], 'recipe':'alternate-recipe.json'},refs[1]]
    alternate_raw=binary_index(alternate)
    host_path(source/'aliased.raceproject').write_bytes(alternate_raw)
    admitted=project('aliased.raceproject',ids[0],'aliased-stage')
    assert admitted['leaf']==products[0][0]['leaf'] and admitted['index_revision']==sha(alternate_raw)!=sha(raw)
    checks.append('stable-ID selection with shared OBJ source; exact legacy mesh/recipe/project product parity; canonical binary raw index identity')

    before=snapshot(output)
    def seal(blob):return bytes(blob[:-32])+hashlib.sha256(blob[:-32]).digest()
    malformed=[b'{}',raw+b' '*8192]
    for offset,data in [(8,struct.pack('<I',2)),(32+152,raw[32:48]),
                        (32+152+52,raw[32+52:32+152]),(32+152+16,bytes(32))]:
        changed_raw=bytearray(raw);changed_raw[offset:offset+len(data)]=data;malformed.append(seal(changed_raw))
    for field,value in [('recipe','missing.json'),('asset','000000000000004d0000000000000003'),('kind','texture')]:
        value_refs=[dict(ref) for ref in refs];value_refs[1][field]=value
        malformed.append(binary_index(value_refs))
    for schema in (1,2):
        value_refs=[{key:value for key,value in ref.items() if schema==2 or key!='kind'} for ref in refs]
        payload=json.dumps(dict(schema=schema,recipes=value_refs)).encode()
        malformed.append(payload)
        host_path(source/'rejected-index.json').write_bytes(payload)
        project('rejected-index.json',ids[0],f'json-extension-{schema}',False)
        assert not host_path(root/f'json-extension-{schema}').exists() and snapshot(output)==before
    host_path(source/'binary-index.json').write_bytes(raw)
    project('binary-index.json',ids[0],'binary-wrong-extension',False)
    invoke(a.tool,'--create-project',source,'disallowed.json','recipe-0.json',success=False)
    assert not host_path(root/'binary-wrong-extension').exists() and not host_path(source/'disallowed.json').exists()
    for n,blob in enumerate(malformed):
        host_path(source/'bad.raceproject').write_bytes(blob);project('bad.raceproject',ids[0],f'bad-{n}',False)
        assert not host_path(root/f'bad-{n}').exists() and snapshot(output)==before
    for label,leaf in [('json-payload','bad.raceproject'),('json-extension','binary-index.json')]:
        invoke(a.tool,'--watch-project',source,leaf,ids[0],source,output,root/label,100,1,success=False)
        assert not host_path(root/label).exists() and snapshot(output)==before
    project('project.raceproject','000000000000004d0000000000000099','missing-id',False)
    assert not host_path(root/'missing-id').exists()
    other=host_path(source/'recipe-1.json').read_bytes();host_path(source/'recipe-1.json').write_bytes(other+b' ')
    project('project.raceproject',ids[0],'unselected-edit',False)
    assert not host_path(root/'unselected-edit').exists() and snapshot(output)==before
    host_path(source/'recipe-1.json').write_bytes(b'{"invalid":true}');project('project.raceproject',ids[0],'unselected-invalid',False)
    assert not host_path(root/'unselected-invalid').exists() and snapshot(output)==before
    invoke(a.tool,'--watch-project',source,'project.raceproject',ids[0],source,output,root/'invalid-watch',100,1,success=False)
    assert not host_path(root/'invalid-watch').exists() and snapshot(output)==before
    host_path(source/'recipe-1.json').write_bytes(other)
    invoke(a.tool,'--project',source,'project.raceproject',ids[0],source,output,root/'mixed',ids[1],success=False)
    assert not host_path(root/'mixed').exists() and host_path(source/'project.raceproject').read_bytes()==raw
    checks.append('whole closure rejects malformed/missing/mismatched/edited unselected recipes and unknown IDs before staging; prior products preserved')

    # File-backed streams avoid a pipe-drain dependency in the failure path.
    # Poll complete bounded lines; retain output and wait for actual termination.
    outlog=root/'watch.stdout';errlog=root/'watch.stderr'
    command=list(map(str,[a.tool,'--watch-project',source,'project.raceproject',ids[0],source,output,root/'watch-stages',2500,8,'--coalesce']))
    with host_path(outlog).open('wb') as stdout,host_path(errlog).open('wb') as stderr:
        active=subprocess.Popen(command,stdout=stdout,stderr=stderr)
    recipe_raw=host_path(source/'recipe-0.json').read_bytes();edited=False;observed=False;records=[]
    deadline=time.monotonic()+12
    while time.monotonic()<deadline:
        log=host_path(outlog).read_bytes();assert len(log)<=128*1024
        lines=log.splitlines(keepends=True);complete=[line for line in lines if line.endswith(b'\n')]
        assert all(len(line)<=1024 for line in complete)
        records=[json.loads(line) for line in complete];assert len(records)<=128
        for item in records:
            assert item['index_revision']==sha(raw) and item['recipe_revision']==refs[0]['revision'] and item['asset']==ids[0]
        if any(item['outcome']==3 for item in records) and not edited:
            wrong=json.loads(recipe_raw);wrong['asset']=ids[1];wrong['target']=99
            host_path(source/'recipe-0.json').write_text(json.dumps(wrong),encoding='utf-8');host_path(source/'project.raceproject').write_bytes(b'{}')
            host_path(source/'source.tmp').write_bytes(changed);host_path(source/'source.tmp').replace(host_path(source/'source.obj'));edited=True
        if edited and any(item['outcome']==3 and item['source']==sha(changed) for item in records):observed=True
        if active.poll() is not None:break
        time.sleep(.005)
    code=active.wait(timeout=70);commands.append(dict(command=command,exit_code=code,stdout=host_path(outlog).read_text(),stderr=host_path(errlog).read_text()));active=None
    final_log=host_path(outlog).read_bytes();final_lines=final_log.splitlines(keepends=True)
    assert len(final_log)<=128*1024 and len(final_lines)<=128 and all(len(line)<=1024 and line.endswith(b'\n') for line in final_lines)
    records=[json.loads(line) for line in final_lines]
    assert all(item['index_revision']==sha(raw) and item['recipe_revision']==refs[0]['revision'] and item['asset']==ids[0] for item in records)
    assert code==0 and observed and records[-1]['phase']==2 and not records[-1]['worker_retained']
    expected_changed=invoke(a.tool,source,'source.obj',output,root/'changed-direct',ids[0]);cooked=cook(expected_changed['leaf'])
    assert any(item['current_revision']==cooked['revision'] for item in records)
    worst=dict(records[-1])
    for key in ('events','observed','desired','attempts','reads','read_failures','superseded','unchanged','retained_bytes','pending_source_bytes'):worst[key]=2**64-1
    for key in ('authored_sha256','recipe_revision','index_revision','source','current_revision'):worst[key]='f'*64
    for key in ('phase','outcome','last_error','terminal_error'):worst[key]=255
    worst['worker_retained']=worst['receipt_limit']=False;worst['elapsed_ms']=worst['max_poll_ms']=9223372036854.7754
    measurements['worst_receipt_bytes_with_rounding_margin']=len(json.dumps(worst,separators=(',',':')))+3
    assert measurements['worst_receipt_bytes_with_rounding_margin']<=1024
    project('project.raceproject',ids[0],'edited-index',False);assert not host_path(root/'edited-index').exists()
    host_path(source/'project.raceproject').write_bytes(raw);project('project.raceproject',ids[0],'edited-pin',False);assert not host_path(root/'edited-pin').exists()
    host_path(source/'recipe-0.json').write_bytes(recipe_raw)
    checks.append('watched invocation retains admitted index/recipe despite edits; exact changed-source cook; next invocation rejects stale pins; bounded receipts')

    host_path(source/'source.obj').write_bytes(small);times={'recipe':[],'project':[]}
    for trial in range(11):
        for mode in (('recipe','project') if trial%2==0 else ('project','recipe')):
            stage=f'measure-{trial}-{mode}'
            item=project('project.raceproject',ids[0],stage) if mode=='project' else invoke(a.tool,'--recipe',source,'recipe-0.json',source,output,root/stage)
            assert item['leaf']==products[0][0]['leaf']
            if trial>=2:times[mode].append(commands[-1]['wall_ms'])
    measurements['paired_invocations']=dict(entries=2,warmups=2,trials=9,samples_ms=times,median_ms={k:statistics.median(v) for k,v in times.items()})
    checks.append('paired recipe versus two-entry project admission cost; identical native output')
finally:
    try:
        if active is not None:
            try:active.wait(timeout=70)
            finally:commands.append(dict(command=command,exit_code=active.poll(),pid=active.pid,cleanup_pending=active.poll() is None,stdout=host_path(outlog).read_text(),stderr=host_path(errlog).read_text(),fixture_failed=True))
    finally:
        receipt=dict(checks=checks,measurements=measurements,commands=commands,binaries={name:sha(host_path(Path(name).resolve()).read_bytes()) for name in (a.tool,a.mesh_tool)})
        host_path(root/'receipt.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
        print(json.dumps(dict(root=str(root),checks=len(checks),measurements=measurements)))

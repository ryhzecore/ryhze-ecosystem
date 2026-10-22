"""Independent recipe JSON/profile and native invocation acceptance. Keep failures."""
from output_paths import evidence_path
from evidence_manifest import host_path
import argparse
import hashlib
import json
from pathlib import Path
import statistics
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
identity='000000000000004d0000000000000001'
commands=[];checks=[];measurements={};active=None
def sha(b):return hashlib.sha256(b).hexdigest()
def snapshot(path):return {f.name:sha(host_path(f).read_bytes()) for f in host_path(path).glob('*.rasset')}
def invoke(*args,success=True):
    command=list(map(str,args));begin=time.perf_counter_ns()
    try:
        run=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
    except subprocess.TimeoutExpired as failure:
        readable=lambda value:value.decode('utf-8',errors='replace') if isinstance(value,bytes) else (value or '')
        commands.append(dict(command=command,timeout_seconds=30,stdout=readable(failure.stdout),stderr=readable(failure.stderr),wall_ms=(time.perf_counter_ns()-begin)/1e6))
        raise
    elapsed=(time.perf_counter_ns()-begin)/1e6
    commands.append(dict(command=command,exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr,wall_ms=elapsed))
    assert (run.returncode==0)==success,commands[-1]
    return json.loads(run.stdout) if success else None
def cook(leaf):return invoke(a.mesh_tool,'cook',output,leaf,output)

try:
    host_path(source/'source.obj').write_bytes(small)
    created=invoke(a.tool,'--create-recipe',source,'recipe.json',identity,'source.obj')
    raw=host_path(source/'recipe.json').read_bytes()
    expected=dict(schema=1,asset=identity,source='source.obj',mesh_container=1,mesh_schema=1,target=1,
        settings=sha(b'race-obj-triangles-settings-1:metres;right-handed;y-up;ccw;flat-normals;binary64'),
        importer=sha(b'race-first-party-obj-triangles-importer-1'),cooker=sha(b'race-native-mesh-portable-cpu-cooker-1'),dependencies=sha(b''))
    assert raw==(json.dumps(expected,separators=(',',':'))+'\n').encode() and created['recipe_revision']==sha(raw)
    invoke(a.tool,'--create-recipe',source,'recipe.json',identity,'source.obj')
    invoke(a.tool,'--create-recipe',source,'recipe.json',identity,'renamed.obj',success=False)
    assert host_path(source/'recipe.json').read_bytes()==raw
    direct=invoke(a.tool,source,'source.obj',output,root/'direct-stage',identity)
    admitted=invoke(a.tool,'--recipe',source,'recipe.json',source,output,root/'recipe-stage')
    assert direct['leaf']==admitted['leaf'] and admitted['recipe_revision']==sha(raw) and admitted['asset']==identity
    old_imported=host_path(output/direct['leaf']).read_bytes();cooked=cook(direct['leaf']);old_cooked=host_path(output/cooked['leaf']).read_bytes()
    # Reordering changes the invocation identity, never the mesh's profile stamp.
    reordered=json.dumps(dict(reversed(list(expected.items()))),indent=2).encode()
    host_path(source/'reordered.json').write_bytes(reordered)
    again=invoke(a.tool,'--recipe',source,'reordered.json',source,output,root/'reordered-stage')
    assert again['recipe_revision']==sha(reordered)!=sha(raw) and again['leaf']==direct['leaf']
    checks.append('independent canonical/profile vector; immutable creation/readback; direct recipe parity and exact raw identity')

    host_path(source/'renamed.obj').write_bytes(small)
    invoke(a.tool,'--create-recipe',source,'renamed.json',identity,'renamed.obj')
    renamed=invoke(a.tool,'--recipe',source,'renamed.json',source,output,root/'renamed-stage')
    assert renamed['leaf']==direct['leaf'] and cook(renamed['leaf'])['leaf']==cooked['leaf']
    assert host_path(output/renamed['leaf']).read_bytes()==old_imported and host_path(output/cooked['leaf']).read_bytes()==old_cooked
    host_path(source/'renamed.obj').write_bytes(changed)
    different=invoke(a.tool,'--recipe',source,'renamed.json',source,output,root/'changed-stage')
    assert different['asset']==identity and different['source']==sha(changed) and different['leaf']!=direct['leaf']
    changed_cooked=cook(different['leaf'])
    checks.append('source rename preserves ID and exact imported/cooked bytes; changed bytes replace revision under same ID')

    before=snapshot(output)
    malformed=[]
    for key in ('schema','mesh_container','mesh_schema','target','settings','importer','cooker','dependencies'):
        obj=dict(expected);obj[key]=2 if isinstance(obj[key],int) else '0'*64;malformed.append(json.dumps(obj).encode())
    malformed += [raw[:-2]+b',"source":"source.obj"}\n',raw+b' '*4096,b'{}',raw.replace(b'"target":1,',b''),
                  raw.replace(b'"source.obj"',b'"../source.obj"')]
    for n,blob in enumerate(malformed):
        host_path(source/'bad.json').write_bytes(blob);stage=root/f'bad-stage-{n}'
        invoke(a.tool,'--recipe',source,'bad.json',source,output,stage,success=False)
        assert not host_path(stage).exists() and snapshot(output)==before
    invoke(a.tool,'--watch-recipe',source,'bad.json',source,output,root/'bad-watch-stage',100,1,success=False)
    assert not host_path(root/'bad-watch-stage').exists()
    invoke(a.tool,'--recipe',source,'recipe.json',source,output,root/'mixed-stage',identity,success=False)
    assert not host_path(root/'mixed-stage').exists() and host_path(source/'source.obj').read_bytes()==small and host_path(source/'recipe.json').read_bytes()==raw
    checks.append('unsupported/malformed/mixed recipe input rejected before staging; prior source/recipe/products preserved')

    # Authoring adopts exactly one recipe snapshot. Subsequent recipe edits do
    # not redirect its source/identity/profile during this invocation.
    command=list(map(str,[a.tool,'--watch-recipe',source,'recipe.json',source,output,root/'watch-stages',2500,8,'--coalesce']))
    active=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    records=[];lines=[];edited=False;observed=False
    for _ in range(129):
        line=active.stdout.readline(1025)
        if not line:break
        assert len(line)<=1024 and line.endswith(b'\n')
        lines.append(line.decode('utf-8'));item=json.loads(line);records.append(item)
        assert item['recipe_revision']==sha(raw) and item['asset']==identity
        if item['outcome']==3 and not edited:
            recipe=dict(expected);recipe['asset']='000000000000004d0000000000000002';recipe['source']='renamed.obj';recipe['target']=99
            host_path(source/'recipe.json').write_text(json.dumps(recipe),encoding='utf-8')
            temporary=source/'source.tmp';host_path(temporary).write_bytes(changed);host_path(temporary).replace(host_path(source/'source.obj'));edited=True
        elif edited and item['outcome']==3 and item['source']==sha(changed):
            assert item['current_revision']==changed_cooked['revision'];observed=True
    code=active.wait(timeout=70);stderr=active.stderr.read().decode('utf-8',errors='replace')
    commands.append(dict(command=command,exit_code=code,stdout=''.join(lines),stderr=stderr));active=None
    assert code==0 and observed and records[-1]['phase']==2 and not records[-1]['worker_retained']
    assert len(records)<=128
    worst=dict(records[-1])
    for key in ('events','observed','desired','reads','superseded','unchanged'):worst[key]=2**64-1
    for key in ('attempts','read_failures','retained_bytes','pending_source_bytes'):worst[key]=2**64-1
    worst['elapsed_ms']=worst['max_poll_ms']=9223372036854.7754
    worst['last_error']=worst['terminal_error']=255
    # snprintf uses four decimal places; allow an extra decimal rounding byte
    # for each duration over Python's equivalent compact fixed-width number.
    assert len(json.dumps(worst,separators=(',',':')))+3<=1024
    failed_before=snapshot(output)
    invoke(a.tool,'--recipe',source,'recipe.json',source,output,root/'edited-recipe-stage',success=False)
    assert not host_path(root/'edited-recipe-stage').exists() and snapshot(output)==failed_before
    checks.append('recipe snapshot survives mid-watch authoring edits; original ID/profile/digest retained; receipt width remains bounded')

    # Same small source/worker/output, paired alternating order; two warmups and
    # nine measured trials per mode. No automatic timing threshold is imposed.
    host_path(source/'recipe.json').write_bytes(raw);host_path(source/'source.obj').write_bytes(small)
    times={'direct':[],'recipe':[]}
    for trial in range(11):
        for mode in (('direct','recipe') if trial%2==0 else ('recipe','direct')):
            stage=root/f'measure-{trial}-{mode}'
            if mode=='direct':item=invoke(a.tool,source,'source.obj',output,stage,identity)
            else:item=invoke(a.tool,'--recipe',source,'recipe.json',source,output,stage)
            assert item['leaf']==direct['leaf']
            if trial>=2:times[mode].append(commands[-1]['wall_ms'])
    measurements['paired_invocations']=dict(warmups=2,trials=9,samples_ms=times,median_ms={k:statistics.median(v) for k,v in times.items()})
    checks.append('paired same-worker recipe admission cost; identical native output')
finally:
    if active is not None:
        remainder,diagnostic=active.communicate(timeout=70)
        commands.append(dict(command=command,exit_code=active.returncode,stdout=''.join(lines)+remainder.decode('utf-8',errors='replace'),stderr=diagnostic.decode('utf-8',errors='replace'),fixture_failed=True))
    receipt=dict(checks=checks,measurements=measurements,commands=commands,binaries={p:sha(host_path(Path(p).resolve()).read_bytes()) for p in (a.tool,a.mesh_tool)})
    host_path(root/'receipt.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    print(json.dumps(dict(root=str(root),checks=len(checks),measurements=measurements)))

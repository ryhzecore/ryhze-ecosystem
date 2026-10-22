"""Independent immutable checkpoint/restart acceptance using real native tools."""
from output_paths import evidence_path
from test_content_project import binary_index, parse_binary_index
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import uuid


def host_io_path(path):
    """Extend Windows fixture I/O paths only; keep ordinary paths in tool argv."""
    path=Path(path)
    if os.name!='nt':return path
    if not path.is_absolute():raise ValueError('Windows fixture I/O requires an absolute path')
    value=str(path)
    if value.startswith('\\\\?\\'):return path
    if value.startswith('\\\\'):return Path('\\\\?\\UNC\\'+value[2:])
    return Path('\\\\?\\'+value)


p=argparse.ArgumentParser()
p.add_argument('--tool',required=True)
p.add_argument('--mesh-tool',required=True)
p.add_argument('--root',required=True)
a=p.parse_args()
root=evidence_path(Path(a.root))/('\u03bb-'+str(uuid.uuid4()));host_io_path(root).mkdir(parents=True)
source=root/'source';products=root/'products';saved=root/'checkpoints';recipes=root/'recipes'
for directory in (source,products,saved,recipes):host_io_path(directory).mkdir()
small=b'v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n'
changed=b'v 0 0 0\nv 2 0 0\nv 0 3 0\nf 1 2 3\n'
invalid=b'this is an invalid newer source\n'
identity='000000000000004d0000000000000001'
other='000000000000004d0000000000000002'
commands=[];checks=[];measurements={};active=None
def sha(blob):return hashlib.sha256(blob).hexdigest()
def snapshot(directory):return {f.name:sha(f.read_bytes()) for f in host_io_path(directory).iterdir() if f.is_file()}
def invoke(*args,success=True):
    command=list(map(str,args));begin=time.perf_counter_ns()
    try:run=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
    except subprocess.TimeoutExpired as failure:
        readable=lambda value:value.decode('utf-8',errors='replace') if isinstance(value,bytes) else (value or '')
        commands.append(dict(command=command,timeout_seconds=30,stdout=readable(failure.stdout),stderr=readable(failure.stderr)));raise
    commands.append(dict(command=command,exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr,wall_ms=(time.perf_counter_ns()-begin)/1e6))
    assert (run.returncode==0)==success,commands[-1]
    return json.loads(run.stdout) if success else None
def lines(path):
    raw=host_io_path(path).read_bytes();assert len(raw)<=128*1024
    complete=[line for line in raw.splitlines(keepends=True) if line.endswith(b'\n')]
    assert len(complete)<=128 and all(len(line)<=1024 for line in complete)
    return [json.loads(line) for line in complete]
def watch(label,*options,success=True,duration=1500,callback=None):
    global active,command,outlog,errlog
    command=list(map(str,[a.tool,'--watch-project',recipes,'project.raceproject',identity,source,products,root/label,duration,8,*options]))
    outlog=root/(label+'.stdout');errlog=root/(label+'.stderr')
    with host_io_path(outlog).open('wb') as stdout,host_io_path(errlog).open('wb') as stderr:active=subprocess.Popen(command,stdout=stdout,stderr=stderr)
    deadline=time.monotonic()+12
    while time.monotonic()<deadline and active.poll() is None:
        records=lines(outlog)
        if callback:callback(records)
        time.sleep(.005)
    code=active.wait(timeout=70)
    commands.append(dict(command=command,exit_code=code,stdout=host_io_path(outlog).read_text(),stderr=host_io_path(errlog).read_text()));active=None
    records=lines(outlog);raw=host_io_path(outlog).read_bytes()
    assert not raw or raw.endswith(b'\n')
    assert (code==0)==success,commands[-1]
    statuses=[item for item in records if 'phase' in item]
    if statuses:assert statuses[-1]['phase']==2 and not statuses[-1]['worker_retained']
    return records
def load(leaf,success=True):return invoke(a.tool,'--load-checkpoint',recipes,'project.raceproject',products,saved,leaf,success=success)
def manifest(value):
    blob=(json.dumps(value,separators=(',',':'))+'\n').encode();leaf=sha(blob)+'.json';(host_io_path(saved/leaf)).write_bytes(blob);return leaf
def reject(leaf,label):
    prior=snapshot(products);checkpoints=snapshot(saved)
    load(leaf,False)
    watch(label,'--resume-checkpoint',saved,leaf,success=False,duration=100)
    assert not (host_io_path(root/label)).exists() and snapshot(products)==prior and snapshot(saved)==checkpoints
def replace_source(blob):
    pending=root/'pending.obj';host_io_path(pending).write_bytes(blob);host_io_path(pending).replace(host_io_path(source/'source.obj'))

try:
    (host_io_path(source/'source.obj')).write_bytes(small)
    for n,asset in enumerate((identity,other)):invoke(a.tool,'--create-recipe',recipes,f'recipe-{n}.json',asset,'source.obj')
    invoke(a.tool,'--create-project',recipes,'project.raceproject','recipe-0.json','recipe-1.json')
    index_raw=(host_io_path(recipes/'project.raceproject')).read_bytes();recipe_raw=(host_io_path(recipes/'recipe-0.json')).read_bytes()
    entries=parse_binary_index(index_raw)['recipes']
    assert index_raw==binary_index(entries)
    (host_io_path(recipes/'alternate-recipe.json')).write_bytes((host_io_path(recipes/entries[1]['recipe'])).read_bytes())
    changed_index=binary_index([entries[0],{**entries[1],'recipe':'alternate-recipe.json'}])
    assert changed_index!=index_raw

    direct=invoke(a.tool,source,'source.obj',products,root/'direct',identity)
    cooked=invoke(a.mesh_tool,'cook',products,direct['leaf'],products)
    cooked_bytes=(host_io_path(products/cooked['leaf'])).read_bytes()
    edited=False
    def reject_newer(records):
        global edited
        if not edited and any(item.get('outcome')==3 for item in records):
            (host_io_path(recipes/'project.raceproject')).write_bytes(changed_index)
            (host_io_path(recipes/'recipe-0.json')).write_bytes(recipe_raw+b' ')
            replace_source(invalid);edited=True
    records=watch('produce','--checkpoint',saved,callback=reject_newer)
    statuses=[item for item in records if 'phase' in item];checkpoint=records[-1]
    assert edited and any(item['outcome']==4 and item['source']==sha(invalid) for item in statuses),records
    assert statuses[-1]['source']==sha(invalid) and statuses[-1]['current_revision']==cooked['revision']
    expected=dict(schema=1,asset=identity,index_revision=sha(index_raw),recipe_revision=sha(recipe_raw),source=sha(small),cooked_revision=cooked['revision'])
    raw=(json.dumps(expected,separators=(',',':'))+'\n').encode();leaf=sha(raw)+'.json'
    assert checkpoint['leaf']==leaf and checkpoint['source']==sha(small) and (host_io_path(saved/leaf)).read_bytes()==raw
    assert (host_io_path(products/cooked['leaf'])).read_bytes()==cooked_bytes
    load(leaf,False);(host_io_path(recipes/'project.raceproject')).write_bytes(index_raw);load(leaf,False)
    (host_io_path(recipes/'recipe-0.json')).write_bytes(recipe_raw)
    checks.append('actual current A plus rejected newer B captures A provenance; independent canonical manifest/checksum; exact cooked product')
    checks.append('capture pins admitted project/recipe snapshots despite later file edits; subsequent loads reject changed closure')
    prior_products=snapshot(products);prior_checkpoints=snapshot(saved)
    (host_io_path(recipes/'binary-index.json')).write_bytes(index_raw)
    invoke(a.tool,'--load-checkpoint',recipes,'binary-index.json',products,saved,leaf,success=False)
    for schema in (1,2):
        refs=[{key:value for key,value in entry.items() if schema==2 or key!='kind'} for entry in entries]
        (host_io_path(recipes/'rejected-json.raceproject')).write_text(json.dumps(dict(schema=schema,recipes=refs)),encoding='ascii')
        invoke(a.tool,'--load-checkpoint',recipes,'rejected-json.raceproject',products,saved,leaf,success=False)
    assert snapshot(products)==prior_products and snapshot(saved)==prior_checkpoints
    checks.append('checkpoint admission rejects JSON payloads and binary indexes with .json extension without output mutation')

    (host_io_path(source/'source.obj')).unlink();loaded=load(leaf)
    assert loaded['cooked_revision']==cooked['revision'] and loaded['source']==sha(small)
    missing=watch('resume-missing','--checkpoint',saved,'--resume-checkpoint',saved,leaf,duration=200)
    assert missing[-1]['leaf']==leaf and any(item.get('outcome')==5 for item in missing)
    assert all(item['attempts']==0 for item in missing if 'attempts' in item)
    assert len(list(host_io_path(saved).glob('*.json')))==1 and (host_io_path(saved/leaf)).read_bytes()==raw
    replace_source(invalid)
    rejected=watch('resume-invalid','--resume-checkpoint',saved,leaf,'--coalesce','--checkpoint',saved,duration=350)
    assert rejected[-1]['leaf']==leaf and any(item.get('outcome')==4 for item in rejected)
    edited=False
    def replace_rejected(records):
        global edited
        if not edited and any(item.get('outcome')==4 for item in records):replace_source(changed);edited=True
    resumed=watch('resume-change','--resume-checkpoint',saved,leaf,'--checkpoint',saved,callback=replace_rejected)
    changed_import=invoke(a.tool,source,'source.obj',products,root/'changed-direct',identity)
    changed_cook=invoke(a.mesh_tool,'cook',products,changed_import['leaf'],products)
    assert edited and any(item.get('outcome')==3 and item['current_revision']==changed_cook['revision'] for item in resumed)
    assert resumed[-1]['cooked_revision']==changed_cook['revision'] and resumed[-1]['source']==sha(changed)
    assert resumed[-1]['leaf']!=leaf and (host_io_path(saved/leaf)).read_bytes()==raw
    assert load(leaf)['cooked_revision']==cooked['revision']
    checks.append('explicit restart without source fallback; missing/rejected input preserves A; valid replacement B coexists with immutable A; repeated capture idempotent')

    # Closing the read end before admission finishes must make receipt delivery fail.
    command=list(map(str,[a.tool,'--load-checkpoint',recipes,'project.raceproject',products,saved,leaf]))
    outlog=None;errlog=root/'closed-stdout.stderr'
    with host_io_path(errlog).open('wb') as stderr:active=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=stderr)
    active.stdout.close();code=active.wait(timeout=70)
    commands.append(dict(command=command,exit_code=code,closed_stdout=True,stderr=host_io_path(errlog).read_text()));active=None
    assert code!=0 and 'receipt output failed' in host_io_path(errlog).read_text()
    checks.append('closed stdout reports explicit checkpoint load delivery failure')

    for field in ('index_revision','recipe_revision','source','cooked_revision','asset'):
        value=dict(expected);value[field]=other if field=='asset' else 'f'*64
        reject(manifest(value),'bad-'+field)
    (host_io_path(saved/('a'*64+'.json'))).write_bytes(raw);reject('a'*64+'.json','bad-name')
    for n,blob in enumerate((b'{}',raw+b' '*1024,raw.replace(b'"schema":1',b'"schema":2'))):
        bad=sha(blob)+'.json';(host_io_path(saved/bad)).write_bytes(blob);reject(bad,'bad-json-'+str(n))
    reject('b'*64+'.json','missing-manifest')
    (host_io_path(products/cooked['leaf'])).unlink();reject(leaf,'missing-product')
    (host_io_path(products/cooked['leaf'])).write_bytes(cooked_bytes[:-1]+bytes([cooked_bytes[-1]^1]));reject(leaf,'corrupt-product')
    (host_io_path(products/cooked['leaf'])).write_bytes(cooked_bytes)
    imported=dict(expected);imported['cooked_revision']=sha((host_io_path(products/direct['leaf'])).read_bytes());reject(manifest(imported),'imported-product')
    (host_io_path(recipes/'recipe-1.json')).write_bytes((host_io_path(recipes/'recipe-1.json')).read_bytes()+b' ');reject(leaf,'unselected-pin')
    other_raw=(host_io_path(recipes/'recipe-1.json')).read_bytes();(host_io_path(recipes/'recipe-1.json')).write_bytes(other_raw[:-1])
    (host_io_path(recipes/'project.raceproject')).write_bytes(changed_index);reject(leaf,'index-raw-pin');(host_io_path(recipes/'project.raceproject')).write_bytes(index_raw)
    # A valid checkpoint for the other project entry must still reject this watch selection.
    second=invoke(a.tool,source,'source.obj',products,root/'second-direct',other)
    second_cook=invoke(a.mesh_tool,'cook',products,second['leaf'],products)
    selected=dict(expected,asset=other,recipe_revision=sha((host_io_path(recipes/'recipe-1.json')).read_bytes()),source=sha(changed),cooked_revision=second_cook['revision'])
    selected_leaf=manifest(selected);assert load(selected_leaf)['asset']==other
    watch('wrong-selection','--resume-checkpoint',saved,selected_leaf,success=False,duration=100);assert not (host_io_path(root/'wrong-selection')).exists()
    checks.append('malformed/hash/pin/selection/closure failures precede staging; missing/imported/corrupt product rejection preserves products and checkpoints')

    preserved=snapshot(saved)
    for n,options in enumerate((('--checkpoint',source),('--checkpoint',saved,'--checkpoint',saved),('--resume-checkpoint',saved),('--resume-checkpoint',saved,leaf,'--resume-checkpoint',saved,leaf),('--unknown',))):
        watch('bad-options-'+str(n),*options,success=False,duration=100);assert not (host_io_path(root/('bad-options-'+str(n)))).exists()
    replace_source(invalid);watch('no-current','--checkpoint',saved,success=False,duration=200)
    assert snapshot(saved)==preserved and (host_io_path(saved/leaf)).read_bytes()==raw
    assert load(leaf)['cooked_revision']==cooked['revision']
    # The saved manifest is loaded first from a clean separate root; publishing
    # the same expected leaf into a conflicting destination must preserve it.
    clean=root/'clean-checkpoint';host_io_path(clean).mkdir()
    clean_file=host_io_path(clean/leaf)
    clean_file.write_bytes(raw)
    assert clean_file.read_bytes()==raw
    conflict=raw+b' ';(host_io_path(saved/leaf)).write_bytes(conflict);prior=snapshot(products)
    collision=watch('collision','--resume-checkpoint',clean,leaf,'--checkpoint',saved,success=False,duration=150)
    assert collision[-1]['phase']==2 and (host_io_path(saved/leaf)).read_bytes()==conflict and snapshot(products)==prior
    (host_io_path(saved/leaf)).write_bytes(raw)
    for mode,options in (('--watch',[source,'source.obj',products,root/'unsupported-watch',identity,100,8]),('--watch-recipe',[recipes,'recipe-0.json',source,products,root/'unsupported-recipe',100,8])):
        invoke(a.tool,mode,*options,'--checkpoint',saved,success=False)
    assert not (host_io_path(root/'unsupported-watch')).exists() and not (host_io_path(root/'unsupported-recipe')).exists()
    measurements['loaded']=loaded;measurements['captures']=[checkpoint,missing[-1],rejected[-1],resumed[-1]]
    measurements['max_record_bytes']=max((len(line)+1 for item in commands for line in item.get('stdout','').splitlines()),default=0)
    assert measurements['max_record_bytes']<=1024
    checks.append('bounded receipt stream; duplicate/incomplete/inside-source options reject before staging; no-current capture failure preserves old checkpoint')
finally:
    try:
        if active is not None:
            try:active.wait(timeout=70)
            finally:commands.append(dict(command=command,exit_code=active.poll(),pid=active.pid,cleanup_pending=active.poll() is None,stdout=host_io_path(outlog).read_text() if outlog else None,stderr=host_io_path(errlog).read_text(),fixture_failed=True))
    finally:
        receipt=dict(checks=checks,measurements=measurements,commands=commands,binaries={name:sha(host_io_path(Path(os.path.abspath(name))).read_bytes()) for name in (a.tool,a.mesh_tool)})
        (host_io_path(root/'receipt.json')).write_text(json.dumps(receipt,indent=2),encoding='utf-8')
        print(json.dumps(dict(root=str(root),checks=len(checks),measurements=measurements)))

#!/usr/bin/env python3
"""Fresh-process deterministic cook, independent digest oracle and explicit Scene bridge."""
import argparse, hashlib, json, pathlib, subprocess, tempfile, time
p=argparse.ArgumentParser();p.add_argument('--tool',required=True);p.add_argument('--headless',required=True);a=p.parse_args()
def invoke(args):
    result=subprocess.run(args,text=True,capture_output=True,timeout=30)
    if result.returncode:
        print(result.stdout, result.stderr, flush=True)
        result.check_returncode()
    return json.loads(result.stdout)
with tempfile.TemporaryDirectory(prefix='race-assets-workflow-') as temp:
    root=pathlib.Path(temp);one=root/'one';two=root/'two';one.mkdir();two.mkdir();paths=[]
    for n in range(1,5):
        hard={1:[2,3],2:[4],3:[4],4:[]}[n]
        text=f'RACEFIXTURE1\nid {77:016x}{n:016x}\nseed {n}\ncount 256\nsettings 0\nenvironment 1\nhard {len(hard)}'+''.join(f' {77:016x}{v:016x}' for v in hard)+'\nsoft 0\n'
        path=root/f'fixture-{n}.txt';path.write_text(text);paths.append(path)
    first=invoke([a.tool,'cook',str(one),*map(str,paths)])
    renamed=root/'renamed';renamed.mkdir()
    moved=[]
    for i,path in enumerate(paths):
        target=renamed/f'new-name-{i}.txt';path.rename(target);moved.append(target)
    second=invoke([a.tool,'cook',str(two),*map(str,reversed(moved))])
    assert first==second
    assert {f.name:f.read_bytes() for f in one.iterdir()}=={f.name:f.read_bytes() for f in two.iterdir()}
    for entry in first['entries']:
        data=(one/(entry['revision']+'.bin')).read_bytes()
        assert len(data)==entry['bytes'] and hashlib.sha256(data).hexdigest()==entry['revision']
    assert invoke([a.tool,'validate',str(one)])==first
    assert invoke([a.tool,'inspect',str(two)])==first
    started=time.perf_counter_ns();result=invoke([a.headless,str(one),f'{77:016x}{1:016x}']);elapsed=time.perf_counter_ns()-started
    assert result['scene_explicit_resolution'] and result['world_isolation'] and result['reads']==result['decodes']==4
    print(json.dumps({'fresh_process_scene':result,'full_process_ns':elapsed,'boundary':'process launch through completion; stage clocks are separate, no performance claim','deterministic_byte_identity':True},sort_keys=True))

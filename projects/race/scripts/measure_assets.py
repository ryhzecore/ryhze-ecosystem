#!/usr/bin/env python3
"""Initial checked Assets fixture baseline; no comparative speedup claim."""
from output_paths import evidence_directory
import argparse,csv,hashlib,json,os,pathlib,random,statistics,subprocess,time
p=argparse.ArgumentParser();p.add_argument('--tool',required=True,type=pathlib.Path);p.add_argument('--headless',required=True,type=pathlib.Path);p.add_argument('--output',type=pathlib.Path);a=p.parse_args();a.output=evidence_directory('measure-assets',a.output)
a.tool=a.tool.resolve();a.headless=a.headless.resolve()
def run(args):
    start=time.perf_counter_ns();r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=30);elapsed=time.perf_counter_ns()-start
    if r.returncode: raise RuntimeError(r.stderr)
    return json.loads(r.stdout),elapsed
cases=[]
for topology in ['leaf','diamond','chain8']:
    for count in [16,4096,65536]:
        name=f'{topology}-{count}';root=a.output/name;root.mkdir();inputs=[];nodes={'leaf':1,'diamond':4,'chain8':8}[topology]
        for i in range(1,nodes+1):
            deps=([i+1] if i<nodes else []) if topology=='chain8' else ({1:[2,3],2:[4],3:[4],4:[]}.get(i,[]) if topology=='diamond' else [])
            path=root/f'input-{i}.txt';path.write_text(f'RACEFIXTURE1 id {91:016x}{i:016x} seed {i} count {count} settings 0 environment 1 hard {len(deps)}'+''.join(f' {91:016x}{d:016x}' for d in deps)+' soft 0\n');inputs.append(path)
        cooked,cook_ns=run([a.tool,'cook',root,*inputs]);(root/'cook.json').write_text(json.dumps({'full_process_ns':cook_ns,'inspection':cooked},indent=2)+'\n')
        cases.append((name,root,nodes,count))
rows=[];rng=random.Random(71923)
for block in range(-2,9):
    order=cases.copy();rng.shuffle(order)
    for position,(name,root,nodes,count) in enumerate(order):
        value,total=run([a.headless,root,f'{91:016x}{1:016x}'])
        assert value['scene_explicit_resolution'] and value['world_isolation'] and value['reads']==value['decodes']==nodes and value['payload_bytes']==nodes*count*8
        row={'case':name,'block':block,'warmup':block<0,'position':position,'full_process_ns':total,**value};rows.append(row)
        with (a.output/'trials.jsonl').open('a') as stream:stream.write(json.dumps(row,sort_keys=True)+'\n')
        print(f'{name} block={block} full_process_ns={total}',flush=True)
with (a.output/'trials.csv').open('w') as stream:
    w=csv.DictWriter(stream,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
def describe(v):
    v=sorted(v)
    def quantile(q):
        p=(len(v)-1)*q;i=int(p);return v[i]+(v[min(i+1,len(v)-1)]-v[i])*(p-i)
    return {'n':len(v),'min':min(v),'median':statistics.median(v),'max':max(v),'mean':statistics.mean(v),'stdev':statistics.stdev(v),'p95':quantile(.95),'p99':quantile(.99)}
metrics=['full_process_ns','read_ns','decode_ns','drain_ns','payload_bytes','temporary_peak_bytes','bounded_metadata_capacity_bytes','rss_bytes']
summary={name:{key:describe([r[key] for r in rows if r['case']==name and not r['warmup']]) for key in metrics} for name,_,_,_ in cases}
receipt={'schema':1,'source':rows[0]['source'],'authored_sha256':rows[0]['authored_sha256'],'artifacts':{str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in [a.tool,a.headless]},'seed':71923,'warmups_per_case':2,'measured_trials_per_case':9,'affinity':sorted(os.sched_getaffinity(0)),'workers':2,'caller':'owner drain, not a CPU scheduler worker','summary':summary,'limitations':['Fresh process lifecycle includes process startup, catalog parse, scheduler/I/O startup, Scene fixture work, shutdown and process exit. Stage clocks are overlapping component sums and cannot be added to reconstruct lifecycle.','OS file cache is not flushed: this is warm-cache fixture loading, not disk cold-start performance.','Shared cloud host; container quota/affinity recorded separately. No cross-machine comparison or optimization adoption.','Nine trials do not establish stable population p95/p99; reported sample quantiles are descriptive only.','Cook process timed once per case as setup, not a repeated cooker performance claim.','Tracked payload, bounded metadata estimate, temporary reservation and process RSS are distinct.']}
(a.output/'summary.json').write_text(json.dumps(receipt,indent=2,sort_keys=True)+'\n')

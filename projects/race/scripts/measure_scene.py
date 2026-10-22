#!/usr/bin/env python3
"""Serial, fresh-process Scene workload capture; never builds or alters sources."""
from __future__ import annotations
from output_paths import evidence_directory
import argparse,datetime,hashlib,json,os,pathlib,random,subprocess,time
FAMILIES=('iteration','structural','transform','loading','template','retained')
SCALES=(256,4096,16384)
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def read(path):
    try:return pathlib.Path(path).read_text().strip()
    except OSError:return None
def environment():
    keys={'model name','vendor_id','cpu family','model','stepping'};cpu={}
    for line in (read('/proc/cpuinfo') or '').splitlines():
        if ':' in line:
            key,value=map(str.strip,line.split(':',1))
            if key in keys and key not in cpu:cpu[key]=value
    return {'cpu':cpu,'affinity':sorted(os.sched_getaffinity(0)),'cpu_max':read('/sys/fs/cgroup/cpu.max'),'memory_max':read('/sys/fs/cgroup/memory.max'),'cpu_stat':read('/sys/fs/cgroup/cpu.stat'),'kernel':os.uname().release,'uid':os.getuid(),'process_status':{line.split(':')[0]:line.split(':',1)[1].strip() for line in (read('/proc/self/status') or '').splitlines() if line.split(':')[0] in {'Uid','Gid','CapEff','CapBnd','NoNewPrivs','Seccomp'}},'note':'Shared KVM host; host contention/frequency not controlled. This runner launches one child at a time.'}
def identify(path):
    r=subprocess.run([str(path),'--identity'],capture_output=True,text=True,check=True,timeout=30)
    identity=json.loads(r.stdout)
    if identity['configuration']!='Release' or identity['sanitizer']!='none':raise ValueError('native measurements require optimized unsanitized Release')
    return {'path':str(path),'bytes':path.stat().st_size,'sha256':digest(path),'identity':identity}
def validate_rows(rows,identity,family,n):
    if len(rows)!=13 or rows[0]['kind']!='identity' or rows[-1]!=dict(rows[-1],kind='complete',correct=True,records=11):raise ValueError('incomplete workload records')
    first=rows[0]
    for key in ('source_commit','authored_input_sha256','configuration','sanitizer'):
        if first[key]!=identity[key]:raise ValueError('embedded identity changed')
    if (first['family'],first['objects'],first['warmups'],first['trials'],first['capacity'])!=(family,n,2,9,2*n):raise ValueError('workload identity differs')
    for i,row in enumerate(rows[1:-1]):
        if row['kind']!='trial' or row['index']!=i or row['warmup']!=(i<2) or not row['correct'] or row['family']!=family or row['objects']!=n:raise ValueError('trial contract failed')
        if row['full_trial_ns']<row['setup_ns']+row['verification_ns']+row['teardown_ns']:raise ValueError('timing boundary impossible')
        if not row['phases'] or any(p['elapsed_ns']<=0 or p['items']<=0 for p in row['phases']):raise ValueError('invalid phase timing')
def variant_order(repetition,case,cases,variants):
    # Stable case identity, not rotated position: every case alternates each block.
    names=list(variants)
    if (repetition+cases.index(case))%2:names.reverse()
    return names
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--reference',type=pathlib.Path,required=True);p.add_argument('--candidate',type=pathlib.Path);p.add_argument('--output',type=pathlib.Path);p.add_argument('--repetitions',type=int,default=4);a=p.parse_args()
    if not 1<=a.repetitions<=32:p.error('bounded repetitions 1..32 required')
    out=evidence_directory('measure-scene',a.output);variants={'reference':identify(a.reference.resolve())}
    if a.candidate:variants['candidate']=identify(a.candidate.resolve())
    if os.getuid()==0:raise ValueError('unprivileged measurements required')
    if int(environment()['process_status']['CapEff'],16)!=0:raise ValueError('effective capabilities must remain zero')
    cases=[(f,n) for f in FAMILIES for n in SCALES];random.Random(42005).shuffle(cases)
    metadata={'schema':1,'kind':'scene-m1-native-matrix','started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'variants':variants,'runner_sha256':digest(pathlib.Path(__file__)),'cases':cases,'warmups':2,'trials':9,'repetitions':a.repetitions,'order_seed':42005,'environment_before':environment(),'runs':[],'accepted':False}
    (out/'matrix.json').write_text(json.dumps(metadata,indent=2)+'\n')
    try:
        for repetition in range(a.repetitions):
            shift=(repetition*5)%len(cases);ordered=cases[shift:]+cases[:shift]
            for case_index,(family,n) in enumerate(ordered):
                names=variant_order(repetition,(family,n),cases,variants)
                for variant in names:
                    label=f'r{repetition:02d}-{family}-{n}-{variant}';raw=out/(label+'.jsonl');err=out/(label+'.stderr');timing=out/(label+'.time.txt')
                    command=['/usr/bin/time','-v','-o',str(timing),variants[variant]['path'],'--family',family,'--objects',str(n),'--warmups','2','--trials','9'];start=time.monotonic()
                    with raw.open('wb') as stdout,err.open('wb') as stderr:process=subprocess.run(command,stdout=stdout,stderr=stderr,timeout=600,check=False)
                    run={'label':label,'repetition':repetition,'family':family,'objects':n,'variant':variant,'command':command,'exit_code':process.returncode,'orchestrator_wall_seconds':time.monotonic()-start,'raw':str(raw),'raw_sha256':digest(raw),'stderr':str(err),'stderr_sha256':digest(err),'time_receipt':str(timing),'time_sha256':digest(timing),'verified':False}
                    metadata['runs'].append(run)
                    if process.returncode:raise ValueError(f'workload failed: {label}; preserve raw stderr')
                    validate_rows([json.loads(line) for line in raw.read_text().splitlines()],variants[variant]['identity'],family,n);run['verified']=True
                    (out/'matrix.json').write_text(json.dumps(metadata,indent=2)+'\n');print(json.dumps({'completed':label,'seconds':round(run['orchestrator_wall_seconds'],3)}),flush=True)
        metadata['accepted']=True
    finally:
        metadata['finished_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();metadata['environment_after']=environment();metadata['artifacts_unchanged']=all(digest(pathlib.Path(v['path']))==v['sha256'] for v in variants.values());metadata['accepted']=metadata['accepted'] and metadata['artifacts_unchanged'];(out/'matrix.json').write_text(json.dumps(metadata,indent=2)+'\n')
    return 0 if metadata['accepted'] else 1
if __name__=='__main__':raise SystemExit(main())

"""Native Scene/Assets composition tests. CPU contract category, NOT graphics."""
import argparse,json,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--binary',required=True);args=p.parse_args()
results=[]
for variant in ['base','base','changed','base']:
    with tempfile.TemporaryDirectory(prefix='race-render-cpu-') as root:
        run=subprocess.run([args.binary,root,'cpu',variant],capture_output=True,text=True,timeout=30)
        assert run.returncode==0,run.stderr
        result=json.loads(run.stdout);results.append(result)
        assert result['object']==[20,1] and result['resource']==[30,1]
        assert result['retained_bytes']<=1024*1024
assert results[0]==results[1]==results[3]
assert results[2]['generation']>results[0]['generation']
assert results[2]['positions']!=results[0]['positions']
assert results[2]['resource_revision']==results[0]['resource_revision']
assert results[2]['color']==results[0]['color']
print('render fixture: four CPU-only compositions, stable identity/change/retention passed')

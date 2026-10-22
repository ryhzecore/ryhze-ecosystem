#!/usr/bin/env python3
"""Opt-in executor graphics gate; never classified as native sanitizer coverage.
Each actual draw is a fresh bounded subprocess. This records correctness and
resource sizes, not scheduler/renderer performance. The packaged driver is CPU.
"""
from output_paths import evidence_directory
import argparse,hashlib,json,os,resource,subprocess,sys,tempfile
from render_process import native_fixture, read_fixture_line, FixtureStartupError, FixtureStartupTimeout
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
ICD='/usr/lib/chromium/vk_swiftshader_icd.json'
p=argparse.ArgumentParser();p.add_argument('--output',type=Path);p.add_argument('--fixture-binary',type=Path);a=p.parse_args();a.output=evidence_directory('software-graphics',a.output)
rows=[]
base={'positions':[[-.7,.7,0,1],[.7,.7,0,1],[0,-.7,0,1]],'color':[64/255,160/255,240/255,1]}

def run(name,width=128,height=96,fixture=None,success=True,icd=ICD,raw=None):
    fixture_path=a.output/(name+'-input.json')
    fixture_path.write_text(raw if raw is not None else json.dumps(fixture or base))
    output=a.output/name
    command=[sys.executable,str(ROOT/'tests/graphics/vulkan_triangle.py'),'--output',str(output),'--width',str(width),'--height',str(height),'--fixture',str(fixture_path)]
    env=os.environ.copy();env['VK_DRIVER_FILES']=icd
    result=subprocess.run(command,env=env,capture_output=True,text=True,timeout=40)
    (a.output/(name+'.stdout.log')).write_text(result.stdout);(a.output/(name+'.stderr.log')).write_text(result.stderr)
    record=json.loads((output/'result.json').read_text())
    row={'name':name,'expected_success':success,'exit_code':result.returncode,'command':command,'result':record}
    rows.append(row)
    assert (result.returncode==0)==success,row
    assert record['status']==('passed' if success else 'failed'),row
    if success:
        assert record['fence_wait_result']==0 and record['device_wait_idle_result']==0,row
        assert record['destroyed_objects']==11 and record['freed_allocations']==2,row
        assert record['device']['type']==4 and not record['hardware_accelerated'],row
    return record

try:
    reference=run('triangle-128x96')
    for w,h in [(64,64),(255,127),(512,512),(128,96)]:
        r=run(f'triangle-{w}x{h}-repeat',w,h)
        if (w,h)==(128,96): assert r['pixels_sha256']==reference['pixels_sha256']
    for i in range(3): assert run(f'cleanup-repeat-{i}')['pixels_sha256']==reference['pixels_sha256']
    color=json.loads(json.dumps(base));color['color']=[1,.25,.125,1]
    assert run('color-change',fixture=color)['pixels_sha256']!=reference['pixels_sha256']
    shifted=json.loads(json.dumps(base))
    for v in shifted['positions']:v[0]+=.15
    assert run('position-change',fixture=shifted)['pixels_sha256']!=reference['pixels_sha256']
    reversed_input=json.loads(json.dumps(base));reversed_input['positions'].reverse()
    assert run('reverse-winding',fixture=reversed_input)['pixels_sha256']==reference['pixels_sha256']
    for size in [15,513]: run('invalid-width-'+str(size),width=size,success=False)
    for name,edit in [
        ('degenerate',lambda x:x['positions'].__setitem__(2,x['positions'][0])),
        ('out-of-clip',lambda x:x['positions'][0].__setitem__(0,2)),
        ('nonfinite',lambda x:x['positions'][0].__setitem__(0,float('nan'))),
        ('invalid-w',lambda x:x['positions'][0].__setitem__(3,0)),
        ('nonopaque',lambda x:x['color'].__setitem__(3,.5)),
        ('out-of-color',lambda x:x['color'].__setitem__(0,2))]:
        data=json.loads(json.dumps(base));edit(data);run(name,fixture=data,success=False)
    for name, rgb in [('matching-clear-color',[8,12,20]),('near-clear-color',[9,11,21])]:
        data=json.loads(json.dumps(base));data['color']=[x/255 for x in rgb]+[1]
        rejected=run(name,fixture=data,success=False)
        assert 'foreground indistinguishable from clear color' in rejected['error']
    run('malformed-json',raw='{',success=False);run('missing-fields',raw='{}',success=False)
    run('oversized-input',raw=' '*65537,success=False)
    no_device=run('missing-icd',icd=str(a.output/'absent-icd.json'),success=False)
    assert 'vkCreateInstance: VkResult -9' in no_device['error']
    if a.fixture_binary:
        fixture_results=[]
        for i,variant in enumerate(['base','base','changed','base','base']):
            with tempfile.TemporaryDirectory(prefix='race-render-vulkan-assets-') as root:
                command=[str(a.fixture_binary.resolve()),root,'software',variant]
                with native_fixture(command) as proc:
                    try:
                        line=read_fixture_line(proc)
                    except (FixtureStartupError, FixtureStartupTimeout) as error:
                        (a.output/f'race-native-{i}.startup-partial.bin').write_bytes(error.partial)
                        (a.output/f'race-native-{i}.startup-error.json').write_text(json.dumps({'error':str(error),'pid':proc.pid,'graphics_started':False})+'\n')
                        raise
                    data=json.loads(line.decode('utf-8'))
                    # The native process remains alive, owning its final Assets lease
                    # until this separate graphics process fences, reads and quiesces.
                    assert proc.poll() is None
                    # run() owns/reaps the graphics subprocess before returning
                    # or raising (including its watchdog). Only then may this
                    # context abort the native lease holder or send an ack.
                    failed_device=i==4
                    r=run(f'race-fixture-{i}-{variant}',fixture=data,success=not failed_device,icd=str(a.output/'absent-icd.json') if failed_device else ICD)
                    if failed_device:
                        # This protocol failure is specifically a no-device case,
                        # never a blanket acknowledgement of arbitrary draw failure.
                        assert r['error']=='RuntimeError: vkCreateInstance: VkResult -9'
                        assert not r.get('device') and not r.get('calls')
                        assert 'fence_wait_result' not in r
                    stdout,stderr=proc.communicate(b'software-quiescent-failure\n' if failed_device else b'software-fence-complete\n',timeout=15)
                    (a.output/f'race-native-{i}.json').write_bytes(line)
                    (a.output/f'race-native-{i}.stderr.log').write_bytes(stderr)
                    assert proc.returncode==(1 if failed_device else 0),(proc.returncode,stderr)
                    assert not stdout
                    fixture_results.append(r)
                    rows[-1]['native_command']=command;rows[-1]['native_exit_code']=proc.returncode;rows[-1]['native_input']=data
        assert fixture_results[0]['pixels_sha256']==fixture_results[1]['pixels_sha256']==fixture_results[3]['pixels_sha256']
        assert fixture_results[2]['pixels_sha256']!=fixture_results[0]['pixels_sha256']
    summary={'status':'passed','cases':len(rows),'rows':rows,'binary_sha256':hashlib.sha256(a.fixture_binary.read_bytes()).hexdigest() if a.fixture_binary else None,
             'children_max_rss_kib':resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss,
             'memory_note':'Maximum child RSS reported by OS across fresh processes; not GPU memory. Explicit image/buffer bytes per case are separate. Driver/compiler host allocations are not controlled by RACE allocator.',
             'limits':['software CPU only','no Vulkan validation layer installed','driver/compiler/Python not covered by native sanitizers','no performance claims']}
except Exception as error:
    summary={'status':'failed','error':f'{type(error).__name__}: {error}','rows':rows}
(a.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps({k:v for k,v in summary.items() if k!='rows'},indent=2))
sys.exit(0 if summary['status']=='passed' else 1)

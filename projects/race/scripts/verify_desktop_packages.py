"""Local package CLI and actual Qt/SDL GPU parity; retains every command/result."""
from output_paths import evidence_directory
import argparse, hashlib, json, shutil, struct, subprocess, time
from pathlib import Path

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--build',type=Path,required=True)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--fixtures',type=Path,required=True)
    parser.add_argument('--large',type=Path)
    args=parser.parse_args();build=args.build.resolve();out=evidence_directory('desktop-packages',args.output)
    calls=[];checks=[];receipts=[];complete=False
    def check(name,value):
        checks.append({'check':name,'passed':bool(value)})
        if not value:raise RuntimeError(name)
    def run(exe,*argv,expected=0):
        cmd=[str(build/exe),*map(str,argv)];begin=time.perf_counter()
        try:
            result=subprocess.run(cmd,capture_output=True,text=True,timeout=120)
        except subprocess.TimeoutExpired as e:
            calls.append({'command':cmd,'timeout':True,'stdout':str(e.stdout),'stderr':str(e.stderr)});raise
        calls.append({'command':cmd,'exit_code':result.returncode,'seconds':time.perf_counter()-begin,'stdout':result.stdout,'stderr':result.stderr})
        check(f'command {len(calls)} exit',result.returncode==expected);return result.stdout
    def json_file(path):return json.loads(path.read_text(encoding='utf-8-sig'))
    def smoke(name,app,scene=None,package=None,assets=None):
        dest=out/name;argv=['--mesh-smoke',dest]
        if scene:argv+=['--scene',scene]
        if package:argv+=['--package',package]
        if assets:argv+=['--assets',assets]
        run('race_editor.exe' if app=='Qt' else 'race_runtime_desktop.exe',*argv)
        receipt=json_file(dest/'mesh-receipt.json');receipts.append({'name':name,**receipt})
        check(name+' D3D12 evidence',receipt['passed'] and receipt['hardware'] and receipt['debug_layer'] and not receipt['debug_errors'] and not receipt['debug_warnings'])
        check(name+' Play preserves authoring',(dest/'mesh.rscene').read_bytes()==(dest/'after-play.rscene').read_bytes())
        check(name+' actual mesh differs from control',(dest/'scene.ppm').read_bytes()!=(dest/'cube-control.ppm').read_bytes())
        if package:
            packed_bytes=package.read_bytes();scene_keys=[]
            for entry in range(struct.unpack_from('<I',packed_bytes,24)[0]):
                at=64+entry*112
                if struct.unpack_from('<I',packed_bytes,at+48)[0]==2:
                    high,low=struct.unpack_from('<QQ',packed_bytes,at)
                    scene_keys.append((f'{high:016x}{low:016x}',packed_bytes[at+16:at+48].hex()))
            check(name+' package identity',receipt['scene_source_kind']=='package' and scene_keys==[(receipt['package_scene_id'],receipt['package_scene_revision'])] and Path(receipt['scene_source'])==package)
            check(name+' load measured',receipt['scene_load_ms']>0)
        return dest
    def parity(name,a,b):
        for leaf in ['scene.ppm','play.ppm','cube-control.ppm','mesh.rscene']:
            check(name+' '+leaf,(a/leaf).read_bytes()==(b/leaf).read_bytes())
    try:
        content=out/'content';content.mkdir();published=out/'published';published.mkdir()
        # These are previously validated fixture bytes only, never inherited acceptance.
        for path in (args.fixtures/'assets').glob('*.rasset'):shutil.copyfile(path,content/path.name)
        shutil.copyfile(args.fixtures/'qt-two-revisions'/'mesh.rscene',content/'scene.rscene')
        scene_id='00000000000000020000000000000001'
        first=json.loads(run('race_package_tool.exe','pack',content,'scene.rscene',published,scene_id))
        second=json.loads(run('race_package_tool.exe','pack',content,'scene.rscene',published,scene_id))
        check('repeated immutable publication deterministic',first==second)
        package=published/first['leaf'];bytes_=package.read_bytes()
        check('content filename equals actual SHA256',hashlib.sha256(bytes_).hexdigest()+'.rpak'==package.name)
        for mode in ['inspect','validate']:
            report=json.loads(run('race_package_tool.exe',mode,published,package.name));check(mode+' exact scene and closure',report['scene_id']==scene_id and report['meshes']==2 and report['sha256']==first['sha256'])
        count,dep_count=struct.unpack_from('<II',bytes_,24);check('exact three-entry closure',count==3 and dep_count==2)
        scene_payloads=[];mesh_payloads=[]
        for n in range(count):
            at=64+n*112;kind=struct.unpack_from('<I',bytes_,at+48)[0];offset,length=struct.unpack_from('<QQ',bytes_,at+72);revision=bytes_[at+16:at+48].hex();payload=bytes_[offset:offset+length]
            check('entry payload hash '+str(n),hashlib.sha256(payload).hexdigest()==revision)
            if kind==2:scene_payloads.append(payload)
            else:mesh_payloads.append(revision);check('exact loose mesh bytes '+str(n),payload==(content/(revision+'.rasset')).read_bytes())
        check('exact cooked Scene bytes',scene_payloads==[(content/'scene.rscene').read_bytes()])
        runs={}
        for app in ['Qt','SDL']:
            runs[app+'-loose']=smoke(app.lower()+'-loose',app,scene=content/'scene.rscene',assets=content)
            runs[app+'-packed']=smoke(app.lower()+'-packed',app,package=package)
            parity(app+' packed/loose',runs[app+'-loose'],runs[app+'-packed'])
        parity('Qt/SDL packed',runs['Qt-packed'],runs['SDL-packed'])
        check('two revisions of stable ID',len({e['asset'] for e in receipts[-1]['meshes']})==1 and len({e['revision'] for e in receipts[-1]['meshes']})==2)
        collision=out/'collision';collision.mkdir();sentinel=b'preserved prior output';(collision/package.name).write_bytes(sentinel)
        run('race_package_tool.exe','pack',content,'scene.rscene',collision,scene_id,expected=1);check('collision preserves bytes',(collision/package.name).read_bytes()==sentinel)
        malformed=out/'malformed';malformed.mkdir()
        for name,offset,value,width in [('codec',64+60,1,4),('schema',64+52,2,4),('target',64+56,2,4),('offset',64+72,2**64-1,8),('count',24,2**32-1,4)]:
            bad=bytearray(bytes_);bad[offset:offset+width]=value.to_bytes(width,'little');bad[-32:]=hashlib.sha256(bad[:-32]).digest();(malformed/(name+'.rpak')).write_bytes(bad)
            run('race_package_tool.exe','validate',malformed,name+'.rpak',expected=1)
        scene_blob=b'scene';mesh_blob=b'\x00\x7f\xff'
        key=lambda number,payload:struct.pack('<QQ',0,number)+hashlib.sha256(payload).digest()
        entry=lambda number,payload,kind,offset,first,count:key(number,payload)+struct.pack('<IIIIQQQQIIQ',kind,1,1,0,0,offset,len(payload),len(payload),first,count,0)
        vector=struct.pack('<8sIIQIIIIQQQ',b'RACEPAK1',1,64,376,2,1,112,48,64,336,0)
        vector+=entry(1,scene_blob,2,336,0,1)+entry(2,mesh_blob,1,341,1,0)+key(2,mesh_blob)+scene_blob+mesh_blob
        vector+=hashlib.sha256(vector).digest()
        golden=malformed/'opaque-container-v1.rpak';golden.write_bytes(vector)
        check('independent container identity',hashlib.sha256(golden.read_bytes()).hexdigest()=='675929d68a40dfab4c855d8db4d02af62924dfbbbf7864847eb0f6577a4bbbe5')
        run('race_package_tool.exe','validate',golden.parent,golden.name,expected=1)
        for app,exe,code in [('Qt','race_editor.exe',64),('SDL','race_runtime_desktop.exe',1)]:
            run(exe,'--package',package,'--scene',content/'scene.rscene','--mesh-smoke',out/(app+'-conflict'),expected=code)
            run(exe,'--package',malformed/'codec.rpak','--mesh-smoke',out/(app+'-bad-package'),expected=1)
        if args.large:
            large=args.large.resolve()
            for app in ['Qt','SDL']:
                a=smoke(app.lower()+'-large-loose',app,scene=large/'scene.rscene',assets=large)
                b=smoke(app.lower()+'-large-packed',app,package=large/'measure.rpak');parity(app+' large packed/loose',a,b)
                if app=='Qt':
                    check('large package UI callbacks observed while request retained',receipts[-1]['callbacks_while_package_request_retained']>0)
                    check('large package successful worker time recorded',receipts[-1]['successful_package_worker_ms']>0)
            parity('large Qt/SDL packed',out/'qt-large-packed',out/'sdl-large-packed')
        check('every app same source bytes',len({r['authored_input_sha256'] for r in receipts})==1)
        complete=True
    finally:
        result={'passed':complete and all(c['passed'] for c in checks),'checks':checks,'calls':calls,'receipts':receipts,
                'artifacts':[{'path':str(p.relative_to(out)),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in sorted(out.rglob('*')) if p.is_file() and p.suffix in ['.rpak','.rscene','.ppm']],
                'scope':'New local Windows D3D12 executions. Prior fixtures are inputs only. No publication or full roadmap acceptance.'}
        (out/'verification.json').write_text(json.dumps(result,indent=2))
    print(json.dumps({'passed':True,'checks':len(checks),'application_runs':len(receipts),'receipt':str(out/'verification.json')}))
if __name__=='__main__':main()

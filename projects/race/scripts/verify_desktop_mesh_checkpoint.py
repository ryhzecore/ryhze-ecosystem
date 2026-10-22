"""Exercise explicit Qt import, checkpoint capture and Scene adoption and compare actual D3D12 readbacks."""
from output_paths import evidence_directory
from test_content_project import binary_index, parse_binary_index
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--output',type=Path)
p.add_argument('--tools',type=Path,help='Fixture tools directory for extracted-package checks')
p.add_argument('--authoring',action='store_true',help='Create project/recipes through native editor controls before import')
a=p.parse_args();build=a.build.resolve();output=evidence_directory('desktop-mesh-checkpoint',a.output);fixture_tools=(a.tools or build).resolve()
if a.authoring:
    from verify_desktop_project_authoring import prepare_reference,verify_authoring
commands=[];checks=[];runs=[];authoring=[];passed=False
identity='000000000000004d0000000000000001'
def sha(blob):return hashlib.sha256(blob).hexdigest()
def read(path):return json.loads(path.read_text(encoding='utf-8-sig'))
def check(name,condition):
    checks.append(dict(check=name,passed=bool(condition)))
    if not condition:raise RuntimeError(name)
def run(name,*arguments):
    base=fixture_tools if name in ['race_mesh_tool.exe','race_mesh_import_tool.exe','race_content_tool.exe'] else build
    command=[str(base/name),*map(str,arguments)]
    try:result=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=90)
    except subprocess.TimeoutExpired as exc:
        commands.append(dict(command=command,timeout_seconds=90));raise
    commands.append(dict(command=command,exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr))
    check(name+' exit '+str(len(commands)),result.returncode==0)
    return result.stdout
try:
    shape='v 1 0 0\nv 3 0 0\nv 2 2 0\nv 2 0 2\n'
    faces='f 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n'
    for label,repetitions in [('small',1),('large',3000)]:
        root=output/label;root.mkdir()
        for folder in ['source','imported','assets','reference-assets','recipes','checkpoints']:(root/folder).mkdir()
        source=root/'source';assets=root/'assets';recipes=root/'recipes';checkpoints=root/'checkpoints'
        def product(leaf,obj,destination):
            (source/leaf).write_bytes(obj.encode('ascii'))
            imported=json.loads(run('race_mesh_tool.exe','import',source,leaf,root/'imported',identity))
            return json.loads(run('race_mesh_tool.exe','cook',root/'imported',imported['leaf'],destination))
        first=product('a.obj',shape+faces,assets)
        obj=shape.replace('v 3 0 0','v 4 0 0')+faces*repetitions
        second=product('b.obj',obj,root/'reference-assets')
        if a.authoring:
            (source/'unused.obj').write_bytes((shape+faces).encode('ascii'))
            reference=prepare_reference(root,identity,run,check,label)
            project_source=reference['root'];main_recipe=reference['main_recipe']
        else:
            run('race_mesh_import_tool.exe','--create-recipe',recipes,'mesh.json',identity,'b.obj')
            run('race_mesh_import_tool.exe','--create-recipe',recipes,'other.json','000000000000004d0000000000000002','unused.obj')
            run('race_mesh_import_tool.exe','--create-project',recipes,'project.raceproject','mesh.json','other.json')
            raw=(recipes/'project.raceproject').read_bytes()
            references=parse_binary_index(raw)['recipes']
            check(label+' independent binary project index',raw==binary_index(references))
            (recipes/'alternate-recipe.json').write_bytes((recipes/references[1]['recipe']).read_bytes())
            changed=[references[0],{**references[1],'recipe':'alternate-recipe.json'}]
            (recipes/'changed-project.raceproject').write_bytes(binary_index(changed))
            bad=[references[0],{**references[1],'revision':'f'*64}]
            (recipes/'bad-project.raceproject').write_bytes(binary_index(bad))
            project_source=recipes;main_recipe='mesh.json'
        project_leaf='project.raceproject'
        definition=dict(schema=1,asset=identity,index_revision=sha((project_source/project_leaf).read_bytes()),recipe_revision=sha((project_source/main_recipe).read_bytes()),source=sha(obj.encode('ascii')),cooked_revision=second['revision'])
        manifest=(json.dumps(definition,separators=(',',':'))+'\n').encode()
        leaf=sha(manifest)+'.json'
        check(label+' new product absent before editor',not (assets/second['leaf']).exists())
        check(label+' checkpoint absent before editor',not (checkpoints/leaf).exists())
        qt=root/'qt'
        run('race_editor.exe','--assets',assets,'--mesh-product',first['leaf'],'--checkpoint-project',recipes/project_leaf,'--checkpoint-file',checkpoints/leaf,'--authoring-smoke' if a.authoring else '--checkpoint-smoke',qt)
        check(label+' editor captures exact checkpoint',(checkpoints/leaf).read_bytes()==manifest)
        check(label+' editor publishes exact cooked product',(assets/second['leaf']).read_bytes()==(root/'reference-assets'/second['leaf']).read_bytes())
        if a.authoring:run('race_content_tool.exe','inspect',recipes,project_leaf)
        else:run('race_mesh_import_tool.exe','--load-checkpoint',recipes,project_leaf,assets,checkpoints,leaf)
        stages=list(checkpoints.glob('race-import-*'))
        check(label+' fresh retained staging',sum((v/'result.rasset').is_file() for v in stages)>=2 and all((v/'input.obj').read_bytes()==obj.encode('ascii') for v in stages if (v/'input.obj').exists()))
        receipt=read(qt/'checkpoint-receipt.json');runs.append(dict(name=label,**receipt))
        check(label+' all Qt checks',receipt['passed'] and all(x.get('passed',False) for x in receipt['checks']))
        if a.authoring:authoring.append(verify_authoring(root,reference,receipt,check,label))
        check(label+' reimport terminal metrics',len(receipt.get('reimport_measurements',[]))==4 and sum(m['passed'] for m in receipt['reimport_measurements'])==2)
        check(label+' reimport bounded metrics',all(all(m[k]>=0 for k in ['admission_ms','revalidate_ms','source_ms','import_ms','cook_publish_ms','capture_ms','publication_ms','qt_publication_ms']) and m['source_bytes']<=8*1024*1024 and m['product_bytes']<=16*1024*1024 for m in receipt['reimport_measurements']))
        if label=='large':check('large import continues callbacks',all(m['callbacks']>0 for m in receipt['reimport_measurements'] if m['passed']))
        check(label+' browser publication metrics',len(receipt.get('browser_measurements',[]))==4 and all(all(m[k]>=0 for k in ['admission_ms','read_ms','publication_ms','qt_publication_ms']) for m in receipt['browser_measurements']))
        check(label+' browser validated rows',all(m['entries']==2 and m['retained_bytes']<=256*1024 for m in receipt['browser_measurements'] if m['passed']))
        check(label+' hardware and debug',receipt['hardware'] and receipt['debug_layer'] and not receipt['debug_errors'] and not receipt['debug_warnings'])
        for left,right in [('before.ppm','undo.ppm'),('updated.ppm','redo.ppm'),('updated.ppm','loose-control.ppm')]:check(label+' '+left+' exact '+right,(qt/left).read_bytes()==(qt/right).read_bytes())
        check(label+' update changes rendered mesh',(qt/'before.ppm').read_bytes()!=(qt/'updated.ppm').read_bytes())
        if label=='large':check('large worker continues callbacks',any(m['changed'] and m['callbacks']>0 for m in receipt['measurements']))
        sdl=root/'sdl'
        run('race_runtime_desktop.exe','--assets',assets,'--scene',qt/'updated.rscene','--mesh-smoke',sdl)
        runtime=read(sdl/'mesh-receipt.json')
        check(label+' SDL hardware',runtime['passed'] and runtime['hardware'] and not runtime['debug_errors'] and not runtime['debug_warnings'])
        check(label+' Qt SDL updated pixels',(sdl/'scene.ppm').read_bytes()==(qt/'updated.ppm').read_bytes())
        check(label+' Qt SDL cooked closure',(sdl/'mesh.rscene').read_bytes()==(qt/'updated.rscene').read_bytes())
    passed=True
finally:
    record=dict(passed=passed,commands=commands,checks=checks,runs=runs,binaries={name:sha(((fixture_tools if name in ['race_mesh_import_tool.exe','race_mesh_tool.exe','race_content_tool.exe'] else build)/name).read_bytes()) for name in ['race_editor.exe','race_runtime_desktop.exe','race_mesh_import_tool.exe','race_mesh_tool.exe','race_mesh_import_worker.exe','race_content_tool.exe']},scope='Actual native Qt/SDL execution and GPU readbacks; Qt synthetic event input; not physical input latency or whole-engine acceptance')
    if a.authoring:
        record['authoring']=authoring
        record['scope']='Actual Qt project/recipe creation against separate CLI byte oracles, followed by import/checkpoint/adoption and Qt/SDL GPU readbacks; synthetic input, not physical input latency or whole-engine acceptance'
    (output/'receipt.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
print(json.dumps(dict(passed=passed,checks=len(checks),receipt=str(output/'receipt.json'))))

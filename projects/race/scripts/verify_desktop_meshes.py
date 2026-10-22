#!/usr/bin/env python3
"""Local Qt/SDL mesh acceptance with real D3D12 readbacks; no keyboard focus needed."""
from output_paths import evidence_directory
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    build, output = args.build.resolve(), evidence_directory('desktop-meshes', args.output)
    calls, checks, receipts = [], [], []
    completed = False

    def check(name, passed):
        checks.append({'check': name, 'passed': bool(passed)})
        if not passed:
            raise RuntimeError(name)

    def run(exe, *argv, reject=False):
        command = [str(build / exe), *map(str, argv)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=50)
        calls.append({'command': command, 'exit_code': result.returncode,
                      'stdout': result.stdout, 'stderr': result.stderr})
        check(f'{exe} call {len(calls)} expected exit', result.returncode == (1 if reject else 0))
        return result.stdout

    def read_json(path):
        return json.loads(path.read_text(encoding='utf-8-sig'))

    def write_json(path, value):
        path.write_text(json.dumps(value, indent=2), encoding='utf-8')

    def product(name, obj):
        (output / 'source' / (name + '.obj')).write_text(obj, encoding='ascii')
        imported = json.loads(run('race_mesh_tool.exe', 'import', output / 'source', name + '.obj',
                                  output / 'imported', '00000000000000010000000000000001'))
        cooked = json.loads(run('race_mesh_tool.exe', 'cook', output / 'imported', imported['leaf'], output / 'assets'))
        return imported, cooked

    def smoke(name, app, scene=None, mesh=None):
        destination = output / name
        argv = ['--assets', output / 'assets', '--mesh-smoke', destination]
        if scene:
            argv += ['--scene', scene]
        if mesh:
            argv += ['--mesh-product', mesh['leaf']]
        run('race_editor.exe' if app == 'Qt' else 'race_runtime_desktop.exe', *argv)
        receipt = read_json(destination / 'mesh-receipt.json')
        check(name + ' hardware and diagnostics', receipt['passed'] and receipt['hardware']
              and not receipt['debug_errors'] and not receipt['debug_warnings'])
        receipts.append({'name': name, **receipt})
        check(name + ' authoring survives Play', (destination / 'mesh.rscene').read_bytes()
              == (destination / 'after-play.rscene').read_bytes())
        check(name + ' real mesh differs from cube', (destination / 'scene.ppm').read_bytes()
              != (destination / 'cube-control.ppm').read_bytes())
        return destination

    def parity(name, a, b):
        for leaf in ['scene.ppm', 'play.ppm', 'cube-control.ppm', 'mesh.rscene']:
            check(name + ' ' + leaf + ' exact parity', (a / leaf).read_bytes() == (b / leaf).read_bytes())

    def set_revision(obj, revision):
        digest = bytes.fromhex(revision)
        component = next(c for c in obj['components'] if c['type'] == 3)
        for field in component['fields']:
            if field['id'] in [2, 3]:
                offset = (field['id'] - 2) * 16
                field['value'] = ''.join(f'{int.from_bytes(digest[i:i+8], "little"):016x}'
                                         for i in [offset, offset + 8])

    def negative(name, scene, root):
        for app, exe in [('Qt', 'race_editor.exe'), ('SDL', 'race_runtime_desktop.exe')]:
            argv = ['--scene', scene, '--mesh-smoke', output / (name + '-' + app)]
            if root:
                argv += ['--assets', root]
            run(exe, *argv, reject=True)

    try:
        for folder in ['source', 'imported', 'assets', 'empty', 'corrupt']:
            (output / folder).mkdir()
        obj = 'v 1 0 0\nv 3 0 0\nv 2 2 0\nv 2 0 2\nf 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n'
        imported, first = product('tetrahedron', obj)
        _, second = product('tetrahedron-wide', obj.replace('v 3 0 0', 'v 4 0 0'))
        authored = smoke('qt-author', 'Qt', mesh=first)
        for app in ['Qt', 'SDL']:
            for extension in ['scene.json', 'rscene']:
                loaded = smoke(app.lower() + '-' + extension.replace('.', '-'), app, authored / ('mesh.' + extension))
                parity(app + ' ' + extension, authored, loaded)
        scene = read_json(authored / 'mesh.scene.json')
        multi = copy.deepcopy(scene)
        mesh = multi['objects'][1]
        transform = next(c for c in mesh['components'] if c['type'] == 1)
        transform['fields'][0]['value'] = [-1, 0, 0]
        transform['fields'][2]['value'] = [-1, 1.5, .75]
        next_mesh = copy.deepcopy(scene['objects'][1])
        next_mesh['id'] = '52414345424f58010000000000000003'
        set_revision(next_mesh, second['revision'])
        multi['objects'].append(next_mesh)
        write_json(output / 'two-revisions.scene.json', multi)
        qt = smoke('qt-two-revisions', 'Qt', output / 'two-revisions.scene.json')
        sdl = smoke('sdl-two-revisions', 'SDL', qt / 'mesh.rscene')
        parity('two revisions, mirrored nonuniform transform', qt, sdl)
        check('two revisions of one stable asset retained', len(receipts[-1]['meshes']) == 2
              and {m['asset'] for m in receipts[-1]['meshes']} == {first['id']}
              and {m['revision'] for m in receipts[-1]['meshes']} == {first['revision'], second['revision']})
        negative('no-assets-root', authored / 'mesh.rscene', None)
        negative('missing-product', authored / 'mesh.scene.json', output / 'empty')
        corrupt = bytearray((output / 'assets' / first['leaf']).read_bytes())
        corrupt[-1] ^= 1
        (output / 'corrupt' / first['leaf']).write_bytes(corrupt)
        negative('corrupt-product', authored / 'mesh.rscene', output / 'corrupt')
        wrong = copy.deepcopy(scene)
        next(c for c in wrong['objects'][1]['components'] if c['type'] == 3)['fields'][0]['value'] = '00000000000000010000000000000002'
        write_json(output / 'wrong-id.scene.json', wrong)
        negative('wrong-id', output / 'wrong-id.scene.json', output / 'assets')
        imported_scene = copy.deepcopy(scene)
        set_revision(imported_scene['objects'][1], imported['revision'])
        write_json(output / 'imported.scene.json', imported_scene)
        negative('imported-only', output / 'imported.scene.json', output / 'imported')
        conflict = copy.deepcopy(scene)
        conflict['objects'][1]['components'].append(copy.deepcopy(scene['objects'][0]['components'][1]))
        write_json(output / 'conflict.scene.json', conflict)
        negative('box-mesh-conflict', output / 'conflict.scene.json', output / 'assets')
        _, large = product('large-bounds', obj.replace('v 1 0 0', 'v 1000 0 0').replace('v 3 0 0', 'v 2000 0 0').replace('v 2 2 0', 'v 1500 1000 0').replace('v 2 0 2', 'v 1500 0 1000'))
        large_run = smoke('qt-large-bounds', 'Qt', mesh=large)
        ppm = (large_run / 'framed.ppm').read_bytes().split(b'\n', 3)[3]
        blue = sum(1 for i in range(0, len(ppm), 3) if ppm[i+2] > 100 and ppm[i+2] > ppm[i+1] * 1.10 and ppm[i+1] > ppm[i] * 1.10)
        check('large framed mesh visibly rasterized', blue > 1000)
        _, dense = product('instance-budget', 'v 1 0 0\nv 3 0 0\nv 2 2 0\n' + 'f 1 3 2\n' * 20001)
        dense_run = smoke('qt-instance-budget', 'Qt', mesh=dense)
        dense_scene = read_json(dense_run / 'mesh.scene.json')
        for n in range(3, 6):
            instance = copy.deepcopy(dense_scene['objects'][1])
            instance['id'] = f'52414345424f5801{n:016x}'
            dense_scene['objects'].append(instance)
        write_json(output / 'over-budget.scene.json', dense_scene)
        negative('over-budget', output / 'over-budget.scene.json', output / 'assets')
        check('all apps use identical authored source', len({r['authored_input_sha256'] for r in receipts}) == 1)
        completed = True
    finally:
        result = {'schema': 1, 'passed': completed and bool(checks) and all(c['passed'] for c in checks),
                  'checks': checks, 'receipts': receipts, 'calls': calls,
                  'artifacts': [{'path': str(p.relative_to(output)), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                                for p in sorted(output.rglob('*')) if p.is_file() and p.suffix in ['.ppm', '.rscene']],
                  'scope': 'Local Windows D3D12 mesh integration; no broad renderer or release qualification.'}
        write_json(output / 'verification.json', result)
    print(json.dumps({'passed': True, 'checks': len(checks), 'application_runs': len(receipts),
                      'receipt': str(output / 'verification.json')}))


if __name__ == '__main__':
    main()

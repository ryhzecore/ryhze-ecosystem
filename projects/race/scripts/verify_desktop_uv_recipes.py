"""Explicit UV recipes through native workers/checkpoints and optional Qt/SDL adoption.

--native-only runs CLI contracts without a window or GPU; the default also runs
the actual mixed-profile editor authoring and textured UV-only adoption smoke.
--tools supplies fixture CLIs when --build names an extracted desktop package.
All executables must match the current checkout's source identity unless both
--expected-source-commit and --expected-authored-input-sha256 explicitly pin a
previously validated build. Application receipts must report the same identity.
"""
from test_content_project import binary_index
from output_paths import evidence_directory
from source_manifest import authored_inputs, file_sha256, git_state
import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import time


def validated_identity(commit, authored_sha256):
    for name, value, length in [('source_commit', commit, 40),
                                ('authored_input_sha256', authored_sha256, 64)]:
        if not isinstance(value, str) or len(value) != length or any(c not in '0123456789abcdef' for c in value):
            raise ValueError('Invalid ' + name + ' identity')
    return dict(source_commit=commit, authored_input_sha256=authored_sha256)


def parse_cli_identity(text):
    fields = text.split()
    if len(fields) != 2:
        raise ValueError('CLI identity must contain exactly one commit and one authored SHA-256')
    return validated_identity(*fields)


def application_identity(receipt):
    if not isinstance(receipt, dict):
        raise ValueError('Application receipt must be a JSON object')
    return validated_identity(receipt.get('source_commit'), receipt.get('authored_input_sha256'))


def verify_scene_adoption(authored, updated, old_pin, new_pin, material_pin, check):
    # The Mesh schema has three pin fields plus field 4 (color). Identify the
    # selected fixture through its pre-adoption pose, then compare the entire
    # Scene to an independent copy with only that stable object's pin replaced.
    def mesh_objects(scene):
        return [obj for obj in scene['objects'] if any(c['type'] == 3 for c in obj['components'])]

    def component(obj, kind):
        matches = [c for c in obj['components'] if c['type'] == kind]
        check('one component ' + str(kind) + ' for ' + obj['id'], len(matches) == 1)
        return matches[0]

    before_meshes, after_meshes = mesh_objects(authored), mesh_objects(updated)
    check('authored and updated Scenes have exactly two mesh instances', len(before_meshes) == len(after_meshes) == 2)
    check('authored Scene object IDs are unique', len({obj['id'] for obj in authored['objects']}) == len(authored['objects']))
    selected = [obj for obj in before_meshes if component(obj, 1)['fields'][0] ==
                dict(id=1, kind='vector', value=[2, 0, 0])]
    check('one pre-adoption selected fixture at authored translation', len(selected) == 1)
    selected_id = selected[0]['id']
    check('UV adoption changes revision under the same asset pin', old_pin != new_pin and old_pin[0] == new_pin[0])
    expected_material = dict(type=4, version=1, fields=material_pin)
    for obj in before_meshes:
        tint = [.8, .3, .2] if obj['id'] == selected_id else [.28, .52, .78]
        expected_mesh = dict(type=3, version=1, fields=old_pin + [dict(id=4, kind='vector', value=tint)])
        check('authored exact mesh pin and tint ' + obj['id'], component(obj, 3) == expected_mesh)
        check('authored exact material pin ' + obj['id'], component(obj, 4) == expected_material)
    check('both instances retain the same pinned material', all(component(obj, 4) == expected_material for obj in after_meshes))
    expected_scene = deepcopy(authored)
    expected_selected = next(obj for obj in expected_scene['objects'] if obj['id'] == selected_id)
    expected_mesh = component(expected_selected, 3)
    expected_mesh['fields'] = new_pin + [expected_mesh['fields'][3]]
    check('selected instance alone adopts B with tint transforms materials and other objects preserved', updated == expected_scene)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    destination = parser.add_mutually_exclusive_group()
    destination.add_argument('--output', type=Path, help='Explicit new evidence directory; must not exist')
    destination.add_argument('--output-parent', type=Path, help='Evidence parent for a fresh attempt-NN directory on each run')
    parser.add_argument('--tools', type=Path)
    parser.add_argument('--native-only', action='store_true')
    parser.add_argument('--mip-surfaces', action='store_true', help='Exercise explicit filtered schema2 surfaces through UV adoption')
    parser.add_argument('--expected-source-commit', help='Full validated source commit; requires --expected-authored-input-sha256')
    parser.add_argument('--expected-authored-input-sha256', help='Exact validated authored-input SHA-256; requires --expected-source-commit')
    args = parser.parse_args()
    if args.native_only and args.mip_surfaces:
        parser.error('--mip-surfaces requires the desktop workflow')
    if (args.expected_source_commit is None) != (args.expected_authored_input_sha256 is None):
        parser.error('Provide both expected identity arguments, or omit both to use the current checkout')
    build = args.build.resolve()
    tools = (args.tools or build).resolve()
    out = evidence_directory('desktop-uv-recipes', args.output, parent=args.output_parent)
    print(json.dumps(dict(output=str(out), native_only=args.native_only)), flush=True)
    checks, calls, app_receipts, executable_identities = [], [], [], []
    binaries_before, binaries_after, binary_errors = {}, {}, {}
    expected_identity = None
    identity_source = 'explicit' if args.expected_source_commit is not None else 'current-checkout'
    passed = False
    identity = '000000000000004d0000000000000001'
    other = '000000000000004d0000000000000002'
    tool_names = {'race_mesh_tool.exe', 'race_mesh_import_tool.exe', 'race_surface_tool.exe'}

    def sha(blob):
        return hashlib.sha256(blob).hexdigest()

    def check(name, condition):
        checks.append(dict(check=name, passed=bool(condition)))
        if not condition:
            raise RuntimeError(name)

    def read(path):
        return json.loads(path.read_text(encoding='utf-8-sig'))

    def snapshot(directory):
        return {path.name: sha(path.read_bytes()) for path in sorted(directory.iterdir()) if path.is_file()}

    def execute(path, *arguments, success=True):
        path = str(Path(path).resolve())
        check('executable unchanged before launch ' + path, file_sha256(Path(path)) == binaries_before[path])
        command = [path, *map(str, arguments)]
        started = time.perf_counter()
        try:
            result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=90)
        except subprocess.TimeoutExpired as error:
            calls.append(dict(command=command, timeout_seconds=90, stdout=str(error.stdout), stderr=str(error.stderr)))
            raise
        calls.append(dict(command=command, exit_code=result.returncode, stdout=result.stdout,
                          stderr=result.stderr, seconds=time.perf_counter()-started))
        check(f'command {len(calls)} expected exit', (result.returncode == 0) == success)
        return result.stdout

    def run(name, *arguments, success=True):
        return execute((tools if name in tool_names else build)/name, *arguments, success=success)

    def admit_application(name, receipt):
        app_receipts.append(dict(name=name, **receipt))
        check(name + ' expected source identity', application_identity(receipt) == expected_identity)

    def invoke(name, *arguments, **options):
        raw = run(name, *arguments, **options)
        return json.loads(raw) if options.get('success', True) else None

    def mesh(source_leaf, destination):
        imported = invoke('race_mesh_tool.exe', 'import', source, source_leaf, out/'imported', identity)
        cooked = invoke('race_mesh_tool.exe', 'cook', out/'imported', imported['leaf'], destination)
        return imported, cooked

    def geometry(blob):
        count, index_count = struct.unpack_from('<QQ', blob, 48)
        stride = struct.unpack_from('<I', blob, 240)[0]
        check('bounded independent UV product layout', blob[:8] == b'RACEAST1' and count <= 65536 and
              index_count <= 196608 and struct.unpack_from('<I', blob, 20)[0] == 2 and stride == 64 and
              len(blob) == 256+count*stride+index_count*4+32 and hashlib.sha256(blob[:-32]).digest() == blob[-32:])
        vertices = [blob[256+n*stride:256+n*stride+48] for n in range(count)]
        uv = [blob[256+n*stride+48:256+(n+1)*stride] for n in range(count)]
        return vertices, blob[256+count*stride:-32], uv

    def manifest(product, raw_source):
        definition = dict(schema=1, asset=identity, index_revision=sha(index_raw),
                          recipe_revision=sha(uv_recipe_raw), source=sha(raw_source), cooked_revision=product['revision'])
        raw = (json.dumps(definition, separators=(',', ':'))+'\n').encode('ascii')
        return sha(raw)+'.json', raw

    def watch(label, *options, success=True, duration=2000):
        text = run('race_mesh_import_tool.exe', '--watch-project', reference, 'project.raceproject', identity,
                   source, reference_assets, out/label, duration, 4, *options, success=success)
        lines = text.encode('utf-8').splitlines(keepends=True)
        check(label+' bounded receipt stream', len(lines) <= 128 and all(line.endswith(b'\n') and len(line) <= 1024 for line in lines))
        records = [json.loads(line) for line in lines]
        statuses = [item for item in records if 'phase' in item]
        if statuses:
            check(label+' physical completion', statuses[-1]['phase'] == 2 and not statuses[-1]['worker_retained'])
        if success:
            check(label+' status evidence', bool(statuses))
        return records, statuses

    def pin_fields(product):
        digest = bytes.fromhex(product['revision'])
        return [{'id': 1, 'kind': 'asset', 'value': product['id']}, *[
            {'id': n+2, 'kind': 'asset', 'value': ''.join(f'{int.from_bytes(digest[i:i+8], "little"):016x}' for i in [n*16, n*16+8])}
            for n in range(2)]]

    try:
        expected_identity = validated_identity(args.expected_source_commit, args.expected_authored_input_sha256) if identity_source == 'explicit' else validated_identity(git_state()['commit'], authored_inputs()['sha256'])
        cli_paths = [tools/name for name in ['race_mesh_tool.exe', 'race_mesh_import_tool.exe', 'race_mesh_import_worker.exe']]
        app_paths = []
        if not args.native_only:
            cli_paths += [tools/'race_surface_tool.exe', build/'race_mesh_import_worker.exe', build/'race_package_tool.exe']
            app_paths = [build/'race_editor.exe', build/'race_runtime_desktop.exe']
        # Resolve and deduplicate the two roots: the packaged editor starts its
        # own worker, while fixture imports start the worker beside --tools.
        cli_paths = list(dict.fromkeys(path.resolve() for path in cli_paths))
        required_paths = list(dict.fromkeys([*cli_paths, *(path.resolve() for path in app_paths)]))
        for path in required_paths:
            check('required executable present ' + str(path), path.is_file())
            binaries_before[str(path)] = file_sha256(path)
        for path in cli_paths:
            raw = execute(path, '--identity')
            observed = parse_cli_identity(raw)
            executable_identities.append(dict(path=str(path), **observed))
            check('CLI expected source identity ' + str(path), observed == expected_identity)
        for name in ['source', 'imported', 'assets', 'reference-assets', 'recipes', 'reference-recipes', 'checkpoints', 'native-checkpoints']:
            (out/name).mkdir()
        source, assets = out/'source', out/'assets'
        reference, reference_assets = out/'reference-recipes', out/'reference-assets'
        checkpoints = out/'native-checkpoints'
        shape = 'v 1 0 0\nv 3 0 0\nv 2 2 0\nv 2 0 2\n'
        faces = 'f 1/1 3/3 2/2\nf 1/1 2/2 4/3\nf 2/1 3/2 4/3\nf 3/1 1/2 4/3\n'
        plain = (shape+'f 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n').encode('ascii')
        first_source = (shape+'vt 0 0\nvt 1 0\nvt 0 1\n'+faces).encode('ascii')
        second_source = (shape+'vt 1 0\nvt 0 0\nvt 1 1\n'+faces).encode('ascii')
        third_source = (shape+'vt 0.25 1\nvt 1 0.25\nvt 0 0\n'+faces).encode('ascii')
        (source/'a.obj').write_bytes(first_source)
        (source/'b.obj').write_bytes(second_source)
        (source/'unused.obj').write_bytes(plain)
        _, first = mesh('a.obj', assets)
        direct, second = mesh('b.obj', reference_assets)
        a_geometry = geometry((assets/first['leaf']).read_bytes())
        b_geometry = geometry((reference_assets/second['leaf']).read_bytes())
        check('A to B changes only UV geometry attributes', a_geometry[:2] == b_geometry[:2] and a_geometry[2] != b_geometry[2])

        uv_result = invoke('race_mesh_import_tool.exe', '--create-recipe', reference, 'uv-seed.json', identity, 'b.obj', 'geometry-uv')
        legacy_result = invoke('race_mesh_import_tool.exe', '--create-recipe', reference, 'legacy-seed.json', other, 'unused.obj')
        uv_recipe_raw = (reference/'uv-seed.json').read_bytes()
        legacy_raw = (reference/'legacy-seed.json').read_bytes()
        common = dict(mesh_container=1, target=1, dependencies=sha(b''))
        expected_uv = dict(schema=2, asset=identity, source='b.obj', mesh_container=1, mesh_schema=2, target=1,
            settings=sha(b'race-obj-triangles-uv-settings-2:metres;right-handed;y-up;ccw;flat-normals;binary64;vt-double2;top-left-v=1-v'),
            importer=sha(b'race-first-party-obj-triangles-uv-importer-2'), cooker=sha(b'race-native-mesh-uv-portable-cpu-cooker-2'), dependencies=common['dependencies'])
        expected_legacy = dict(schema=1, asset=other, source='unused.obj', mesh_container=1, mesh_schema=1, target=1,
            settings=sha(b'race-obj-triangles-settings-1:metres;right-handed;y-up;ccw;flat-normals;binary64'),
            importer=sha(b'race-first-party-obj-triangles-importer-1'), cooker=sha(b'race-native-mesh-portable-cpu-cooker-1'), dependencies=common['dependencies'])
        check('independent exact UV recipe vector', uv_recipe_raw == (json.dumps(expected_uv, separators=(',', ':'))+'\n').encode() and uv_result['recipe_revision'] == sha(uv_recipe_raw))
        check('legacy recipe bytes unchanged', legacy_raw == (json.dumps(expected_legacy, separators=(',', ':'))+'\n').encode() and legacy_result['recipe_revision'] == sha(legacy_raw))
        recipe_leaves = []
        for raw in [uv_recipe_raw, legacy_raw]:
            leaf = sha(raw)+'.json'; (reference/leaf).write_bytes(raw); recipe_leaves.append(leaf)
        binary_references = [dict(asset=asset, kind='mesh', recipe=leaf, revision=sha(raw))
                             for asset, leaf, raw in [(identity, recipe_leaves[0], uv_recipe_raw),
                                                      (other, recipe_leaves[1], legacy_raw)]]
        for leaf, entries in [('empty.raceproject', []), ('one.raceproject', recipe_leaves[:1]), ('project.raceproject', recipe_leaves)]:
            created = invoke('race_mesh_import_tool.exe', '--create-project', reference, leaf, *entries)
            check('mixed project raw pin '+leaf, created['index_revision'] == sha((reference/leaf).read_bytes()) and created['entries'] == len(entries))
            check('mixed project independent binary bytes '+leaf, (reference/leaf).read_bytes() == binary_index(binary_references[:len(entries)]))
        index_raw = (reference/'project.raceproject').read_bytes()
        for mode, middle, stage in [('--recipe', [recipe_leaves[0]], 'recipe-stage'), ('--project', ['project.raceproject', identity], 'project-stage')]:
            imported = invoke('race_mesh_import_tool.exe', mode, reference, *middle, source, reference_assets, out/stage)
            check(mode+' real worker exact UV import', imported['leaf'] == direct['leaf'] and (reference_assets/imported['leaf']).read_bytes() == (out/'imported'/direct['leaf']).read_bytes())
            check(mode+' trusted worker source snapshot', (out/stage/'input.obj').read_bytes() == second_source and (out/stage/'result.rasset').read_bytes() == (out/'imported'/direct['leaf']).read_bytes())
        legacy = invoke('race_mesh_import_tool.exe', '--project', reference, 'project.raceproject', other, source, reference_assets, out/'legacy-stage')
        check('mixed project keeps schema1 geometry', struct.unpack_from('<I', (reference_assets/legacy['leaf']).read_bytes(), 20)[0] == 1)
        before = snapshot(reference_assets)
        invoke('race_mesh_import_tool.exe', '--create-recipe', reference, 'wrong-profile.json', identity, 'b.obj')
        invoke('race_mesh_import_tool.exe', '--recipe', reference, 'wrong-profile.json', source, reference_assets, out/'wrong-geometry-stage', success=False)
        (source/'b.obj').write_bytes(plain)
        invoke('race_mesh_import_tool.exe', '--recipe', reference, recipe_leaves[0], source, reference_assets, out/'wrong-uv-stage', success=False)
        (source/'b.obj').write_bytes(second_source)
        check('both source/profile mismatches preserve published products', snapshot(reference_assets) == before)
        for n, changes in enumerate([{'schema': 1}, {'mesh_schema': 1}, {'settings': expected_legacy['settings']}, {'cooker': 'f'*64}]):
            definition = dict(expected_uv, **changes)
            (reference/'invalid.json').write_text(json.dumps(definition), encoding='utf-8')
            stage = out/f'invalid-recipe-{n}'
            invoke('race_mesh_import_tool.exe', '--recipe', reference, 'invalid.json', source, reference_assets, stage, success=False)
            check('recipe mismatch rejected before staging '+str(n), not stage.exists() and snapshot(reference_assets) == before)

        leaf_b, manifest_b = manifest(second, second_source)
        records, statuses = watch('watch-b', '--checkpoint', checkpoints, '--coalesce')
        check('real UV watch publishes and captures B', any(s['outcome'] == 3 and s['current_revision'] == second['revision'] for s in statuses) and records[-1]['leaf'] == leaf_b and (checkpoints/leaf_b).read_bytes() == manifest_b)
        check('fresh retained UV watch stage', (out/'watch-b'/'attempt-1'/'input.obj').read_bytes() == second_source)
        loaded = invoke('race_mesh_import_tool.exe', '--load-checkpoint', reference, 'project.raceproject', reference_assets, checkpoints, leaf_b)
        check('UV checkpoint restore exact revision', loaded['cooked_revision'] == second['revision'] and loaded['source'] == sha(second_source))
        records, statuses = watch('resume-same', '--resume-checkpoint', checkpoints, leaf_b, '--coalesce', '--checkpoint', checkpoints, duration=500)
        check('restored UV source coalesces without worker', all(s['attempts'] == 0 for s in statuses) and statuses[-1]['unchanged'] >= 1 and records[-1]['leaf'] == leaf_b and not list((out/'resume-same').glob('attempt-*')))
        (source/'b.obj').write_bytes(third_source)
        _, third = mesh('b.obj', reference_assets)
        c_geometry = geometry((reference_assets/third['leaf']).read_bytes())
        check('B to C changes only UV geometry attributes', b_geometry[:2] == c_geometry[:2] and b_geometry[2] != c_geometry[2])
        leaf_c, manifest_c = manifest(third, third_source)
        records, statuses = watch('resume-change', '--resume-checkpoint', checkpoints, leaf_b, '--coalesce', '--checkpoint', checkpoints)
        check('UV-only reimport replaces revision under same asset', any(s['outcome'] == 3 and s['current_revision'] == third['revision'] for s in statuses) and records[-1]['leaf'] == leaf_c and (checkpoints/leaf_c).read_bytes() == manifest_c)
        check('old checkpoint remains immutable', (checkpoints/leaf_b).read_bytes() == manifest_b)
        # A structurally valid native product with legacy fingerprints must not
        # satisfy the UV recipe even when checksum, full revision and ID match.
        invalid = bytearray((reference_assets/second['leaf']).read_bytes())
        for offset, value in [(112, expected_legacy['settings']), (144, expected_legacy['importer']), (176, expected_legacy['cooker'])]:
            invalid[offset:offset+32] = bytes.fromhex(value)
        invalid[-32:] = hashlib.sha256(invalid[:-32]).digest()
        invalid_product = dict(second, revision=sha(invalid), leaf=sha(invalid)+'.rasset')
        (reference_assets/invalid_product['leaf']).write_bytes(invalid)
        invoke('race_mesh_tool.exe', 'validate', reference_assets, invalid_product['leaf'])
        invalid_leaf, invalid_manifest = manifest(invalid_product, second_source)
        (checkpoints/invalid_leaf).write_bytes(invalid_manifest)
        prior_products, prior_checkpoints = snapshot(reference_assets), snapshot(checkpoints)
        invoke('race_mesh_import_tool.exe', '--load-checkpoint', reference, 'project.raceproject', reference_assets, checkpoints, invalid_leaf, success=False)
        watch('invalid-resume', '--resume-checkpoint', checkpoints, invalid_leaf, success=False, duration=100)
        check('profile rejection precedes restore worker and preserves outputs', not (out/'invalid-resume').exists() and snapshot(reference_assets) == prior_products and snapshot(checkpoints) == prior_checkpoints)
        (source/'b.obj').write_bytes(second_source)

        if not args.native_only:
            # Asymmetric color proves a UV-only edit through the genuine editor
            # reimport/checkpoint/adoption flow, with no geometry movement.
            (source/'color.ppm').write_bytes(b'P6\n3 2\n255\n'+bytes([255,0,0, 0,255,0, 0,0,255, 128,128,128, 255,255,0, 0,255,255]))
            imported = invoke('race_surface_tool.exe', 'texture', source, 'color.ppm', out/'imported', '00000000000000020000000000000001', 'srgb')
            surface_schema = 2 if args.mip_surfaces else 1
            texture = invoke('race_surface_tool.exe', 'cook-mips' if args.mip_surfaces else 'cook', out/'imported', imported['leaf'], assets)
            material_json = dict(schema=surface_schema, model='opaque-diffuse', base_color=[1, 1, 1], base_color_texture=dict(asset=texture['id'], revision=texture['revision']))
            if args.mip_surfaces:
                material_json['sampling'] = 'linear-clamp-mips'
            (source/'material.json').write_text(json.dumps(material_json), encoding='utf-8')
            imported = invoke('race_surface_tool.exe', 'material', source, 'material.json', out/'imported', '00000000000000030000000000000001')
            material = invoke('race_surface_tool.exe', 'cook', out/'imported', imported['leaf'], assets)
            if args.mip_surfaces:
                texture_bytes = (assets/texture['leaf']).read_bytes()
                material_bytes = (assets/material['leaf']).read_bytes()
                check('explicit filtered products retain complete mip chain and sampler',
                      struct.unpack_from('<I', texture_bytes, 20)[0] == 2 and
                      struct.unpack_from('<IIII', texture_bytes, 240) == (3, 2, 1, 2) and
                      len(texture_bytes) == 256+24+4+32 and
                      struct.unpack_from('<I', material_bytes, 20)[0] == 2 and
                      struct.unpack_from('<I', material_bytes, 248)[0] == 2)
            check('editor authoring starts with absent project and candidate', not any((out/'recipes').iterdir()) and not (assets/second['leaf']).exists() and not (out/'checkpoints'/leaf_b).exists())
            # CLI and editor now share the canonical binary index pins. The
            # retained native checkpoint provides the same independent raw oracle.
            check('binary project checkpoint matches CLI capture', (checkpoints/leaf_b).read_bytes() == manifest_b)
            qt = out/'qt-authoring'
            run('race_editor.exe', '--assets', assets, '--mesh-product', first['leaf'], '--material-product', material['leaf'],
                '--checkpoint-project', out/'recipes'/'project.raceproject', '--checkpoint-file', out/'checkpoints'/leaf_b, '--authoring-uv-smoke', qt)
            receipt = read(qt/'checkpoint-receipt.json'); admit_application('Qt authoring/adoption', receipt)
            check('Qt mixed authoring and adoption checks', receipt['passed'] and all(item['passed'] for item in receipt['checks']) and bool(receipt.get('authoring_checks')) and all(item['passed'] for item in receipt['authoring_checks']))
            successful = [item for item in receipt.get('authoring_measurements', []) if item['passed']]
            check('Qt explicit mixed-profile publications', [item['profile'] for item in successful] == ['geometry', 'geometry-uv', 'geometry'] and
                  [item['entries'] for item in successful] == [0, 1, 2] and
                  [item['source_bytes'] for item in successful] == [0, len(second_source), len(plain)])
            check('Qt D3D12 hardware and diagnostics', receipt['hardware'] and receipt['debug_layer'] and receipt['debug_errors'] == receipt['debug_warnings'] == 0)
            for leaf in ['empty.raceproject', 'one.raceproject', 'project.raceproject', *recipe_leaves]:
                check('Qt exact immutable mixed closure '+leaf, (out/'recipes'/leaf).read_bytes() == (reference/leaf).read_bytes())
            check('Qt publishes exact project-derived UV checkpoint', (out/'checkpoints'/leaf_b).read_bytes() == manifest_b and (assets/second['leaf']).read_bytes() == (reference_assets/second['leaf']).read_bytes())
            for left, right in [('before.ppm', 'undo.ppm'), ('updated.ppm', 'redo.ppm'), ('updated.ppm', 'loose-control.ppm')]:
                check('Qt textured history '+left+' '+right, (qt/left).read_bytes() == (qt/right).read_bytes())
            check('UV-only replacement visibly changes textured pixels', (qt/'before.ppm').read_bytes() != (qt/'updated.ppm').read_bytes())
            verify_scene_adoption(read(qt/'authored.scene.json'), read(qt/'updated.scene.json'),
                                  pin_fields(first), pin_fields(second), pin_fields(material), check)
            sdl = out/'sdl-updated'
            run('race_runtime_desktop.exe', '--assets', assets, '--scene', qt/'updated.rscene', '--mesh-smoke', sdl)
            receipt = read(sdl/'mesh-receipt.json'); admit_application('SDL adopted UV scene', receipt)
            check('SDL D3D12 hardware and diagnostics', receipt['passed'] and receipt['hardware'] and receipt['debug_layer'] and receipt['debug_errors'] == receipt['debug_warnings'] == 0)
            check('Qt SDL adopted material pixels exact', (sdl/'scene.ppm').read_bytes() == (qt/'updated.ppm').read_bytes())
            check('Qt SDL adopted Scene bytes exact', (sdl/'mesh.rscene').read_bytes() == (qt/'updated.rscene').read_bytes())
            check('SDL Play keeps pinned material Scene', (sdl/'after-play.rscene').read_bytes() == (sdl/'mesh.rscene').read_bytes())
            check('same material and texture shared by both instances', len(receipt['materials']) == len(receipt['textures']) == 1)
            if args.mip_surfaces:
                check('SDL retains resolved filtering and full chain after UV adoption',
                      receipt['materials'][0]['sampling'] == 'linear-clamp-mips' and
                      receipt['textures'][0]['mip_count'] == 2 and receipt['textures'][0]['texel_bytes'] == 28)
            packages = out/'packages'; packages.mkdir()
            shutil.copyfile(qt/'updated.rscene', assets/'scene.rscene')
            package = invoke('race_package_tool.exe', 'pack', assets, 'scene.rscene', packages, '00000000000000040000000000000001')
            packed = packages/package['leaf']; encoded = packed.read_bytes()
            check('package exact content revision and retained UV closure', package['sha256'] == sha(encoded) and
                  package['leaf'] == sha(encoded)+'.rpak' and package['meshes'] == 2 and
                  package['materials'] == package['textures'] == 1)
            count, edges = struct.unpack_from('<II', encoded, 24)
            kinds = [struct.unpack_from('<II', encoded, 64+n*112+48) for n in range(count)]
            check('package advertises both UV revisions and exact surface closure', sorted(kinds) == [(1, 2), (1, 2), (2, 1), (3, surface_schema), (4, surface_schema)] and edges == 4)
            for label, application in [('Qt', 'race_editor.exe'), ('SDL', 'race_runtime_desktop.exe')]:
                destination = out/(label.lower()+'-package')
                run(application, '--package', packed, '--mesh-smoke', destination)
                receipt = read(destination/'mesh-receipt.json'); admit_application(label+' packaged UV scene', receipt)
                check(label+' package D3D12 hardware and diagnostics', receipt['passed'] and receipt['hardware'] and
                      receipt['debug_layer'] and receipt['debug_errors'] == receipt['debug_warnings'] == 0)
                for leaf in ['scene.ppm', 'play.ppm', 'cube-control.ppm', 'mesh.scene.json', 'mesh.rscene', 'after-play.rscene']:
                    check(label+' exact loose/package '+leaf, (destination/leaf).read_bytes() == (sdl/leaf).read_bytes())
                check(label+' package retains both mesh revisions and shared surfaces', len(receipt['meshes']) == 2 and
                      len(receipt['materials']) == len(receipt['textures']) == 1)
                if args.mip_surfaces:
                    check(label+' package retains resolved mip profile', receipt['materials'][0]['sampling'] == 'linear-clamp-mips' and
                          receipt['textures'][0]['mip_count'] == 2 and receipt['textures'][0]['texel_bytes'] == 28)
        passed = True
    finally:
        # Keep the primary failure and always write its receipt. Missing,
        # unreadable or changed executables fail a previously successful run.
        for path in binaries_before:
            try:
                binaries_after[path] = file_sha256(Path(path))
            except OSError as error:
                binary_errors[path] = str(error)
        unchanged = bool(binaries_before) and binaries_before == binaries_after and not binary_errors
        checks.append(dict(check='all exercised executable bytes unchanged', passed=unchanged))
        passed = passed and unchanged
        (out/'receipt.json').write_text(json.dumps(dict(schema=1, passed=passed, native_only=args.native_only, mip_surfaces=args.mip_surfaces,
            checks=checks, calls=calls, applications=app_receipts, binaries=binaries_after,
            expected_identity=expected_identity, identity_source=identity_source,
            executable_identities=executable_identities, binaries_before=binaries_before,
            binaries_after=binaries_after, binary_errors=binary_errors,
            scope='Explicit profile worker import, immutable mixed project/checkpoint restore, UV-only revisions and watch coalescing.'+
                  ('' if args.native_only else ' Actual Qt authoring/adoption/history, SDL D3D12 pixels and exact Qt/SDL loose/package parity; synthetic input, not physical input latency.')), indent=2), encoding='utf-8')
    print(json.dumps(dict(passed=passed, checks=len(checks), application_runs=len(app_receipts), receipt=str(out/'receipt.json'))))
    if not passed:
        raise RuntimeError('Executable bytes changed during UV validation; see receipt')


if __name__ == '__main__':
    main()

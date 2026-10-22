"""Native surface authoring, Qt assignment, SDL and exact package GPU parity."""
from test_content_project import binary_index, parse_binary_index
from output_paths import evidence_directory
from test_surface_import import native_wire, scalar_material_wire
from source_manifest import authored_inputs, file_sha256, git_state
from verify_desktop_uv_recipes import application_identity, parse_cli_identity, validated_identity
import argparse
import copy
import csv
import ctypes
import hashlib
import json
import io
from pathlib import Path
import shutil
import struct
import subprocess
import time


def native_guard(output: Path):
    """Recheck the owner's Windows resource/serialization limits at each launch."""
    class Memory(ctypes.Structure):
        _fields_ = [('length', ctypes.c_uint32), ('load', ctypes.c_uint32),
                    *[(name, ctypes.c_uint64) for name in
                      ('total', 'available', 'page_total', 'page_available',
                       'virtual_total', 'virtual_available', 'extended')]]
    memory = Memory()
    memory.length = ctypes.sizeof(memory)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(memory)):
        raise ctypes.WinError()
    processes = subprocess.run(['tasklist.exe', '/FO', 'CSV', '/NH'], capture_output=True,
                               text=True, errors='replace', timeout=15, check=True)
    blocked = {'claude.exe', 'cl.exe', 'link.exe', 'ninja.exe', 'cmake.exe', 'ctest.exe', 'dxc.exe'}
    busy = [row[:2] for row in csv.reader(io.StringIO(processes.stdout)) if row and
            (row[0].lower() in blocked or row[0].lower().startswith('race'))]
    free_disk = shutil.disk_usage(output).free
    return dict(passed=memory.available >= 2*1024**3 and free_disk >= 10*1024**3 and not busy,
                free_physical_bytes=memory.available, minimum_physical_bytes=2*1024**3,
                disk_free_bytes=free_disk, minimum_disk_bytes=10*1024**3, busy=busy,
                recorded_unix_seconds=time.time())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--tools', type=Path, help='Fixture CLI directory when testing an extracted package')
    parser.add_argument('--bmp-source', action='store_true', help='Import the native texture from the bounded BMP24 source profile')
    parser.add_argument('--editor-content', action='store_true', help='Exercise native typed recipe dialogs, surface capture and explicit Content adoption')
    parser.add_argument('--surface-recipes', action='store_true', help='Author pinned surface recipes and a typed content catalog; load recipe products through Qt/SDL')
    parser.add_argument('--mip-surfaces', action='store_true', help='Opt-in complete cooked mip chains and filtered materials')
    parser.add_argument('--scalar-material', action='store_true', help='Exercise linear R diffuse reflectance through recipes, loose loading and packages')
    parser.add_argument('--expected-source-commit')
    parser.add_argument('--expected-authored-input-sha256')
    args = parser.parse_args()
    if args.editor_content and not (args.surface_recipes and args.bmp_source and args.mip_surfaces):
        parser.error('--editor-content requires --surface-recipes --bmp-source --mip-surfaces')
    if args.scalar_material and (not args.mip_surfaces or args.editor_content):
        parser.error('--scalar-material requires --mip-surfaces and uses the standalone material path; Content smoke covers both roles')
    if (args.expected_source_commit is None) != (args.expected_authored_input_sha256 is None):
        parser.error('Provide both expected identity arguments or neither')
    build, out = args.build.resolve(), evidence_directory('desktop-surfaces', args.output)
    tools = (args.tools or build).resolve()
    tool_names = {'race_mesh_tool.exe', 'race_surface_tool.exe'}
    cli_names = ['race_mesh_tool.exe', 'race_surface_tool.exe', 'race_package_tool.exe']
    if args.surface_recipes:
        # Fixture mesh tools may come from --tools; exercise the selected package's
        # actual surface/content executables for both direct and recipe workflows.
        tool_names.remove('race_surface_tool.exe')
        cli_names.append('race_content_tool.exe')
    checks, calls, receipts, content_failures = [], [], [], []
    identities, binaries_before, binaries_after, binary_errors = [], {}, {}, {}
    expected_identity = None
    surface_products = {}
    recipe_receipts, content_catalog = [], None
    completed = False

    def check(name, value):
        checks.append({'check': name, 'passed': bool(value)})
        if not value:
            raise RuntimeError(name)

    def run(exe, *arguments, expected=0):
        path = ((tools if exe in tool_names else build) / exe).resolve()
        check('executable unchanged before launch '+str(path), file_sha256(path) == binaries_before[str(path)])
        guard = native_guard(out)
        write_json(out/f'launch-{len(calls)+1:03d}-guard.json', guard)
        check('resource/serialization guard before launch '+str(len(calls)+1), guard['passed'])
        command = [str(path), *map(str, arguments)]
        start = time.perf_counter()
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=90)
        except subprocess.TimeoutExpired as error:
            calls.append({'command': command, 'timeout': True, 'stdout': str(error.stdout), 'stderr': str(error.stderr)})
            raise
        calls.append({'command': command, 'seconds': time.perf_counter() - start,
                      'exit_code': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr})
        check(f'command {len(calls)} expected exit', result.returncode == expected)
        return result.stdout

    def read_json(path):
        return json.loads(path.read_text(encoding='utf-8-sig'))

    def write_json(path, value):
        path.write_text(json.dumps(value, indent=2), encoding='utf-8')

    def cli_json_identity(name, receipt):
        check(name+' exact expected source identity',
              validated_identity(receipt.get('source_commit'), receipt.get('authored_sha256')) == expected_identity)

    def file_snapshot(root):
        return {str(p.relative_to(root)): file_sha256(p) for p in sorted(root.rglob('*')) if p.is_file()}

    def surface(mode, name, identity, encoding=None):
        arguments = [mode, out/'source', name, out/'imported', identity]
        if encoding:
            arguments.append(encoding)
        imported = json.loads(run('race_surface_tool.exe', *arguments))
        cook_mode = ('cook-scalar-mips' if args.scalar_material else 'cook-mips') if args.mip_surfaces and mode == 'texture' else 'cook'
        direct_assets = out/('oracle-assets' if args.surface_recipes else 'assets')
        cooked = json.loads(run('race_surface_tool.exe', cook_mode, out/'imported', imported['leaf'], direct_assets))
        repeated = json.loads(run('race_surface_tool.exe', cook_mode, out/'imported', imported['leaf'], direct_assets))
        check(name+' deterministic cooking/publication', cooked == repeated)
        check(name+' full revision identity', hashlib.sha256((direct_assets/cooked['leaf']).read_bytes()).hexdigest() == cooked['revision'])
        if not args.surface_recipes:
            return imported, cooked
        cli_json_identity(name+' direct import', imported)
        cli_json_identity(name+' direct cook', cooked)
        profile = ('bmp' if args.bmp_source else 'ppm')+('-scalar' if args.scalar_material else '-srgb') if mode == 'texture' else ('material-scalar' if args.scalar_material else 'material')
        if args.mip_surfaces:
            profile += '-mips'
        recipe_leaf = mode+'-recipe.json'
        pin = read_json(out/'source'/name)['base_color_texture'] if mode == 'material' else None
        creation_args = ['create-recipe', out/'recipes', recipe_leaf, identity, name, profile]
        if pin:
            creation_args += [pin['asset'], pin['revision']]
        created = json.loads(run('race_surface_tool.exe', *creation_args))
        raw = (out/'recipes'/recipe_leaf).read_bytes()
        recipe_revision = hashlib.sha256(raw).hexdigest()
        imported_bytes = (out/'imported'/imported['leaf']).read_bytes()
        cooked_bytes = (direct_assets/cooked['leaf']).read_bytes()
        # Pin every compiled tuple field to the separately cooked native products.
        definition = dict(schema=1, asset=identity, source=name, profile=profile,
                          asset_container=1, asset_schema=cooked['schema'], target=1,
                          import_settings=imported_bytes[112:144].hex(), settings=cooked_bytes[112:144].hex(),
                          importer=imported_bytes[144:176].hex(), cooker=cooked_bytes[176:208].hex(),
                          dependencies=cooked_bytes[208:240].hex(), texture_asset=pin['asset'] if pin else '0'*32,
                          texture_revision=pin['revision'] if pin else '0'*64)
        check(name+' exact canonical recipe tuple and dependency pin',
              raw == (json.dumps(definition, separators=(',', ':'))+'\n').encode('ascii'))
        cli_json_identity(name+' recipe creation', created)
        check(name+' complete recipe creation receipt', created == dict(format='RACESURFACE_RECIPE1', schema=1,
              id=identity, source=name, recipe_profile=profile, recipe_leaf=recipe_leaf, recipe_revision=recipe_revision,
              source_commit=expected_identity['source_commit'], authored_sha256=expected_identity['authored_input_sha256']))
        recipes_before = file_snapshot(out/'recipes')
        check(name+' repeated recipe creation receipt', json.loads(run('race_surface_tool.exe', *creation_args)) == created)
        check(name+' repeated recipe creation immutable', file_snapshot(out/'recipes') == recipes_before)
        execution_args = ['recipe', out/'recipes', recipe_leaf, out/'source', out/'assets']
        executed = json.loads(run('race_surface_tool.exe', *execution_args))
        cli_json_identity(name+' recipe execution', executed)
        check(name+' complete recipe execution receipt', executed == dict(cooked, recipe_revision=recipe_revision,
              recipe_profile=profile, source=hashlib.sha256((out/'source'/name).read_bytes()).hexdigest(),
              imported_revision=imported['revision'], imported_leaf=imported['leaf']))
        check(name+' entire imported recipe product equals direct oracle',
              (out/'assets'/executed['imported_leaf']).read_bytes() == imported_bytes)
        check(name+' entire cooked recipe product equals direct oracle',
              (out/'assets'/executed['leaf']).read_bytes() == cooked_bytes)
        products_before = file_snapshot(out/'assets')
        check(name+' repeated recipe execution receipt', json.loads(run('race_surface_tool.exe', *execution_args)) == executed)
        check(name+' repeated recipe execution immutable', file_snapshot(out/'assets') == products_before
              and file_snapshot(out/'recipes') == recipes_before)
        recipe_receipts.append(dict(kind=mode, created=created, executed=executed, direct_imported=imported, direct_cooked=cooked))
        return imported, executed

    def surface_catalog():
        references, rows, arguments = [], [], ['create', out/'recipes', 'surfaces.raceproject']
        for record in recipe_receipts:
            kind, created = record['kind'], record['created']
            references.append(dict(asset=created['id'], kind=kind, recipe=created['recipe_leaf'], revision=created['recipe_revision']))
            rows.append(dict(kind=kind, id=created['id'], source=created['source'], recipe=created['recipe_leaf'],
                             recipe_revision=created['recipe_revision'], profile=created['recipe_profile']))
            arguments += [kind, created['id'], created['recipe_leaf'], created['recipe_revision']]
        check('catalog contains exactly texture and material recipe pins',
              len(references) == 2 and [r['kind'] for r in references] == ['texture', 'material'])
        catalog = json.loads(run('race_content_tool.exe', *arguments))
        raw = (out/'recipes'/'surfaces.raceproject').read_bytes()
        expected = binary_index(references)
        check('typed surface catalog exact raw index', raw == expected)
        cli_json_identity('typed surface catalog', catalog)
        check('typed surface catalog exact metadata and full rows', catalog['format'] == 'RACECONTENT_PROJECT'
              and catalog['encoding'] == 'binary' and catalog['wire_version'] == 1 and catalog['schema'] == 2 and catalog['leaf'] == 'surfaces.raceproject' and catalog['entries'] == 2
              and catalog['revision'] == hashlib.sha256(raw).hexdigest() and catalog['recipes'] == rows
              and 0 < catalog['retained_bytes'] <= 256*1024)
        before = file_snapshot(out/'recipes')
        check('typed surface catalog repeated immutable publication',
              json.loads(run('race_content_tool.exe', *arguments)) == catalog and file_snapshot(out/'recipes') == before)
        check('typed surface catalog separate native inspection',
              json.loads(run('race_content_tool.exe', 'inspect', out/'recipes', 'surfaces.raceproject')) == catalog)
        return catalog

    def smoke(name, app, scene=None, package=None, mesh=None, material=None, assets=None, expected_textured=True):
        destination = out/name
        arguments = ['--content-smoke' if args.editor_content and name == 'qt-assign' else '--mesh-smoke', destination]
        if scene:
            arguments += ['--scene', scene]
        if package:
            arguments += ['--package', package]
        else:
            arguments += ['--assets', assets or out/'assets']
        if mesh:
            arguments += ['--mesh-product', mesh['leaf']]
        if material:
            arguments += ['--material-product', material['leaf']]
        run('race_editor.exe' if app == 'Qt' else 'race_runtime_desktop.exe', *arguments)
        receipt = read_json(destination/'mesh-receipt.json')
        receipts.append({'name': name, **receipt})
        check(name+' exact expected source identity', application_identity(receipt) == expected_identity)
        check(name+' hardware and debug', receipt['passed'] and receipt['hardware'] and receipt['debug_layer']
              and receipt['debug_errors'] == receipt['debug_warnings'] == 0)
        check(name+' Play isolation', (destination/'mesh.rscene').read_bytes() == (destination/'after-play.rscene').read_bytes())
        check(name+' expected retained surface counts',
              len(receipt['materials']) == len(receipt['textures']) == int(expected_textured))
        if expected_textured:
            for key, field in [('material', 'materials'), ('texture', 'textures')]:
                product = surface_products[key]
                check(name+' exact retained '+key+' pin', receipt[field][0]['asset'] == product['id'] and
                      receipt[field][0]['revision'] == product['revision'])
        if expected_textured:
            check(name+' resolved explicit texture role', all(m['texture_role'] == ('diffuse-reflectance-r' if args.scalar_material else 'color') for m in receipt['materials']))
            check(name+' resolved explicit texture encoding', all(t['encoding'] == ('linear-data' if args.scalar_material else 'srgb-color') for t in receipt['textures']))
        if args.mip_surfaces and expected_textured:
            check(name+' resolved filtered material profile', all(m['sampling'] == 'linear-clamp-mips' for m in receipt['materials']))
            check(name+' resolved full chain storage', all(t['mip_count'] == 2 and t['texel_bytes'] == 28 for t in receipt['textures']))
        return destination

    def parity(name, first, second):
        for leaf in ['scene.ppm', 'play.ppm', 'cube-control.ppm', 'mesh.rscene']:
            check(name+' '+leaf+' exact', (first/leaf).read_bytes() == (second/leaf).read_bytes())

    def content_failure(name, mesh_leaf, blocked_recipes=False):
        # Real filesystem failures, through the same UI/worker path as normal
        # authoring. No production fault switch or unchecked fixture shortcut.
        root = out/name
        root.mkdir()
        for folder in ['source', 'editor-checkpoints']:
            (root/folder).mkdir()
        sentinel = b'Preserved non-directory recipe-root fixture\n'
        if blocked_recipes:
            (root/'editor-recipes').write_bytes(sentinel)
        else:
            (root/'editor-recipes').mkdir()
        destination = root/'qt'
        run('race_editor.exe', '--content-smoke', destination, '--assets', out/'editor-products',
            '--mesh-product', mesh_leaf, expected=2)
        receipt = read_json(destination/'checkpoint-receipt.json')
        content_failures.append(dict(name=name, **receipt))
        check(name+' exact source', application_identity(receipt) == expected_identity)
        check(name+' reports failure without native renderer errors', not receipt['passed'] and
              receipt['hardware'] and receipt['debug_layer'] and
              receipt['debug_errors'] == receipt['debug_warnings'] == 0)
        failed = [entry for entry in receipt['checks'] if 'error' in entry or not entry.get('passed')]
        if blocked_recipes:
            check(name+' specific authoring failure only', len(failed) == 1 and
                  failed[0].get('error', '').startswith('Content authoring failed: '))
            check(name+' retains obstructing file', (root/'editor-recipes').read_bytes() == sentinel)
        else:
            check(name+' missing mesh fails before authoring', len(failed) == 2 and
                  'error' in failed[0] and failed[1] == dict(check='content_mesh_added', passed=False))
            check(name+' no project publication', not list((root/'editor-recipes').iterdir()))
        check(name+' no later recipe/scene steps', not (destination/'mesh.rscene').exists() and
              not any(entry.get('check') in {'content_binary_empty_project',
                      'content_texture_preserves_binary_index', 'content_material_adopted'}
                      for entry in receipt['checks']))

    def pin_fields(reference):
        digest = bytes.fromhex(reference['revision'])
        return [{'id': 1, 'kind': 'asset', 'value': reference['id']}, *[
            {'id': n+2, 'kind': 'asset', 'value': ''.join(f'{int.from_bytes(digest[i:i+8], "little"):016x}' for i in [n*16, n*16+8])}
            for n in range(2)]]

    def reject_scene(name, scene, assets):
        for app in ['race_editor.exe', 'race_runtime_desktop.exe']:
            run(app, '--scene', scene, '--assets', assets, '--mesh-smoke', out/(name+'-'+app), expected=1)

    try:
        expected_identity = validated_identity(args.expected_source_commit, args.expected_authored_input_sha256) if args.expected_source_commit else validated_identity(git_state()['commit'], authored_inputs()['sha256'])
        for name in [*cli_names, 'race_editor.exe', 'race_runtime_desktop.exe']:
            path = ((tools if name in tool_names else build)/name).resolve()
            check('required executable '+str(path), path.is_file())
            binaries_before[str(path)] = file_sha256(path)
        for name in cli_names:
            identity = parse_cli_identity(run(name, '--identity'))
            identities.append(dict(executable=name, **identity))
            check(name+' exact expected source identity', identity == expected_identity)
        for folder in ['source', 'imported', 'assets', 'packages', 'missing', 'malformed']:
            (out/folder).mkdir()
        if args.surface_recipes:
            for folder in ['oracle-assets', 'recipes']:
                (out/folder).mkdir()
        uv_obj = ('v 1 0 0\nv 3 0 0\nv 2 2 0\nv 2 0 2\n'
                  'vt 0 0\nvt 1 0\nvt 0.5 1\n'
                  'f 1/1 3/3 2/2\nf 1/1 2/2 4/3\nf 2/1 3/2 4/3\nf 3/1 1/2 4/3\n')
        (out/'source'/'mapped.obj').write_text(uv_obj, encoding='ascii')
        imported_mesh = json.loads(run('race_mesh_tool.exe', 'import', out/'source', 'mapped.obj', out/'imported', '00000000000000010000000000000001'))
        mesh = json.loads(run('race_mesh_tool.exe', 'cook', out/'imported', imported_mesh['leaf'], out/'assets'))
        blob = (out/'assets'/mesh['leaf']).read_bytes()
        check('native UV mesh explicit schema and stride', struct.unpack_from('<I', blob, 20)[0] == 2 and struct.unpack_from('<I', blob, 240)[0] == 64)
        # Odd width, asymmetric color, gray and leading raster whitespace bytes.
        rgb = bytes([10,32,9, 255,0,0, 0,255,0, 0,0,255, 128,128,128, 255,255,0])
        if args.bmp_source:
            # Independent source fixture: bottom-up BGR, nonzero row padding.
            raster = b''.join(b''.join(rgb[i:i+3][::-1] for i in range(row*9, row*9+9, 3))+b'\xa5'*3
                              for row in [1, 0])
            source = struct.pack('<2sIHHI', b'BM', 54+len(raster), 0, 0, 54)
            source += struct.pack('<IiiHHIIiiII', 40, 3, 2, 1, 24, 0, len(raster), 0, 0, 0, 0)+raster
            texture_name = 'color.bmp'
        else:
            source = b'P6\n3 2\n255\n'+rgb
            texture_name = 'color.ppm'
        (out/'source'/texture_name).write_bytes(source)
        if args.editor_content:
            (out/'source'/'scalar.ppm').write_bytes(b'P6\n2 1\n255\n'+bytes([0,10,20,255,30,40]))
        imported_texture, texture = surface('texture', texture_name, '00000000000000020000000000000001', 'linear' if args.scalar_material else 'srgb')
        imported_blob = (out/'imported'/imported_texture['leaf']).read_bytes()
        check('source texture exact RGB orientation and opaque alpha', imported_blob[256:-32] ==
              b''.join(rgb[i:i+3]+b'\xff' for i in range(0, len(rgb), 3)))
        check('source texture complete original byte fingerprint', imported_blob[80:112] == hashlib.sha256(source).digest())
        if args.bmp_source:
            check('BMP source importer fingerprint', imported_blob[144:176] ==
                  hashlib.sha256(b'race-bmp-bgr24-info40-importer-1').digest())
            check('BMP source settings fingerprint', imported_blob[112:144] ==
                  hashlib.sha256(('race-bmp-1:rgba8;top-left;'+('linear-data' if args.scalar_material else 'srgb')+';opaque').encode()).digest())
        surface_schema = 3 if args.scalar_material else 2 if args.mip_surfaces else 1
        definition = {'schema': surface_schema, 'model': 'opaque-diffuse', 'base_color': [1, 1, 1],
                      'base_color_texture': {'asset': texture['id'], 'revision': texture['revision']}}
        if args.mip_surfaces:
            definition['sampling'] = 'linear-clamp-mips'
        if args.scalar_material:
            definition['texture_role'] = 'diffuse-reflectance-r'
        write_json(out/'source'/'material.json', definition)
        _, material = surface('material', 'material.json', '00000000000000030000000000000001')
        surface_products.update(material=material, texture=texture)
        if args.surface_recipes:
            content_catalog = surface_catalog()
        if args.editor_content:
            # Derive the exact future BMP scalar pin independently, before UI import.
            expected_scalar = bytearray(native_wire(source, 'bmp', 3, 2, rgb, encoding=2, stage='cook-scalar-mips'))
            struct.pack_into('<QQ', expected_scalar, 64, 5, 1)
            expected_scalar[-32:] = hashlib.sha256(expected_scalar[:-32]).digest()
            scalar_pin = dict(asset='00000000000000050000000000000001', revision=hashlib.sha256(expected_scalar).hexdigest())
            write_json(out/'source'/'scalar-material.json', dict(schema=3, model='opaque-diffuse', base_color=[1, 0.25, 0.1], base_color_texture=scalar_pin, sampling='linear-clamp-mips', texture_role='diffuse-reflectance-r'))
            for folder in ['editor-recipes', 'editor-checkpoints', 'editor-products']:
                (out/folder).mkdir()
            shutil.copyfile(out/'assets'/mesh['leaf'], out/'editor-products'/mesh['leaf'])
            check('editor starts without surface products or recipes', len(list((out/'editor-products').iterdir())) == 1 and not list((out/'editor-recipes').iterdir()))
            content_failure('content-missing-mesh', 'missing.rasset')
            content_failure('content-project-failure', mesh['leaf'], blocked_recipes=True)
        authored = smoke('qt-assign', 'Qt', mesh=mesh, material=material, assets=out/'editor-products' if args.editor_content else None)
        if args.editor_content:
            for product in (out/'assets').iterdir():
                check('editor produced exact oracle product '+product.name, (out/'editor-products'/product.name).read_bytes() == product.read_bytes())
            check('native Content import and adoption metrics', len(receipts[-1]['content_metrics']) == 8 and [m['operation'] for m in receipts[-1]['content_metrics']] == ['import', 'import', 'import', 'adopt', 'import', 'import', 'import', 'adopt'])
            scalar_raw = (out/'editor-recipes'/'scalar-bmp.raceproject').read_bytes()
            scalar_index = parse_binary_index(scalar_raw)
            check('scalar index exact binary oracle', scalar_raw == binary_index(scalar_index['recipes']))
            check('scalar additions preserve four typed recipes', scalar_index['schema'] == 2 and len(scalar_index['recipes']) == 4)
            for identity, profile in [('00000000000000040000000000000001', 'ppm-scalar-mips'), ('00000000000000050000000000000001', 'bmp-scalar-mips')]:
                row = next(r for r in scalar_index['recipes'] if r['asset'] == identity)
                raw = (out/'editor-recipes'/row['recipe']).read_bytes()
                recipe = json.loads(raw)
                check(profile+' native authored raw pin', row['kind'] == 'texture' and hashlib.sha256(raw).hexdigest() == row['revision'] and recipe['profile'] == profile and recipe['asset_schema'] == 3)
                captures = [read_json(p) for p in (out/'editor-checkpoints').glob('*.json')]
                captured = next(c for c in captures if c.get('asset') == identity)
                product = (out/'editor-products'/(captured['cooked_revision']+'.rasset')).read_bytes()
                scalar_ppm = profile.startswith('ppm')
                scalar_rgb = bytes([0,10,20,255,30,40]) if scalar_ppm else rgb
                scalar_source = (out/'source'/recipe['source']).read_bytes()
                expected = bytearray(native_wire(scalar_source, 'ppm' if scalar_ppm else 'bmp',
                                                 2 if scalar_ppm else 3, 1 if scalar_ppm else 2,
                                                 scalar_rgb, encoding=2, stage='cook-scalar-mips'))
                struct.pack_into('<QQ', expected, 64, 4 if scalar_ppm else 5, 1)
                expected[-32:] = hashlib.sha256(expected[:-32]).digest()
                check(profile+' exact independent native product', product == expected)
                check(profile+' native schema and encoding', struct.unpack_from('<I', product, 20)[0] == 3 and struct.unpack_from('<I', product, 248)[0] == 2 and hashlib.sha256(product).hexdigest() == captured['cooked_revision'])
            scalar_material_raw = (out/'editor-recipes'/'scalar-material.raceproject').read_bytes()
            scalar_material_index = parse_binary_index(scalar_material_raw)
            check('scalar material exact binary project oracle', scalar_material_raw == binary_index(scalar_material_index['recipes']) and len(scalar_material_index['recipes']) == 5)
            scalar_material_row = next(r for r in scalar_material_index['recipes'] if r['asset'] == '00000000000000060000000000000001')
            scalar_recipe_raw = (out/'editor-recipes'/scalar_material_row['recipe']).read_bytes()
            scalar_recipe = json.loads(scalar_recipe_raw)
            check('scalar material authored role and pin', scalar_material_row['kind'] == 'material' and hashlib.sha256(scalar_recipe_raw).hexdigest() == scalar_material_row['revision'] and scalar_recipe['profile'] == 'material-scalar-mips' and scalar_recipe['texture_asset'] == scalar_pin['asset'] and scalar_recipe['texture_revision'] == scalar_pin['revision'])
            scalar_capture = next(read_json(p) for p in (out/'editor-checkpoints').glob('*.json') if read_json(p).get('asset') == scalar_material_row['asset'])
            scalar_product = (out/'editor-products'/(scalar_capture['cooked_revision']+'.rasset')).read_bytes()
            scalar_expected = scalar_material_wire((out/'source'/'scalar-material.json').read_bytes(), (scalar_pin['asset'], scalar_pin['revision']), cooked=True, identity=scalar_material_row['asset'])
            check('scalar material exact independent native product', scalar_product == scalar_expected and hashlib.sha256(scalar_expected).hexdigest() == scalar_capture['cooked_revision'])
            typed_raw = (out/'editor-recipes'/'surfaces.raceproject').read_bytes()
            typed = parse_binary_index(typed_raw)
            check('typed editor index exact binary oracle', typed_raw == binary_index(typed['recipes']))
            check('native Content complete typed recipe index', typed['schema'] == 2 and len(typed['recipes']) == 2)
            for row in typed['recipes']:
                expected = next(r for r in recipe_receipts if r['created']['id'] == row['asset'])
                check('native authored exact recipe '+row['kind'], (out/'editor-recipes'/row['recipe']).read_bytes() == (out/'recipes'/expected['created']['recipe_leaf']).read_bytes())
        check('real editor material actions and worker', all(any(c.get('check') == name and c['passed'] for c in receipts[-1]['checks'])
              for name in (['content_import_worker_admitted', 'content_cancel_does_not_publish', 'content_texture_capture', 'content_material_capture_without_scene_mutation', 'content_adoption_worker_admitted', 'content_material_adopted', 'content_redo_exact_pin', 'content_texture_cannot_assign', 'content_scalar_ppm_capture', 'content_scalar_capture', 'content_scalar_cancel_retains_draft', 'content_scalar_keeps_color_pin', 'content_scalar_not_offered_as_color', 'content_scalar_status', 'content_color_not_offered_as_scalar', 'content_scalar_material_capture', 'content_scalar_material_adopted', 'content_scalar_undo_restores_color', 'content_scalar_redo_restores_pin'] if args.editor_content else ['material_worker_admitted', 'material_assigned', 'material_undo', 'material_redo_pin', 'material_clear', 'material_restored_pin', 'material_assignment_serializes_content'])))
        for app in ['Qt', 'SDL']:
            for suffix in ['scene.json', 'rscene']:
                loaded = smoke(app.lower()+'-'+suffix.replace('.', '-'), app, scene=authored/('mesh.'+suffix))
                parity(app+' '+suffix, authored, loaded)
        scene = read_json(authored/'mesh.scene.json')
        mapped = next(obj for obj in scene['objects'] if any(c['type'] == 3 for c in obj['components']))
        check('Scene pins exact material revision', next(c for c in mapped['components'] if c['type'] == 4)['fields'] == pin_fields(material))
        plain = copy.deepcopy(scene)
        for obj in plain['objects']:
            obj['components'] = [c for c in obj['components'] if c['type'] != 4]
        write_json(out/'plain.scene.json', plain)
        plain_run = smoke('qt-unassigned', 'Qt', scene=out/'plain.scene.json', expected_textured=False)
        check('texture visibly changes rendered pixels', (plain_run/'scene.ppm').read_bytes() != (authored/'scene.ppm').read_bytes())
        # Shared material/texture over two UV mesh instances resolves one revision.
        shared = copy.deepcopy(scene)
        duplicate = copy.deepcopy(mapped);duplicate['id']='52414345424f58010000000000000003'
        next(c for c in duplicate['components'] if c['type'] == 1)['fields'][0]['value'] = [-2, 0, 0]
        shared['objects'].append(duplicate);write_json(out/'shared.scene.json', shared)
        shared_qt=smoke('qt-shared', 'Qt', scene=out/'shared.scene.json')
        shared_sdl=smoke('sdl-shared', 'SDL', scene=shared_qt/'mesh.rscene')
        parity('shared texture', shared_qt, shared_sdl)
        check('shared CPU packets deduplicated',len(receipts[-1]['materials'])==len(receipts[-1]['textures'])==1)
        check('GPU upload reuse observed',0<receipts[-1]['texture_uploads']<=2 and receipts[-1]['texture_upload_bytes']>0 and receipts[-1]['texture_cache_hits']>=1)
        shutil.copyfile(shared_qt/'mesh.rscene', out/'assets'/'scene.rscene')
        package = json.loads(run('race_package_tool.exe', 'pack', out/'assets', 'scene.rscene', out/'packages', '00000000000000040000000000000001'))
        packed = out/'packages'/package['leaf']
        encoded = packed.read_bytes(); count, edges = struct.unpack_from('<II', encoded, 24)
        kinds = sorted(struct.unpack_from('<I', encoded, 64+n*112+48)[0] for n in range(count))
        check('exact four-kind deduplicated transitive closure', kinds == [1,2,3,4] and edges == 3)
        for n in range(count):
            at=64+n*112;kind,schema=struct.unpack_from('<II',encoded,at+48)
            check('package advertised schema '+str(n),schema==(2 if kind==1 else surface_schema if kind in (3,4) else 1))
        for app in ['Qt','SDL']:
            packed_run=smoke(app.lower()+'-packed',app,package=packed)
            parity(app+' loose/package',shared_qt,packed_run)
        for path in (out/'assets').glob('*.rasset'):
            if path.name != texture['leaf']:
                shutil.copyfile(path,out/'missing'/path.name)
        shutil.copyfile(shared_qt/'mesh.rscene',out/'missing'/'scene.rscene')
        reject_scene('missing-texture',authored/'mesh.rscene',out/'missing')
        run('race_package_tool.exe','pack',out/'missing','scene.rscene',out/'packages','00000000000000040000000000000001',expected=1)
        bad=copy.deepcopy(scene)
        binding=next(c for obj in bad['objects'] for c in obj['components'] if c['type']==4)
        binding['fields'][0]['value']='00000000000000030000000000000009'
        write_json(out/'wrong-material.scene.json',bad)
        reject_scene('wrong-material',out/'wrong-material.scene.json',out/'assets')
        bad=copy.deepcopy(scene);binding=next(c for obj in bad['objects'] for c in obj['components'] if c['type']==4)
        binding['fields']=pin_fields(texture);write_json(out/'wrong-kind.scene.json',bad)
        reject_scene('wrong-kind',out/'wrong-kind.scene.json',out/'assets')
        bad_material=dict(definition);bad_material['base_color_texture']={'asset': imported_texture['id'],'revision': imported_texture['revision']}
        write_json(out/'source'/'imported-binding.json',bad_material)
        imported=json.loads(run('race_surface_tool.exe','material',out/'source','imported-binding.json',out/'imported',material['id']))
        run('race_surface_tool.exe','cook',out/'imported',imported['leaf'],out/'imported',expected=1)
        corrupt=bytearray(encoded);corrupt[-1]^=1;(out/'malformed'/'corrupt.rpak').write_bytes(corrupt)
        run('race_package_tool.exe','validate',out/'malformed','corrupt.rpak',expected=1)
        check('one authored identity across actual app runs',len({r['authored_input_sha256'] for r in receipts})==1)
        completed=True
    finally:
        for path in binaries_before:
            try:
                binaries_after[path] = file_sha256(Path(path))
            except OSError as error:
                binary_errors[path] = str(error)
        unchanged = bool(binaries_before) and binaries_before == binaries_after and not binary_errors
        checks.append(dict(check='all exercised executable bytes unchanged', passed=unchanged))
        completed = completed and unchanged
        write_json(out/'verification.json', {'schema':1,'passed':completed and bool(checks) and all(c['passed'] for c in checks),
            'editor_content':args.editor_content, 'scalar_material':args.scalar_material, 'mip_surfaces':args.mip_surfaces, 'bmp_source':args.bmp_source, 'expected_identity':expected_identity, 'executable_identities':identities,
            'binaries_before':binaries_before, 'binaries_after':binaries_after, 'binary_errors':binary_errors,
            'checks':checks,'calls':calls,'receipts':receipts,'content_failures':content_failures,
            **(dict(surface_recipes=True, surface_recipe_receipts=recipe_receipts, content_catalog=content_catalog) if args.surface_recipes else {}),
            'artifacts':[{'path':str(p.relative_to(out)),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
                         for p in sorted(out.rglob('*')) if p.is_file() and p.suffix in ['.bmp','.ppm','.png','.rscene','.rpak','.rasset','.raceproject','.json','.obj']],
            'scope':'Bounded Windows D3D12 opaque diffuse native surface profile; not full PBR, streaming or performance qualification.'})
    if not completed:
        raise RuntimeError('Surface validation failed; see verification.json')
    print(json.dumps({'passed':True,'checks':len(checks),'application_runs':len(receipts),'receipt':str(out/'verification.json')}))


if __name__ == '__main__':
    main()

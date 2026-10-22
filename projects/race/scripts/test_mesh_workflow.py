"""Independent fresh-process OBJ import/cook/lease and immutable-file checks."""
import argparse
import hashlib
import json
import pathlib
import struct
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--tool', required=True)
args = parser.parse_args()

def invoke(*arguments, success=True):
    result = subprocess.run([args.tool, *map(str, arguments)], capture_output=True, text=True, timeout=30)
    if success:
        assert result.returncode == 0, (result.stdout, result.stderr)
        return json.loads(result.stdout)
    assert result.returncode != 0, result.stdout

def snapshot(root):
    return {p.name: p.read_bytes() for p in root.iterdir()}

with tempfile.TemporaryDirectory(prefix='race-mesh-workflow-') as temporary:
    root = pathlib.Path(temporary)
    sources, output, repeat = (root / n for n in ('sources', 'output', 'repeat'))
    for folder in (sources, output, repeat):
        folder.mkdir()
    # Four non-coplanar faces, not the existing renderer's unit cube fixture.
    source = b'# tetrahedron\nv 0 0 0\nv 2 0 0\nv 0 3 0\nv 0 0 4\nf 1 3 2\nf 1 2 4\nf 1 4 3\nf 2 3 4\n'
    (sources / 'tetra.obj').write_bytes(source)
    identity = '000000000000004d0000000000000001'
    imported = invoke('import', sources, 'tetra.obj', output, identity)
    assert imported['id'] == identity and imported['vertices'] == imported['indices'] == 12
    assert imported['representation'] == 1 and imported['target'] == 0
    assert not imported['leased']
    product = (output / imported['leaf']).read_bytes()
    # Check native bytes independently of the engine decoder.
    assert product[:8] == b'RACEAST1'
    assert struct.unpack_from('<4I', product, 8) == (1, 256, 1, 1)
    assert struct.unpack_from('<2Q', product, 48) == (12, 12)
    assert product[80:112] == hashlib.sha256(source).digest()
    assert product[-32:] == hashlib.sha256(product[:-32]).digest()
    assert hashlib.sha256(product).hexdigest() == imported['revision']
    assert imported['leaf'] == imported['revision'] + '.rasset'
    assert struct.unpack_from('<6d', product, 256) == (0., 0., 0., 0., 0., -1.)
    assert struct.unpack_from('<12I', product, 256 + 12 * 48) == tuple(range(12))
    (sources / 'tetra.obj').rename(sources / 'renamed.obj')
    assert invoke('import', sources, 'renamed.obj', repeat, identity) == imported
    assert snapshot(output) == snapshot(repeat)
    assert invoke('import', sources, 'renamed.obj', output, identity) == imported
    cooked = invoke('cook', output, imported['leaf'], output)
    assert cooked['id'] == identity and cooked['representation'] == 2 and cooked['target'] == 1
    assert cooked['revision'] != imported['revision']
    assert invoke('cook', repeat, imported['leaf'], repeat) == cooked
    assert snapshot(output) == snapshot(repeat)
    native = (output / cooked['leaf']).read_bytes()
    assert native[80:176] == product[80:176] and native[256:-32] == product[256:-32]
    assert native[-32:] == hashlib.sha256(native[:-32]).digest()
    assert hashlib.sha256(native).hexdigest() == cooked['revision']
    assert invoke('inspect', output, cooked['leaf']) == cooked
    assert invoke('validate', output, cooked['leaf']) == cooked
    loaded = invoke('load', output, cooked['leaf'])
    assert loaded == dict(cooked, leased=True)
    before = snapshot(output)
    invoke('load', output, imported['leaf'], success=False)
    invoke('cook', output, cooked['leaf'], output, success=False)
    for source_text in (b'mtllib ../escape.mtl\n', b'call evil.obj\n', b'v nan 0 0\n',
                        source + b'f -1 -2 -3\n', source + b'f 1 2 3 4\n', source + b'\0'):
        (sources / 'bad.obj').write_bytes(source_text)
        invoke('import', sources, 'bad.obj', output, identity, success=False)
        assert snapshot(output) == before
    for leaf in ('../renamed.obj', 'RENAMED.OBJ', 'sub/renamed.obj', 'renamed.obj:stream'):
        invoke('import', sources, leaf, output, identity, success=False)
    invoke('import', sources, 'renamed.obj', output, '0' * 32, success=False)
    assert snapshot(output) == before
    # An occupied hash leaf containing different bytes must never be overwritten.
    (repeat / imported['leaf']).write_bytes(b'occupied')
    invoke('import', sources, 'renamed.obj', repeat, identity, success=False)
    assert (repeat / imported['leaf']).read_bytes() == b'occupied'
    (repeat / 'damaged.rasset').write_bytes(native[:-1] + bytes([native[-1] ^ 1]))
    invoke('load', repeat, 'damaged.rasset', success=False)
    print(json.dumps({'deterministic_import_and_cook': True, 'independent_wire_checks': True,
                      'fresh_process_owning_lease': loaded, 'immutable_old_revision_preserved': True,
                      'malformed_and_external_reference_rejection': True}, sort_keys=True))

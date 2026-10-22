"""Native trusted-worker parity/failure checks; receipts and staging are retained."""
from output_paths import evidence_path
from evidence_manifest import host_path
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import time
import uuid

parser = argparse.ArgumentParser()
parser.add_argument('--tool', required=True)
parser.add_argument('--in-process', required=True)
parser.add_argument('--root', required=True)
args = parser.parse_args()
root = evidence_path(Path(args.root)) / str(uuid.uuid4())
host_path(root).mkdir(parents=True, exist_ok=False)
commands = []
checks = []


def invoke(tool, *arguments, success=True):
    command = [tool, *map(str, arguments)]
    started = time.perf_counter_ns()
    run = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=30)
    wall_ms = (time.perf_counter_ns() - started) / 1e6
    commands.append(dict(command=command, exit_code=run.returncode, stdout=run.stdout, stderr=run.stderr, wall_ms=wall_ms))
    assert (run.returncode == 0) == success, commands[-1]
    return json.loads(run.stdout) if success else run.stderr


def snapshot(folder):
    return {p.name: hashlib.sha256(host_path(p).read_bytes()).hexdigest() for p in host_path(folder).iterdir() if host_path(p).is_file()}


try:
    source, direct, output = (root / name for name in ('source', 'direct', 'output'))
    for path in (source, direct, output):
        host_path(path).mkdir()
    blob = b'v 0 0 0\nv 2 0 0\nv 0 3 0\nf 1 2 3\n'
    host_path(source / 'shape.obj').write_bytes(blob)
    identity = '000000000000004d0000000000000001'
    native = invoke(args.in_process, 'import', source, 'shape.obj', direct, identity)
    original = host_path(direct / native['leaf']).read_bytes()
    for index in range(2):
        receipt = invoke(args.tool, source, 'shape.obj', output, root / f'stage-{index}', identity)
        assert receipt['quiescent'] and receipt['leaf'] == native['leaf']
        assert host_path(output / receipt['leaf']).read_bytes() == original
        assert host_path(root / f'stage-{index}' / 'input.obj').read_bytes() == blob
    cooked_direct = invoke(args.in_process, 'cook', direct, native['leaf'], direct)
    cooked_worker = invoke(args.in_process, 'cook', output, native['leaf'], output)
    assert cooked_worker['leaf'] == cooked_direct['leaf']
    assert host_path(output / cooked_worker['leaf']).read_bytes() == host_path(direct / cooked_direct['leaf']).read_bytes()
    checks.append('exact imported and cooked bytes; repeated immutable publication')
    # The new parent/child entrypoints preserve Windows Unicode directories.
    unicode_root = root / 'caf\u00e9-\u732b'
    host_path(unicode_root).mkdir()
    host_path(unicode_root / 'source').mkdir()
    host_path(unicode_root / 'output').mkdir()
    host_path(unicode_root / 'source' / 'shape.obj').write_bytes(blob)
    unicode_result = invoke(args.tool, unicode_root / 'source', 'shape.obj', unicode_root / 'output', unicode_root / 'stage', identity)
    assert host_path(unicode_root / 'output' / unicode_result['leaf']).read_bytes() == original
    checks.append('Unicode source, output and staging directories')
    before = snapshot(output)
    invoke(args.tool, source, 'shape.obj', output, root / 'stage-0', identity, success=False)
    assert snapshot(output) == before
    host_path(source / 'invalid.obj').write_bytes(b'mtllib forbidden.mtl\n')
    diagnostic = invoke(args.tool, source, 'invalid.obj', output, root / 'stage-invalid', identity, success=False)
    assert 'mesh_worker_error' in diagnostic and snapshot(output) == before
    assert not host_path(root / 'stage-invalid' / 'result.rasset').exists()
    collision = root / 'collision'
    host_path(collision).mkdir()
    host_path(collision / native['leaf']).write_bytes(b'occupied')
    invoke(args.tool, source, 'shape.obj', collision, root / 'stage-collision', identity, success=False)
    assert host_path(collision / native['leaf']).read_bytes() == b'occupied'
    checks.append('occupied staging, parser failure diagnostics and output collision preserve publication')
    # Alternate matched end-to-end CLI workflows: direct tool imports in its own
    # process; worker tool adds an owned child. Includes process launch and I/O.
    dense = b'v 0 0 0\nv 1 0 0\nv 0 1 0\n' + b'f 1 2 3\n' * 12000
    host_path(source / 'dense.obj').write_bytes(dense)
    samples = {'direct_tool_ms': [], 'worker_tool_ms': []}
    dense_direct = dense_worker = None
    for iteration in range(11):
        order = ('direct', 'worker') if iteration % 2 == 0 else ('worker', 'direct')
        for lane in order:
            if lane == 'direct':
                dense_direct = invoke(args.in_process, 'import', source, 'dense.obj', direct, identity)
            else:
                dense_worker = invoke(args.tool, source, 'dense.obj', output, root / f'dense-stage-{iteration}', identity)
            if iteration >= 2:
                samples[f'{lane}_tool_ms'].append(commands[-1]['wall_ms'])
        assert dense_direct['leaf'] == dense_worker['leaf']
        assert host_path(direct / dense_direct['leaf']).read_bytes() == host_path(output / dense_worker['leaf']).read_bytes()
    measurement = dict(source_bytes=len(dense), triangles=12000, warmups=2, trials=9,
                       scope='Matched complete CLI workflows, including startup and filesystem; no performance threshold or speedup claim',
                       samples=samples, median_ms={key: statistics.median(value) for key, value in samples.items()},
                       product_sha256=Path(dense_direct['leaf']).stem)
    checks.append('fixed 12000-triangle direct/worker complete-byte parity and paired measurements')
    result = dict(passed=True, checks=checks, measurement=measurement, commands=commands)
except BaseException as error:
    result = dict(passed=False, checks=checks, error=repr(error), commands=commands)
    raise
finally:
    host_path(root / 'receipt.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(dict(root=str(root), passed=result['passed'], checks=len(checks))))

"""Qualify the private Windows runtime ABI with an independently compiled caller.

Run beneath run-windows-guard.ps1. This does not qualify public language FFI.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import time


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--source-manifest', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--supervisor', type=Path, required=True)
    parser.add_argument('--clang', type=Path, required=True)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('native Windows required')
    root = args.source_root.resolve(strict=True)
    manifest = json.loads(args.source_manifest.read_text(encoding='utf-8-sig'))
    assert Path(manifest['source']).resolve(strict=True) == root
    paths = ('runtime/runtime.c', 'runtime/runtime.h', 'tests/native/windows_runtime.c')
    hashes = {entry['path']: entry['sha256'] for entry in manifest['files']}
    for path in paths:
        assert sha(root/path) == hashes[path], ('source mismatch', path)
    clang = args.clang.resolve(strict=True)
    supervisor = args.supervisor.resolve(strict=True)
    output = args.output_dir.resolve()
    output.mkdir(exist_ok=False)
    result = {'status': 'running', 'source_commit': manifest['commit'],
              'source_manifest_sha256': sha(args.source_manifest),
              'harness_sha256': sha(Path(__file__)),
              'target': 'x86_64-pc-windows-msvc', 'public_ffi_qualified': False,
              'wsl_invoked_by_harness': False, 'clang_sha256': sha(clang),
              'supervisor_sha256': sha(supervisor), 'inputs': {p: hashes[p] for p in paths},
              'checks': []}

    def save():
        (output/'runtime.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')

    def run(name, command, expected=(0, b'', b'')):
        command = list(map(str, command))
        entry = {'name': name, 'command': command, 'passed': False}
        result['checks'].append(entry)
        save()
        started = time.perf_counter()
        child = subprocess.run([str(supervisor), '--timeout-seconds', '45', '--', *command],
                               cwd=root, capture_output=True, timeout=55)
        (output/(name+'.stdout')).write_bytes(child.stdout)
        (output/(name+'.stderr')).write_bytes(child.stderr)
        entry.update(exit_code=child.returncode, seconds=time.perf_counter()-started,
                     stdout_sha256=hashlib.sha256(child.stdout).hexdigest(),
                     stderr_sha256=hashlib.sha256(child.stderr).hexdigest())
        save()
        if expected is None:
            assert child.returncode == 0, (name, child.returncode, child.stderr)
        else:
            assert (child.returncode, child.stdout, child.stderr) == expected, (
                name, child.returncode, child.stdout, child.stderr)
        entry['passed'] = True
        save()
        return child

    try:
        common = [clang, '--target=x86_64-pc-windows-msvc', '-std=c11', '-O2',
                  '-fno-fast-math', '-ffp-contract=off', '-Wall', '-Wextra', '-Werror',
                  '-I', root/'runtime']
        # Separate translation units preserve the independent caller boundary.
        for name, path in (('runtime', paths[0]), ('caller', paths[2])):
            obj = output/(name+'.obj')
            run(name+'-compile', [*common, '-c', root/path, '-o', obj])
            assert struct.unpack_from('<H', obj.read_bytes(), 0)[0] == 0x8664
        executable = output/'runtime-caller.exe'
        run('link', [clang, '--target=x86_64-pc-windows-msvc', '-fuse-ld=lld',
                     '-Xlinker', '/Brepro', output/'caller.obj', output/'runtime.obj',
                     '-o', executable])
        payload = executable.read_bytes()
        offset = struct.unpack_from('<I', payload, 60)[0]
        assert payload[:2] == b'MZ' and payload[offset:offset+4] == b'PE\0\0'
        assert struct.unpack_from('<H', payload, offset+4)[0] == 0x8664
        cleanup = b'cleanup:1 active:0\n'
        cases = {
            'values': (0, b'-9223372036854775808\n9223372036854775807\nf64:0x8000000000000000\ntrue\nfalse\nresult: 42\n', b''),
            'result': (0, b'result: 42\n', b''),
            'trap': (70, cleanup, b'trap[invalid_union_tag] at 12:7\n'),
            'module-trap': (70, cleanup, b'trap[invalid_union_tag] at values.ax:12:7\n'),
            'mismatch': (2, cleanup, b'configuration[tensor_runtime]: incompatible internal ABI/version\n'),
        }
        for mode, expected in cases.items():
            run(mode, [executable, mode], expected)
        run('pe-imports', [clang.with_name('llvm-readobj.exe'), '--file-headers', '--coff-imports', executable], None)
        result.update(status='passed', passed=len(result['checks']), failures=0,
                      executable_sha256=sha(executable), runtime_object_sha256=sha(output/'runtime.obj'),
                      caller_object_sha256=sha(output/'caller.obj'))
        save()
        print(json.dumps({k: v for k, v in result.items() if k != 'checks'}))
    except BaseException as error:
        result.update(status='failed', error=f'{type(error).__name__}: {error}',
                      passed=sum(c['passed'] for c in result['checks']))
        save()
        raise


if __name__ == '__main__':
    main()

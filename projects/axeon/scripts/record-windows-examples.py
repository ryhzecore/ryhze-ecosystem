"""Native Windows example/metadata gate; no compilation of the Rust toolchain."""
import argparse
import ast
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
    parser.add_argument('--compiler', required=True, type=Path)
    parser.add_argument('--source-root', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--source-manifest', required=True, type=Path)
    parser.add_argument('--opt', type=int, choices=(0,2,3), action='append')
    args = parser.parse_args()
    if os.name != 'nt': parser.error('native Windows required')
    compiler = args.compiler.resolve(strict=True)
    supervisor = compiler.with_name('axeon-process.exe')
    root = args.source_root.resolve(strict=True)
    manifest = json.loads(args.source_manifest.read_text(encoding='utf-8'))
    assert Path(manifest['source']).resolve(strict=True) == root
    for entry in manifest['files']:
        assert sha(root/entry['path']) == entry['sha256'], ('source mismatch',entry['path'])
    args.output_dir.mkdir(exist_ok=False)
    # Reuse the independent expected values, never execute the Linux harness.
    oracle = root/'scripts/record-native-examples.py'
    tree = ast.parse(oracle.read_text(encoding='utf-8'))
    expected = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='EXPECTED' for t in n.targets))
    checks=[]
    result={'schema_version':1,'source_commit':manifest['commit'],'source_manifest_sha256':sha(args.source_manifest),
            'harness_sha256':sha(Path(__file__)),
            'compiler':str(compiler),'compiler_sha256':sha(compiler),'runtime_sha256':sha(compiler.with_name('axeon_tensor_runtime.lib')),
            'supervisor_sha256':sha(supervisor),'oracle_sha256':sha(oracle),
            'target':'x86_64-pc-windows-msvc','wsl_invoked_by_harness':False,'checks':checks,'status':'running'}
    def save():
        (args.output_dir/'examples.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    def run(name, command, want=None, seconds=45):
        started=time.perf_counter()
        entry={'name':name,'command':list(map(str,command)),'passed':False}
        checks.append(entry); save()
        p=subprocess.run([str(supervisor),'--timeout-seconds',str(seconds),'--',*map(str,command)],cwd=root,capture_output=True,timeout=seconds+10)
        (args.output_dir/(name+'.stdout')).write_bytes(p.stdout)
        (args.output_dir/(name+'.stderr')).write_bytes(p.stderr)
        entry.update(exit_code=p.returncode,seconds=time.perf_counter()-started,
                     stdout_sha256=hashlib.sha256(p.stdout).hexdigest(),stderr_sha256=hashlib.sha256(p.stderr).hexdigest())
        save()
        if want is not None:
            assert (p.returncode,p.stdout,p.stderr)==want,(name,p.returncode,p.stdout,p.stderr)
        entry['passed']=True;save()
        return p
    try:
        for name,(code,out,err) in expected.items():
            source=root/'examples'/(name+'.ax')
            label=name.replace('/','-')
            want=(code,out.encode(),err.encode())
            run(label+'-interpret',[compiler,'interpret',source],want)
            for opt in args.opt or (0,2,3):
                executable=args.output_dir/(label+f'-o{opt}.exe')
                run(label+f'-o{opt}-build',[compiler,'build',source,'--opt',str(opt),'-o',executable],(0,b'',b''))
                payload=executable.read_bytes(); offset=struct.unpack_from('<I',payload,60)[0]
                assert payload[:2]==b'MZ' and payload[offset:offset+4]==b'PE\0\0'
                assert struct.unpack_from('<H',payload,offset+4)[0]==0x8664
                metadata=json.loads(executable.with_suffix('.exe.json').read_text())
                assert metadata['target']=='x86_64-pc-windows-msvc'
                assert metadata['artifact_sha256']==sha(executable)
                assert metadata['source_sha256']==sha(source)
                run(label+f'-o{opt}-execute',[executable],want)
        ownership=run('ownership-negative',[compiler,'check',root/'examples/ownership-error.ax','--diagnostic-format','json'])
        assert ownership.returncode==1 and json.loads(ownership.stderr)['diagnostics'][0]['code']=='O007'
        obj=args.output_dir/'generic-nominals.obj'
        run('coff-object',[compiler,'emit-object',root/'examples/generic-nominals/main.ax','-o',obj],(0,b'',b''))
        assert struct.unpack_from('<H',obj.read_bytes(),0)[0]==0x8664
        run('generic-nominal-tests',[compiler,'test',root/'examples/generic-nominals/tests.ax'])
        assert checks[-1]['exit_code']==0
        result.update(status='passed',example_count=len(expected),passed=len(checks),failures=0)
        save();print(json.dumps({k:v for k,v in result.items() if k!='checks'}))
    except BaseException as error:
        result.update(status='failed',error=f'{type(error).__name__}: {error}',passed=sum(c['passed'] for c in checks))
        save();raise


if __name__=='__main__': main()

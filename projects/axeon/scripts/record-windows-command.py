"""Record a native command supervised by the qualified Windows Job adapter."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--supervisor', type=Path, required=True)
    parser.add_argument('--cwd', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=300)
    parser.add_argument('--command-file', type=Path, required=True)
    args = parser.parse_args()
    args.command = json.loads(args.command_file.read_text(encoding='utf-8-sig'))
    if os.name != 'nt' or not isinstance(args.command, list) or not args.command or not all(isinstance(x,str) for x in args.command):
        parser.error('native Windows and a nonempty JSON command array required')
    args.output_dir.mkdir(exist_ok=False)
    receipt = {'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'command': args.command, 'cwd': str(args.cwd), 'seconds': args.seconds,
               'harness_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
               'supervisor': str(args.supervisor),
               'supervisor_sha256': hashlib.sha256(args.supervisor.read_bytes()).hexdigest(),
               'wsl_invoked_by_harness': False, 'jobs': 1, 'test_threads': 1}
    start = time.perf_counter()
    with (args.output_dir/'started.json').open('x', encoding='utf-8') as out:
        json.dump(receipt, out, indent=2)
    with (args.output_dir/'stdout.log').open('xb') as out, (args.output_dir/'stderr.log').open('xb') as err:
        child = None
        try:
            child = subprocess.Popen([str(args.supervisor), '--timeout-seconds', str(args.seconds), '--', *args.command],
                                     cwd=args.cwd, stdout=out, stderr=err)
            receipt['supervisor_pid'] = child.pid
            receipt['exit_code'] = child.wait(timeout=args.seconds+15)
        except subprocess.TimeoutExpired:
            # This exact owned helper's noninherited job handle closes on exit.
            child.kill()
            child.wait(timeout=10)
            receipt['exit_code'] = 124
            receipt['outer_deadline'] = True
        except BaseException as error:
            receipt['exit_code'] = 125
            receipt['error'] = f'{type(error).__name__}: {error}'
        finally:
            if child is not None and child.poll() is None:
                child.kill()
                child.wait(timeout=10)
    receipt['seconds_elapsed'] = time.perf_counter()-start
    for name in ('stdout.log', 'stderr.log'):
        path = args.output_dir/name
        with path.open('rb') as stream:
            receipt[name] = {'bytes': path.stat().st_size, 'sha256': hashlib.file_digest(stream, 'sha256').hexdigest()}
    with (args.output_dir/'result.json').open('x', encoding='utf-8') as out:
        json.dump(receipt, out, indent=2)
        out.write('\n')
    print(json.dumps(receipt))
    raise SystemExit(0 if receipt['exit_code'] == 0 else 1)


if __name__ == '__main__':
    main()

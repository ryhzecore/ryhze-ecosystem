#!/usr/bin/env python3
"""Prepare without kernel timings; measure only after review with --execute."""
import argparse
import csv
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import resource
import signal
import statistics
import subprocess
import sys
import tempfile
import time

import generate_inputs

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CRATE = ROOT / 'crates/axeon-numerics'
RUSTC = ROOT / '.tools/rust/bin/rustc'
CLANG = Path('/workspace/tools/sysroot/usr/lib/llvm-19/bin/clang')
LANGUAGES = ('Axeon numerical library (Rust implementation)', 'checked C++ reference')
SLUGS = ('rust', 'cpp')
THREAD_ENV = {key: '1' for key in ('OPENBLAS_NUM_THREADS', 'OMP_NUM_THREADS', 'MKL_NUM_THREADS',
                                'BLIS_NUM_THREADS', 'VECLIB_MAXIMUM_THREADS', 'NUMEXPR_NUM_THREADS')}
# Reuse only the established deterministic statistics helpers, not a timing runner.
_spec = importlib.util.spec_from_file_location('benchmark_statistics', ROOT / 'benchmarks/run.py')
base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(base)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source_hashes():
    files = list(HERE.glob('*.py')) + list(HERE.glob('*.rs')) + list(HERE.glob('*.cpp')) + list(HERE.glob('*.hpp'))
    files += list((HERE / 'inputs').glob('*')) + list((CRATE / 'src').glob('*.rs'))
    files += [CRATE / 'Cargo.toml', ROOT / 'numerics-tests/cpp/reference.hpp',
              ROOT / 'numerics-tests/ieee_oracle.py', ROOT / 'benchmarks/run.py', ROOT / 'toolchain-lock.json']
    return {str(path.relative_to(ROOT)): sha(path) for path in sorted(files) if path.is_file()}


def host():
    cpuinfo = Path('/proc/cpuinfo').read_text()
    model = re.search(r'^model name\s*:\s*(.*)$', cpuinfo, re.M)
    def read(path):
        return Path(path).read_text().strip() if Path(path).exists() else None
    return {'platform': platform.platform(), 'python': sys.version, 'processor_model': model.group(1) if model else None,
            'visible_cpu_count': os.cpu_count(), 'affinity': sorted(os.sched_getaffinity(0)),
            'cpu_max': read('/sys/fs/cgroup/cpu.max'), 'memory_max': read('/sys/fs/cgroup/memory.max'),
            'thread_environment': THREAD_ENV, 'pinned_cpu': 0,
            'limitations': 'shared virtual machine; no frequency/turbo/isolation control; tiny scalar reference matrices only'}


class Runner:
    def __init__(self, receipt):
        self.receipt = receipt
        self.start = time.monotonic()
        self.child_cpu = 0.0
    def run(self, command, label):
        remaining = 180 - (time.monotonic() - self.start)
        if remaining <= 0 or self.child_cpu >= 180:
            raise RuntimeError('aggregate 180-second wall/child-CPU limit')
        def limits():
            os.setsid()
            os.sched_setaffinity(0, {0})
            resource.setrlimit(resource.RLIMIT_AS, (2*1024**3, 2*1024**3))
            resource.setrlimit(resource.RLIMIT_CPU, (30, 30))
            resource.setrlimit(resource.RLIMIT_FSIZE, (8*1024**2, 8*1024**2))
        command = [str(part) for part in command]
        env = dict(os.environ, LC_ALL='C', LANG='C', PYTHONHASHSEED='0', **THREAD_ENV)
        with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as errors:
            started = time.perf_counter_ns()
            process = subprocess.Popen(command, cwd=ROOT, stdout=output, stderr=errors, env=env, preexec_fn=limits)
            timeout = min(30, remaining)
            timed_out = False
            while True:
                pid, status, usage = os.wait4(process.pid, os.WNOHANG)
                if pid:
                    break
                if (time.perf_counter_ns() - started)/1e9 > timeout:
                    os.killpg(process.pid, signal.SIGKILL)
                    _, status, usage = os.wait4(process.pid, 0)
                    timed_out = True
                    break
                time.sleep(.001)
            elapsed = time.perf_counter_ns() - started
            process.returncode = os.waitstatus_to_exitcode(status)
            output.seek(0)
            errors.seek(0)
            stdout, stderr = output.read(8*1024**2+1), errors.read(8*1024**2+1)
        self.child_cpu += usage.ru_utime + usage.ru_stime
        record = {'label': label, 'command': command, 'wall_ns': elapsed,
                  'user_cpu_seconds': usage.ru_utime, 'system_cpu_seconds': usage.ru_stime,
                  'wait4_peak_rss_kib_includes_preexec': usage.ru_maxrss, 'returncode': process.returncode,
                  'stdout': stdout.decode(), 'stderr': stderr.decode(), 'timed_out': timed_out}
        self.receipt['commands'].append(record)
        if timed_out or process.returncode or stderr or len(stdout) > 8*1024**2:
            raise RuntimeError(f'{label} failed: {record}')
        return record


def fixtures():
    manifest = json.loads((HERE/'inputs/expected.json').read_text())
    for item in manifest['dimensions']:
        payload, expected = generate_inputs.fixture(item['dimension'])
        if item != expected or (HERE/'inputs'/f'matrix-{item["dimension"]}.bits').read_text() != payload:
            raise RuntimeError('frozen fixture differs from independent regeneration')
    return manifest['dimensions']


def planned_process_order():
    sequence = []
    for trial in range(3):
        dimensions = [8, 16] if trial % 2 == 0 else [16, 8]
        for n in dimensions:
            # Key language order by size identity, not its reversed position.
            slugs = SLUGS if (trial + int(n == 16)) % 2 == 0 else tuple(reversed(SLUGS))
            sequence.extend((trial, n, slug) for slug in slugs)
    expected = [
        (0, 8, 'rust'), (0, 8, 'cpp'), (0, 16, 'cpp'), (0, 16, 'rust'),
        (1, 16, 'rust'), (1, 16, 'cpp'), (1, 8, 'cpp'), (1, 8, 'rust'),
        (2, 8, 'rust'), (2, 8, 'cpp'), (2, 16, 'cpp'), (2, 16, 'rust'),
    ]
    if sequence != expected or len(set(sequence)) != 12:
        raise RuntimeError('counterbalanced schedule differs from reviewed 12-process plan')
    first = {n: [next(slug for t, size, slug in sequence if t == trial and size == n)
                 for trial in range(3)] for n in (8, 16)}
    if any(set(languages) != set(SLUGS) for languages in first.values()):
        raise RuntimeError('both languages must appear first within every size')
    if any(sum(languages.count(slug) for languages in first.values()) != 3 for slug in SLUGS):
        raise RuntimeError('global first-position counts must be 3:3')
    return sequence


def function_ir(text, symbol_fragment):
    functions = re.findall(r'^define [^\n]*\n.*?^}', text, re.M | re.S)
    found = [body for body in functions if symbol_fragment in body.splitlines()[0]]
    if len(found) != 1:
        raise RuntimeError(f'expected one definition for {symbol_fragment}, got {len(found)}')
    return found[0]


def strict_audit(path, symbol):
    text = path.read_text()
    body = function_ir(text, symbol)
    forbidden = re.findall(r'\b(?:fadd|fsub|fmul|fdiv|fcmp|call)\s+(?:fast|reassoc|nnan|ninf|nsz|arcp|contract|afn)\b', text)
    if forbidden or re.search(r'@llvm\.(?:fma|fmuladd)\.', text):
        raise RuntimeError('fast math or fused IR')
    if '"unsafe-fp-math"="true"' in text or '"no-nans-fp-math"="true"' in text:
        raise RuntimeError('unsafe module FP attribute')
    fmul, fadd = len(re.findall(r'\bfmul\b', body)), len(re.findall(r'\bfadd\b', body))
    if not fmul or not fadd:
        raise RuntimeError('matrix arithmetic absent from real kernel definition')
    return {'path': str(path), 'sha256': sha(path), 'symbol_fragment': symbol,
            'fmul_in_kernel_definition': fmul, 'fadd_in_kernel_definition': fadd,
            'fast_math_or_fused_operations': 0,
            'scope': 'static arithmetic retained; reviewer also inspects reduction and opaque per-batch call loops'}


def metadata(lines):
    row = lines[-1]
    if row.get('final_budget_bytes') != 0 or type(row.get('image_peak_rss_kib')) is not int or row['image_peak_rss_kib'] <= 0:
        raise RuntimeError('missing final budget/RSS evidence')
    return row


def prepare(output, receipt, runner):
    rust_version = runner.run([RUSTC, '--version', '--verbose'], 'rustc-version')['stdout']
    clang_version = runner.run([CLANG, '--version'], 'clang-version')['stdout']
    if 'rustc 1.85.0' not in rust_version or '19.1.7' not in clang_version.split():
        raise RuntimeError('pinned toolchain version mismatch')
    receipt['toolchains'] = {'rustc': rust_version, 'clang': clang_version,
                             'rustc_sha256': sha(RUSTC), 'clang_sha256': sha(CLANG)}
    rust_flags = ['--edition=2021', '-C', 'opt-level=3', '-C', 'target-cpu=x86-64',
                  '-C', 'target-feature=-fma', '-C', 'overflow-checks=yes', '-C', 'codegen-units=1', '-C', 'lto=no']
    cpp_flags = ['-std=c++17', '-O3', '-march=x86-64', '-mno-fma', '-fno-fast-math',
                 '-ffp-contract=off', '-fno-lto', '-Wall', '-Wextra', '-Werror', '-pedantic']
    library, rust_ir, rust_asm = output/'libaxeon_numerics.rlib', output/'rust-library.ll', output/'rust-library.s'
    runner.run([RUSTC, CRATE/'src/lib.rs', '--crate-name', 'axeon_numerics', '--crate-type=rlib',
                *rust_flags, '--emit', f'link={library},llvm-ir={rust_ir},asm={rust_asm}'], 'rust-library')
    runner.run([RUSTC, HERE/'driver.rs', *rust_flags, '--extern', f'axeon_numerics={library}',
                '--emit', f'link={output/"rust"},llvm-ir={output/"rust-driver.ll"},asm={output/"rust-driver.s"}'], 'rust-driver')
    runner.run([CLANG, *cpp_flags, '-c', HERE/'kernel.cpp', '-o', output/'cpp-kernel.o'], 'cpp-kernel')
    runner.run([CLANG, *cpp_flags, '-c', HERE/'driver.cpp', '-o', output/'cpp-driver.o'], 'cpp-driver')
    runner.run([CLANG, '-fno-lto', output/'cpp-kernel.o', output/'cpp-driver.o', '-lstdc++', '-lm', '-o', output/'cpp'], 'cpp-link')
    for name in ('kernel', 'driver'):
        runner.run([CLANG, *cpp_flags, '-S', '-emit-llvm', HERE/f'{name}.cpp', '-o', output/f'cpp-{name}.ll'], f'cpp-{name}-ir')
        runner.run([CLANG, *cpp_flags, '-S', HERE/f'{name}.cpp', '-o', output/f'cpp-{name}.s'], f'cpp-{name}-asm')
    receipt['ir_audits'] = [strict_audit(rust_ir, '6matmul'), strict_audit(output/'cpp-kernel.ll', 'checked_matmul')]
    for path in (rust_asm, output/'cpp-kernel.s', output/'rust-driver.s', output/'cpp-driver.s'):
        if re.search(r'\bv(?:fmadd|fmsub|fnmadd|fnmsub)', path.read_text()):
            raise RuntimeError('fused instruction in assembly')
    receipt['opaque_call_audits'] = []
    for slug, symbol in [('rust', '6matmul'), ('cpp', 'checked_matmul')]:
        path = output/f'{slug}-driver.ll'
        text = path.read_text()
        if any(symbol in line for line in text.splitlines() if line.startswith('define ')):
            raise RuntimeError('kernel unexpectedly defined/inlined into wrapper translation unit')
        calls = [line.strip() for line in text.splitlines() if symbol in line and re.search(r'\b(call|invoke)\b', line)]
        if len(calls) < 2:
            raise RuntimeError('missing check and runtime-batch opaque kernel calls')
        receipt['opaque_call_audits'].append({'language': slug, 'path': str(path), 'sha256': sha(path), 'calls': calls})
    receipt['preflight'] = []
    for item in receipt['fixtures']:
        n = item['dimension']
        for slug, language in zip(SLUGS, LANGUAGES):
            result = runner.run([output/slug, 'check', HERE/'inputs'/f'matrix-{n}.bits', n, 16, item['batch_checksum']], f'untimed-{slug}-{n}')
            lines = [json.loads(line) for line in result['stdout'].splitlines()]
            if len(lines) != 2 or lines[0] != {'dimension': n, 'bits': item['expected_bits'], 'checksum': item['single_checksum']}:
                raise RuntimeError('full-vector independent preflight mismatch')
            receipt['preflight'].append({'language': language, 'dimension': n, 'all_output_words_exact': True,
                                         'output_count': len(item['expected_bits']), 'metadata': metadata(lines)})
    receipt['artifacts'] = {path.name: sha(path) for path in sorted(output.iterdir()) if path.is_file() and path.name != 'readiness.json'}
    receipt['kernel_measurements_taken'] = False


def measure(output, receipt, runner, ready_path):
    ready = json.loads(ready_path.read_text())
    if ready.get('status') != 'passed' or ready.get('stage') != 'prepare' or ready.get('source_hashes') != source_hashes():
        raise RuntimeError('source changed since passed readiness; review and prepare anew before timing')
    directory = ready_path.parent
    for name, expected in ready['artifacts'].items():
        if sha(directory/name) != expected:
            raise RuntimeError('artifact changed after readiness')
    receipt['readiness'] = {'path': str(ready_path), 'sha256': sha(ready_path), 'artifacts': ready['artifacts']}
    receipt['toolchains'] = ready['toolchains']
    receipt['runs'] = []
    sequence = planned_process_order()
    if ready.get('planned_process_order') != [list(row) for row in sequence]:
        raise RuntimeError('measurement order differs from prepared and reviewed plan')
    receipt['process_order'] = sequence
    for trial, n, slug in sequence:
        item = next(row for row in receipt['fixtures'] if row['dimension'] == n)
        process = runner.run([directory/slug, 'measure', HERE/'inputs'/f'matrix-{n}.bits', n, 16,
                              item['batch_checksum']], f'measure-{slug}-{n}-trial{trial}')
        lines = [json.loads(line) for line in process['stdout'].splitlines()]
        if len(lines) != 10:
            raise RuntimeError('expected nine observed batch calls plus metadata')
        for index, row in enumerate(lines[:-1]):
            if row.get('call') != index or row.get('batch') != 16 or row.get('checksum') != item['batch_checksum']:
                raise RuntimeError('timed checksum/protocol mismatch')
            if type(row.get('elapsed_ns')) is not int or row['elapsed_ns'] <= 0:
                raise RuntimeError('invalid elapsed nanoseconds')
        receipt['runs'].append({'language': LANGUAGES[SLUGS.index(slug)], 'slug': slug, 'trial': trial,
                                'dimension': n, 'batch': 16, 'samples': lines[:-1], 'metadata': metadata(lines)})
    receipt['summary'] = []
    for n in (8,16):
        grouped = {}
        for slug, language in zip(SLUGS, LANGUAGES):
            runs = [row for row in receipt['runs'] if row['dimension'] == n and row['slug'] == slug]
            groups = [[sample['elapsed_ns']/16 for sample in row['samples'][2:]] for row in runs]
            grouped[slug] = groups
            values = [value for group in groups for value in group]
            receipt['summary'].append({'language': language, 'slug': slug, 'dimension': n,
                                        'warm_nanoseconds_per_matmul': base.summary(values),
                                        'cluster_bootstrap_95_percent_median_interval_ns': base.cluster_bootstrap(groups, 4000+n),
                                        'median_image_peak_rss_kib': statistics.median(row['metadata']['image_peak_rss_kib'] for row in runs)})
        rust_values = [x for group in grouped['rust'] for x in group]
        cpp_values = [x for group in grouped['cpp'] for x in group]
        receipt.setdefault('ratios', []).append({'dimension': n,
            'cpp_median_over_rust_median': statistics.median(cpp_values)/statistics.median(rust_values),
            'cluster_bootstrap_95_percent_interval': base.speedup_bootstrap(grouped['cpp'], grouped['rust'], 4400+n)})
    with (output/'samples.csv').open('w', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['language','dimension','trial','phase','batch','elapsed_batch_ns','ns_per_matmul','checksum'])
        for row in receipt['runs']:
            for sample in row['samples']:
                phase = 'first_batch' if sample['call'] == 0 else 'warmup_batch' if sample['call'] == 1 else 'warm'
                writer.writerow([row['language'],row['dimension'],row['trial'],phase,16,sample['elapsed_ns'],sample['elapsed_ns']/16,sample['checksum']])
    receipt['kernel_measurements_taken'] = True


def main():
    if not __debug__:
        raise RuntimeError('run with Python assertions enabled')
    parser = argparse.ArgumentParser()
    parser.add_argument('--stage', choices=('prepare','measure'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--readiness', type=Path)
    parser.add_argument('--execute', action='store_true')
    args = parser.parse_args()
    if args.stage == 'measure' and (not args.execute or args.readiness is None):
        parser.error('measure requires reviewed --readiness and explicit --execute')
    if 0 not in os.sched_getaffinity(0):
        raise RuntimeError('required CPU0 unavailable; do not silently select another CPU')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    path = output/('readiness.json' if args.stage == 'prepare' else 'results.json')
    receipt = {'schema_version': 1, 'stage': args.stage, 'status': 'running',
               'prior_order_limited_source_commit': '6c4f30efc8b317cd4151e504163c71bfe7046ee5',
               'comparison_designation': 'corrected counterbalanced comparison; original preserved separately',
               'planned_process_order': planned_process_order(),
               'comparison': list(LANGUAGES), 'host': host(), 'source_hashes': source_hashes(), 'fixtures': fixtures(),
               'workload': 'ordered reference square matmul, dense inputs, 16 allocation+compute+full-consumption+drop operations per batch',
               'limits': {'child_address_space_bytes': 2*1024**3, 'child_cpu_seconds':30, 'child_wall_seconds':30,
                          'aggregate_wall_seconds':180, 'aggregate_child_cpu_seconds':180, 'child_output_file_bytes':8*1024**2},
               'commands': []}
    runner = Runner(receipt)
    path.write_text(json.dumps(receipt, indent=2)+'\n')
    try:
        if args.stage == 'prepare': prepare(output, receipt, runner)
        else: measure(output, receipt, runner, args.readiness.resolve())
        if receipt['source_hashes'] != source_hashes():
            raise RuntimeError('source changed during stage')
        receipt['status'] = 'passed'
    except Exception as error:
        receipt['status'] = 'failed'
        receipt['error'] = str(error)
        raise
    finally:
        receipt['stage_wall_seconds'] = time.monotonic()-runner.start
        receipt['stage_child_cpu_seconds'] = runner.child_cpu
        path.write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps({'status':receipt['status'], 'stage':args.stage, 'receipt':str(path),
                      'kernel_measurements_taken':receipt['kernel_measurements_taken']}))


if __name__ == '__main__':
    main()

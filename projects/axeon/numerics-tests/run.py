#!/usr/bin/env python3
"""Bounded independent full-vector gate for the optional native CPU library."""
import argparse
from decimal import Decimal, localcontext, ROUND_CEILING
from fractions import Fraction
import hashlib
import json
import os
from pathlib import Path
import platform
import re
if os.name != 'nt':
    import resource
import shutil
import signal
import subprocess
import sys
import time

import decimal_oracle
import generate_fixtures
import ieee_oracle

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CRATE = ROOT / 'crates' / 'axeon-numerics'
FORBIDDEN = re.compile(r'\b(?:fadd|fsub|fmul|fdiv|frem|fcmp|call)\s+(?:fast|nnan|ninf|nsz|arcp|contract|afn|reassoc)\b')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources():
    paths = [*CRATE.rglob('*.rs'), CRATE / 'Cargo.toml', *HERE.glob('*.py'),
             *HERE.glob('*.rs'), *HERE.glob('*.md'), HERE / 'fixtures.json',
             HERE / 'requests.txt', *HERE.joinpath('cpp').glob('*.cpp'),
             *HERE.joinpath('cpp').glob('*.hpp')]
    return {str(path.relative_to(ROOT)): sha(path) for path in sorted(paths)}


class Runner:
    def __init__(self, output, receipt):
        self.output = output
        self.receipt = receipt
        self.started = time.monotonic()
        self.cpu_started = self.child_cpu()

    @staticmethod
    def child_cpu():
        usage = resource.getrusage(resource.RUSAGE_CHILDREN)
        return usage.ru_utime + usage.ru_stime

    def check_budget(self):
        if time.monotonic() - self.started > 180 or self.child_cpu() - self.cpu_started > 180:
            raise RuntimeError('aggregate 180-second wall/child-CPU budget exceeded')

    def call(self, name, command, input_data=None):
        self.check_budget()
        def limits():
            resource.setrlimit(resource.RLIMIT_AS, (2 * 1024**3, 2 * 1024**3))
            resource.setrlimit(resource.RLIMIT_CPU, (30, 30))
            resource.setrlimit(resource.RLIMIT_FSIZE, (32 * 1024**2, 32 * 1024**2))
        stdout_path = self.output / f'{name}.stdout'
        stderr_path = self.output / f'{name}.stderr'
        start = time.monotonic_ns()
        with stdout_path.open('wb') as stdout, stderr_path.open('wb') as stderr:
            process = subprocess.Popen(list(map(str, command)), cwd=ROOT,
                                       stdin=subprocess.PIPE if input_data is not None else subprocess.DEVNULL,
                                       stdout=stdout, stderr=stderr, preexec_fn=limits,
                                       start_new_session=True)
            try:
                process.communicate(input=input_data, timeout=30)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                raise RuntimeError(f'{name}: 30-second subprocess timeout')
        entry = {'name': name, 'command': list(map(str, command)),
                 'exit_status': process.returncode, 'wall_ns': time.monotonic_ns() - start,
                 'stdout': stdout_path.name, 'stderr': stderr_path.name,
                 'stdout_sha256': sha(stdout_path), 'stderr_sha256': sha(stderr_path)}
        self.receipt['commands'].append(entry)
        self.check_budget()
        if process.returncode:
            raise RuntimeError(f'{name}: status {process.returncode}; see {stderr_path}')
        if stdout_path.stat().st_size > 8 * 1024**2 or stderr_path.stat().st_size > 1024**2:
            raise RuntimeError(f'{name}: output budget exceeded')
        return stdout_path.read_text()


if os.name == 'nt':
    from windows_runner import Runner


def parse_response(line):
    fields = line.split()
    if len(fields) < 4 or fields[0] != 'v1':
        raise ValueError('malformed response prefix')
    if fields[2] == 'err':
        if len(fields) != 4:
            raise ValueError('malformed error response')
        return fields[1], {'kind': 'error', 'error': fields[3]}
    if fields[2] != 'ok':
        raise ValueError('unknown response status')
    rank = int(fields[3])
    shape = list(map(int, fields[4:4 + rank]))
    count = int(fields[4 + rank])
    encoded = fields[5 + rank:]
    product = 1
    for extent in shape:
        product *= extent
    if len(shape) != rank or product != count or len(encoded) != count:
        raise ValueError('response shape/count mismatch')
    if any(not re.fullmatch('[0-9a-f]{16}', value) for value in encoded):
        raise ValueError('invalid response encoding')
    return fields[1], {'kind': 'values', 'shape': shape, 'bits': encoded}


def ordered_bits(bits):
    return (~bits & ((1 << 64) - 1)) if bits >> 63 else bits | (1 << 63)


def compare(fixture, actual):
    expected = fixture['expected']
    if expected['kind'] == 'error':
        if actual != expected:
            raise AssertionError(f'{fixture["id"]}: expected {expected}, got {actual}')
        return {'id': fixture['id'], 'mode': 'error', 'correct': True}
    if actual['kind'] != 'values' or actual['shape'] != expected['shape']:
        raise AssertionError(f'{fixture["id"]}: wrong result kind/shape')
    values = actual['bits']
    reference = expected.get('bits', expected.get('reference_bits'))
    if len(values) != len(reference):
        raise AssertionError(f'{fixture["id"]}: wrong full-result element count')
    if expected['kind'] == 'exact':
        for index, (value, target) in enumerate(zip(values, reference)):
            if value != target:
                raise AssertionError(f'{fixture["id"]}[{index}]: {value} != exact {target}')
        return {'id': fixture['id'], 'mode': 'exact', 'elements': len(values), 'correct': True}
    maximum_fraction = Decimal(0)
    maximum_error = Decimal(0)
    maximum_ulps = 0
    with localcontext() as context:
        context.prec = 500
        context.rounding = ROUND_CEILING
        for index, (encoded, enclosure, tolerance, reference_bits) in enumerate(zip(
                values, expected['intervals'], expected['absolute_budgets'], reference)):
            value_bits = int(encoded, 16)
            if not ieee_oracle.is_finite(value_bits):
                raise AssertionError(f'{fixture["id"]}[{index}]: nonfinite stable result')
            value = ieee_oracle.fraction_from_bits(value_bits)
            if fixture.get('op') in ('sigmoid', 'softmax') and not 0 <= value <= 1:
                raise AssertionError(f'{fixture["id"]}[{index}]: stable probability outside [0,1]')
            low, high = (Fraction(Decimal(item)) for item in enclosure)
            error = max(abs(value - low), abs(value - high))
            budget = Fraction(Decimal(tolerance))
            if error > budget:
                raise AssertionError(f'{fixture["id"]}[{index}]: error {error} > fixed budget {budget}')
            error_decimal = Decimal(error.numerator) / Decimal(error.denominator)
            fraction = error / budget
            fraction_decimal = Decimal(fraction.numerator) / Decimal(fraction.denominator)
            maximum_error = max(maximum_error, error_decimal)
            maximum_fraction = max(maximum_fraction, fraction_decimal)
            ulps = abs(ordered_bits(value_bits) - ordered_bits(int(reference_bits, 16)))
            maximum_ulps = max(maximum_ulps, ulps)
            if expected['maximum_ulps'] is not None and ulps > expected['maximum_ulps']:
                raise AssertionError(f'{fixture["id"]}[{index}]: {ulps} ULP > fixed {expected["maximum_ulps"]}')
    if expected['exact_bits'] is not None and values != expected['exact_bits']:
        raise AssertionError(f'{fixture["id"]}: exact stable special case changed')
    normalization_error = None
    if expected['normalization_length'] is not None:
        total = sum((ieee_oracle.fraction_from_bits(int(value, 16)) for value in values), Fraction())
        error = abs(total - 1)
        if error > Fraction(8 * expected['normalization_length'], 2**52):
            raise AssertionError(f'{fixture["id"]}: softmax normalization exceeds fixed budget')
        normalization_error = str(error)
    return {'id': fixture['id'], 'mode': 'accuracy', 'elements': len(values), 'correct': True,
            'maximum_absolute_error_upper_bound': str(maximum_error),
            'maximum_fraction_of_predeclared_budget': str(maximum_fraction),
            'maximum_observed_ulps_from_correctly_rounded_reference': maximum_ulps,
            'normalization_error_exact_fraction': normalization_error}


def audit_ir(path):
    content = path.read_text()
    flags = [line.strip() for line in content.splitlines() if FORBIDDEN.search(line)]
    if flags or re.search(r'@llvm\.(?:fma|fmuladd)\.', content):
        raise AssertionError(f'{path}: forbidden floating permission or FMA intrinsic')
    return {'file': path.name, 'sha256': sha(path), 'forbidden_flags': [], 'fma_intrinsics': []}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    output = args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        raise SystemExit('Choose a fresh output directory; previous evidence is preserved.')
    output.mkdir(parents=True, exist_ok=True)
    receipt_path = output / 'oracle-gate.json'
    if receipt_path.exists():
        raise SystemExit('Refusing to overwrite an existing oracle gate receipt; use a new output directory.')
    receipt = {'schema': 1, 'status': 'running', 'library': 'axeon-numerics-0.1.0',
               'compiler_language_version': 'not involved: standalone Rust library and C++ oracle drivers',
               'target': 'x86_64-pc-windows-msvc' if os.name == 'nt' else 'x86_64-unknown-linux-gnu',
               'cpu': 'baseline x86-64; no FMA',
               'host': {'platform': platform.platform(), 'python': sys.version},
               'accuracy_scope': 'predeclared bounds validated on this corpus/host; no portable libm guarantee',
               'limits': {'child_memory_bytes': 2 * 1024**3, 'child_seconds': 30,
                          'aggregate_wall_seconds': 180, 'aggregate_child_cpu_seconds': 180},
               'commands': [], 'runs': [], 'ir_audits': []}
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    runner = None
    try:
        runner = Runner(output, receipt)
        frozen = json.loads((HERE / 'fixtures.json').read_text())
        reconstructed = generate_fixtures.fixtures()
        if frozen['fixtures'] != reconstructed:
            raise AssertionError('committed fixtures differ from independent regeneration')
        requests = (HERE / 'requests.txt').read_bytes()
        if hashlib.sha256(requests).hexdigest() != frozen['requests_sha256']:
            raise AssertionError('request hash mismatch')
        if requests.decode() != ''.join(generate_fixtures.command(item) + '\n' for item in reconstructed):
            raise AssertionError('request/fixture reconstruction mismatch')
        receipt['accuracy_budget_version'] = frozen['accuracy_budget_version']
        receipt['source_hashes'] = sources()
        receipt['fixture_count'] = len(reconstructed)
        receipt['requests_sha256'] = frozen['requests_sha256']
        rustc = ROOT / '.tools/rust/bin/rustc'
        if not rustc.is_file():
            installed = shutil.which('rustc')
            if not installed:
                raise RuntimeError('Rust 1.85.0 compiler not found')
            rustc = Path(installed)
        override = os.environ.get('AXEON_CLANG')
        if override:
            clang = Path(shutil.which(override) or override)
        else:
            clang = Path('/workspace/tools/sysroot/usr/lib/llvm-19/bin/clang')
            if not clang.is_file():
                installed = shutil.which('clang-19') or shutil.which('clang')
                if not installed:
                    raise RuntimeError('Clang 19.1.7 compiler not found')
                clang = Path(installed)
        if not clang.is_file():
            raise RuntimeError(f'Clang override is not an executable file: {clang}')
        receipt['toolchains'] = {'rustc_sha256': sha(rustc), 'clang_sha256': sha(clang),
                                'rustc': runner.call('rust-version', [rustc, '--version', '--verbose']),
                                'clang': runner.call('clang-version', [clang, '--version'])}
        if 'rustc 1.85.0' not in receipt['toolchains']['rustc']:
            raise AssertionError('oracle gate requires Rust 1.85.0')
        if not re.search(r'\bclang version 19\.1\.7(?:\s|$)', receipt['toolchains']['clang'].splitlines()[0]):
            raise AssertionError('oracle gate requires Clang 19.1.7')
        if os.name == 'nt':
            if 'host: x86_64-pc-windows-msvc' not in receipt['toolchains']['rustc'].splitlines():
                raise AssertionError('native Windows MSVC Rust host required')
            if 'Target: x86_64-pc-windows-msvc' not in receipt['toolchains']['clang'].splitlines():
                raise AssertionError('native Windows MSVC Clang host required')
        runner.call('oracle-unit-tests', [sys.executable, '-m', 'unittest', 'discover',
                                        '-s', HERE, '-p', 'test_*.py', '-v'])
        for optimization in (0, 2, 3):
            library = output / f'libaxeon_numerics_O{optimization}.rlib'
            llvm = output / f'axeon_numerics_O{optimization}.ll'
            assembly = output / f'axeon_numerics_O{optimization}.s'
            common = ['--edition=2021', '-C', f'opt-level={optimization}', '-C', 'target-cpu=x86-64',
                      '-C', 'target-feature=-fma', '-C', 'overflow-checks=yes', '-C', 'codegen-units=1']
            if os.name == 'nt':
                common += ['--target', 'x86_64-pc-windows-msvc']
            runner.call(f'rust-library-O{optimization}', [rustc, CRATE / 'src/lib.rs', '--crate-name',
                        'axeon_numerics', '--crate-type=rlib', *common,
                        '--emit', f'link={library},llvm-ir={llvm},asm={assembly}'])
            receipt['ir_audits'].append(audit_ir(llvm))
            if re.search(r'\bv?f(?:n?madd|n?msub)', assembly.read_text()):
                raise AssertionError('Rust library assembly contains fused arithmetic')
            suffix = '.exe' if os.name == 'nt' else ''
            rust_driver = output / f'rust-O{optimization}{suffix}'
            runner.call(f'rust-driver-O{optimization}', [rustc, HERE / 'driver.rs', *common,
                        '--extern', f'axeon_numerics={library}', '-o', rust_driver])
            cpp_driver = output / f'cpp-O{optimization}{suffix}'
            cpp_common = ['-std=c++17', f'-O{optimization}', '-march=x86-64', '-mno-fma',
                          '-fno-fast-math', '-ffp-contract=off', '-Wall', '-Wextra', '-Werror', '-pedantic']
            if os.name == 'nt':
                cpp_common += ['--target=x86_64-pc-windows-msvc', '--driver-mode=g++',
                               '-mno-incremental-linker-compatible']
            cpp_link = ['-fuse-ld=lld', '-Xlinker', '/Brepro'] if os.name == 'nt' else ['-lstdc++', '-lm']
            cpp_ir = output / f'cpp-O{optimization}.ll'
            runner.call(f'cpp-ir-O{optimization}', [clang, *cpp_common, '-S', '-emit-llvm',
                        HERE / 'cpp/driver.cpp', '-o', cpp_ir])
            receipt['ir_audits'].append(audit_ir(cpp_ir))
            runner.call(f'cpp-driver-O{optimization}', [clang, *cpp_common, HERE / 'cpp/driver.cpp',
                        *cpp_link, '-o', cpp_driver])
            for language, driver in [('Rust', rust_driver), ('C++', cpp_driver)]:
                stdout = runner.call(f'{language.lower()}-fixtures-O{optimization}', [driver], requests)
                lines = stdout.splitlines()
                if len(lines) != len(reconstructed):
                    raise AssertionError(f'{language} O{optimization}: wrong response count')
                checks = []
                for fixture, line in zip(reconstructed, lines):
                    identity, actual = parse_response(line)
                    if identity != fixture['id']:
                        raise AssertionError('fixture response identity/order mismatch')
                    checks.append(compare(fixture, actual))
                receipt['runs'].append({'language': language, 'optimization': optimization,
                                        'driver_sha256': sha(driver), 'checks': checks,
                                        'fixtures_passed': len(checks),
                                        'full_elements_checked': sum(item.get('elements', 0) for item in checks)})
        if sources() != receipt['source_hashes']:
            raise AssertionError('source files changed during oracle gate')
        runner.check_budget()
        receipt['status'] = 'passed'
    except Exception as error:
        held = os.name == 'nt' and runner is not None and isinstance(error, runner.api['ResourceHold'])
        receipt['status'] = 'resource_hold' if held else 'failed'
        receipt['error'] = f'{type(error).__name__}: {error}'
    finally:
        receipt['total_wall_seconds'] = time.monotonic() - runner.started if runner else None
        receipt['total_child_cpu_seconds'] = runner.child_cpu() - runner.cpu_started if runner else None
        receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({'status': receipt['status'], 'receipt': str(receipt_path),
                      'runs': len(receipt['runs']), 'fixtures_per_run': receipt.get('fixture_count'),
                      'error': receipt.get('error')}))
    return 0 if receipt['status'] == 'passed' else 75 if receipt['status'] == 'resource_hold' else 1


if __name__ == '__main__':
    raise SystemExit(main())

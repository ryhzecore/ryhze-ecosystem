#!/usr/bin/env python3
"""Exact dyadic fixtures, with complete expected outputs from integer algebra."""
from fractions import Fraction
import hashlib
import importlib.util
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location('ieee_oracle', ROOT / 'numerics-tests/ieee_oracle.py')
# Dataclass annotations need the imported module registered before execution.
import sys
oracle = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = oracle
spec.loader.exec_module(oracle)
OFFSET = 0xcbf29ce484222325
PRIME = 0x100000001b3
MASK = 2**64 - 1
BATCH = 16


def checksum(values, batch=BATCH):
    value = OFFSET
    for _ in range(batch):
        for word in values:
            value = ((value ^ word) * PRIME) & MASK
    return value


def fixture(n):
    left = [[(row * 17 + column * 11 + 3) % 31 - 15 for column in range(n)] for row in range(n)]
    right = [[(row * 7 + column * 19 + 5) % 29 - 14 for column in range(n)] for row in range(n)]
    numerators = [sum(left[row][k] * right[k][column] for k in range(n))
                  for row in range(n) for column in range(n)]
    a_bits = [oracle.round_fraction(Fraction(value, 16)) for row in left for value in row]
    b_bits = [oracle.round_fraction(Fraction(value, 32)) for row in right for value in row]
    out_bits = [oracle.round_fraction(Fraction(value, 512)) for value in numerators]
    # Independently replay specified rounded IEEE operations as a second oracle.
    replay = []
    for row in range(n):
        for column in range(n):
            value = 0
            for k in range(n):
                value = oracle.add(value, oracle.mul(a_bits[row*n+k], b_bits[k*n+column]))
            replay.append(value)
    if out_bits != replay:
        raise RuntimeError('integer exactness proof disagrees with rounded IEEE replay')
    payload = ('AXNUMBENCH1 ' + str(n) + ' ' + str(n*n) + '\n'
               + ' '.join(f'{value:016x}' for value in a_bits) + '\n'
               + ' '.join(f'{value:016x}' for value in b_bits) + '\n')
    return payload, {'dimension': n, 'batch': BATCH, 'count': n*n,
                     'expected_bits': [f'{value:016x}' for value in out_bits],
                     'single_checksum': f'{checksum(out_bits, 1):016x}',
                     'batch_checksum': f'{checksum(out_bits):016x}',
                     'input_sha256': hashlib.sha256(payload.encode()).hexdigest(),
                     'proof': {'left_denominator': 16, 'right_denominator': 32,
                               'product_denominator': 512, 'max_abs_product_numerator': 210,
                               'max_abs_partial_sum_numerator_bound': n*210,
                               'all_products_and_partial_sums_exact_in_binary64': True,
                               'complete_result_matches_independent_rounded_ieee_replay': True}}


def main():
    manifest = {'schema_version': 1, 'batch': BATCH, 'dimensions': []}
    for n in (8, 16):
        payload, evidence = fixture(n)
        (HERE / 'inputs' / f'matrix-{n}.bits').write_text(payload)
        manifest['dimensions'].append(evidence)
    (HERE / 'inputs' / 'expected.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()

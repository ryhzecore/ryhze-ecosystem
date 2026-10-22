#!/usr/bin/env python3
"""Generate independent, bounded full-value fixtures; never run native code."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import random
from fractions import Fraction

import decimal_oracle as decimal
import ieee_oracle as ieee

ROOT = Path(__file__).resolve().parent


def bits(value):
    return ieee.round_fraction(Fraction(str(value)))


def tensor(shape, values, transforms=()):
    return {'shape': list(shape), 'bits': list(values),
            'transforms': [list(item) for item in transforms]}


def logical(value):
    """Independent explicit integer-coordinate mapping; no native descriptor reuse."""
    shape = value['shape'][:]
    if len(shape) > 4:
        raise ValueError('RankLimit')
    count = 1
    for extent in shape:
        count *= extent
    if count != len(value['bits']):
        raise ValueError('ShapeMismatch')
    mapping = list(range(count))
    for transform in value['transforms']:
        old_shape = shape[:]
        if transform[0] == 'p':
            axes = transform[1:]
            if sorted(axes) != list(range(len(shape))):
                raise ValueError('InvalidPermutation')
            shape = [old_shape[axis] for axis in axes]
            def old_coordinate(coordinate):
                result = [0] * len(shape)
                for new_axis, old_axis in enumerate(axes):
                    result[old_axis] = coordinate[new_axis]
                return result
        else:
            _, axis, start, end, step = transform
            if step == 0:
                raise ValueError('InvalidStep')
            if axis >= len(shape) or start > end or end > shape[axis]:
                raise ValueError('Bounds')
            shape[axis] = (end - start + step - 1) // step
            def old_coordinate(coordinate):
                result = list(coordinate)
                result[axis] = start + result[axis] * step
                return result
        next_mapping = []
        for coordinate in itertools.product(*(range(extent) for extent in shape)):
            index = 0
            for component, extent in zip(old_coordinate(coordinate), old_shape):
                index = index * extent + component
            next_mapping.append(mapping[index])
        mapping = next_mapping
    return shape, [ieee.canonical(value['bits'][index]) for index in mapping], mapping


def encode_tensor(value):
    fields = [str(len(value['shape'])), *map(str, value['shape']),
              str(len(value['bits'])), *(f'{item:016x}' for item in value['bits']),
              str(len(value['transforms']))]
    for transform in value['transforms']:
        if transform[0] == 'p':
            fields += ['p', str(len(transform) - 1), *map(str, transform[1:])]
        else:
            fields += list(map(str, transform))
    return fields


def command(fixture):
    fields = ['v1', fixture['id'], fixture['op'], *encode_tensor(fixture['a']),
              str(int('b' in fixture))]
    if 'b' in fixture:
        fields += encode_tensor(fixture['b'])
    return ' '.join(fields)


def expected(fixture):
    op = fixture['op']
    try:
        shape, a, mapping = logical(fixture['a'])
        fixture['logical_mapping_a'] = mapping
        if 'b' in fixture:
            b_shape, b, b_mapping = logical(fixture['b'])
            fixture['logical_mapping_b'] = b_mapping
        if op in ('add', 'mul'):
            if shape != b_shape:
                raise ValueError('ShapeMismatch')
            result = [getattr(ieee, op)(x, y) for x, y in zip(a, b)]
        elif op == 'sum':
            result = [0]
            for value in a:
                result[0] = ieee.add(result[0], value)
            shape = []
        elif op == 'matmul':
            if len(shape) != 2 or len(b_shape) != 2:
                raise ValueError('RankMismatch')
            m, k = shape
            other_k, n = b_shape
            if k != other_k:
                raise ValueError('ShapeMismatch')
            if m * n > 65_536:
                raise ValueError('ElementLimit')
            if m * n * (k + 1) > 1_048_576:
                raise ValueError('WorkLimit')
            result = []
            if fixture.get('zero_boundary_proof'):
                # Every operand is +0; every rounded +0*+0 and +0++0 is +0.
                # This verifies the complete large result without running a
                # million Fraction operations just to regenerate a known zero.
                assert all(value == 0 for value in a + b)
                result = [0] * (m * n)
            else:
                for row in range(m):
                    for column in range(n):
                        accumulator = 0
                        for inner in range(k):
                            product = ieee.mul(a[row * k + inner], b[inner * n + column])
                            accumulator = ieee.add(accumulator, product)
                        result.append(accumulator)
            shape = [m, n]
        else:
            if op != 'sigmoid' and len(shape) != 1:
                raise ValueError('RankMismatch')
            if op != 'sigmoid' and not a:
                raise ValueError('EmptyInput')
            if not all(ieee.is_finite(value) for value in a):
                raise ValueError('NonFiniteInput')
            intervals, rounded, precision = decimal.references(op, a)
            budgets = decimal.accuracy_budgets(op, a, intervals)
            if op == 'logsumexp':
                shape = []
            return {'kind': 'accuracy', 'shape': shape,
                    'reference_bits': [f'{value:016x}' for value in rounded],
                    'intervals': [[str(value.lo), str(value.hi)] for value in intervals],
                    'absolute_budgets': list(map(str, budgets)),
                    'oracle_decimal_precision': precision,
                    'normalization_length': len(a) if op == 'softmax' else None,
                    'exact_bits': fixture.get('exact_stable_bits'),
                    'maximum_ulps': fixture.get('maximum_ulps')}
        return {'kind': 'exact', 'shape': shape, 'bits': [f'{value:016x}' for value in result]}
    except ValueError as error:
        return {'kind': 'error', 'error': str(error)}


def fixtures():
    result = []
    def add(name, op, shape, a, b=None, b_shape=None, transforms=(), b_transforms=(), **extra):
        item = {'id': name, 'op': op, 'a': tensor(shape, a, transforms), **extra}
        if b is not None:
            item['b'] = tensor(shape if b_shape is None else b_shape, b, b_transforms)
        result.append(item)

    special = [0, 1 << 63, 1, (1 << 63) | 1, 0x000fffffffffffff,
               0x0010000000000000, 0x8010000000000000, 0x3fefffffffffffff,
               bits(1), bits(-1), 0x3ff0000000000001, bits('9007199254740992'),
               bits('-9007199254740992'), 0x7fefffffffffffff, 0xffefffffffffffff,
               0x7ff0000000000000, 0xfff0000000000000, 0x7ff0000000000001,
               0xfff8000000001234, 0x7ff8000000000000]
    pairs = list(itertools.product(special, repeat=2))
    for op in ('add', 'mul'):
        add(f'{op}_ieee_cross_product', op, [len(special), len(special)],
            [a for a, _ in pairs], [b for _, b in pairs])
        rng = random.Random(0xA0E04)
        random_a = [rng.getrandbits(64) for _ in range(256)]
        random_b = [rng.getrandbits(64) for _ in range(256)]
        add(f'{op}_seeded_bits', op, [256], random_a, random_b)
        add(f'{op}_empty', op, [3, 0], [], [])
        add(f'{op}_scalar', op, [], [bits('1.5')], [bits('-2.25')])
        add(f'{op}_shape_error', op, [2], [0, 0], [0, 0], b_shape=[1, 2])
    reductions = [[], [1 << 63], [1 << 63, 1 << 63], [bits('1e16'), bits(1), bits('-1e16')],
                  [bits('1e16'), bits('-1e16'), bits(1)], [1, 1, 1],
                  [0x7fefffffffffffff, 0x7fefffffffffffff, 0xffefffffffffffff],
                  [0x7ff0000000000000, 0xfff0000000000000], [0x7ff0000000000001, bits(2)],
                  [bits('9007199254740992'), bits(1), bits(1)]]
    for index, values in enumerate(reductions):
        add(f'sum_case_{index}', 'sum', [len(values)], values)
    base = [bits(Fraction(index - 10, 8)) for index in range(20)]
    for index, transforms in enumerate([(('p', 1, 0),), (('s', 0, 1, 4, 2),),
                                       (('p', 1, 0), ('s', 0, 1, 5, 2)),
                                       (('s', 1, 2, 2, 1), ('p', 1, 0))]):
        add(f'sum_view_{index}', 'sum', [4, 5], base, transforms=transforms)
        shape, values, _ = logical(tensor([4, 5], base, transforms))
        add(f'add_view_{index}', 'add', [4, 5], base, [bits(1)] * len(values),
            b_shape=shape, transforms=transforms)
    add('bad_permutation', 'sum', [4, 5], base, transforms=(('p', 0, 0),))
    add('bad_slice_step', 'sum', [4, 5], base, transforms=(('s', 0, 0, 2, 0),))
    add('bad_slice_bounds', 'sum', [4, 5], base, transforms=(('s', 1, 0, 6, 1),))
    add('matmul_rectangular', 'matmul', [2, 3], list(map(bits, [1, 2, 3, 4, 5, 6])),
        list(map(bits, [7, 8, 9, 10, 11, 12])), b_shape=[3, 2])
    add('matmul_separate_rounding', 'matmul', [1, 2],
        [bits(Fraction(1) + Fraction(1, 2**27)), bits(-1)],
        [bits(Fraction(1) - Fraction(1, 2**27)), bits(1)], b_shape=[2, 1])
    add('matmul_transposed_offset', 'matmul', [3, 4], list(map(bits, range(12))),
        list(map(bits, range(-7, 8))), b_shape=[5, 3],
        transforms=(('p', 1, 0), ('s', 0, 1, 4, 2)), b_transforms=(('p', 1, 0),))
    add('matmul_empty_inner', 'matmul', [2, 0], [], [], b_shape=[0, 3])
    add('matmul_empty_rows', 'matmul', [0, 2], [], [bits(1)] * 6, b_shape=[2, 3])
    add('matmul_shape_error', 'matmul', [1, 2], [0, 0], [0] * 3, b_shape=[3, 1])
    add('matmul_rank_error', 'matmul', [2], [0, 0], [0, 0])
    for inner in (0, 15, 16):
        add(f'matmul_work_boundary_k{inner}', 'matmul', [256, inner], [0] * (256 * inner),
            [0] * (inner * 256), b_shape=[inner, 256], zero_boundary_proof=True)
    stable_values = ['-1e308', '-800', '-745', '-710', '-100', '-40', '-1', '-1e-20',
                     '0', '1e-20', '1', '40', '100', '710', '745', '800', '1e308']
    add('sigmoid_extremes', 'sigmoid', [len(stable_values)], list(map(bits, stable_values)))
    add('sigmoid_signed_zero', 'sigmoid', [2], [0, 1 << 63],
        exact_stable_bits=['3fe0000000000000'] * 2)
    add('sigmoid_scalar', 'sigmoid', [], [bits('-0.5')])
    add('sigmoid_empty', 'sigmoid', [4, 0], [])
    add('sigmoid_view', 'sigmoid', [4, 5], base,
        transforms=(('p', 1, 0), ('s', 0, 1, 5, 2)))
    rows = [[0], [1 << 63], list(map(bits, [0, 0, 0])), list(map(bits, [0, -40])),
            list(map(bits, [-1000, -1001, -1002])), list(map(bits, [1000, 1001, 1002])),
            list(map(bits, ['1e308', '-1e308', '0'])),
            list(map(bits, [1, 1, 1, -100])),
            [1, 2, 3, 0x8000000000000001],
            list(map(bits, ['709.5', '-35.25', '708.75'])),
            list(map(bits, ['-1e-20', '0', '1e-20'])),
            list(map(bits, [0, -745])), list(map(bits, [0, -800])),
            [bits(Fraction((index * 17) % 29 - 14, 7)) for index in range(64)]]
    for op in ('softmax', 'logsumexp'):
        for index, values in enumerate(rows):
            extra = {}
            if op == 'logsumexp' and len(values) == 1:
                extra['exact_stable_bits'] = [f'{values[0]:016x}']
            if op == 'logsumexp' and index == 3:
                extra['maximum_ulps'] = 4
            if op == 'logsumexp' and values == [0, bits(-745)]:
                extra['exact_stable_bits'] = ['0000000000000001']
            if op == 'logsumexp' and values == [0, bits(-800)]:
                extra['exact_stable_bits'] = ['0000000000000000']
            add(f'{op}_finite_{index}', op, [len(values)], values, **extra)
        add(f'{op}_empty_error', op, [0], [])
        add(f'{op}_rank_error', op, [1, 2], [0, 0])
        add(f'{op}_view', op, [20], base, transforms=(('s', 0, 1, 19, 3),))
    for op in ('sigmoid', 'softmax', 'logsumexp'):
        for name, value in [('positive_inf', 0x7ff0000000000000),
                            ('negative_inf', 0xfff0000000000000),
                            ('quiet_nan', 0xfff8000000001234),
                            ('signaling_nan', 0x7ff0000000000001)]:
            add(f'{op}_{name}_error', op, [2], [bits(1), value])
    for fixture in result:
        fixture['expected'] = expected(fixture)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'fixtures.json')
    parser.add_argument('--requests', type=Path, default=ROOT / 'requests.txt')
    args = parser.parse_args()
    data = fixtures()
    requests = ''.join(command(fixture) + '\n' for fixture in data)
    assert len(data) <= 512 and len(requests.encode()) <= 1_048_576
    assert max(map(len, requests.splitlines())) <= 262_144
    payload = {'schema': 1, 'accuracy_budget_version': 'predeclared-2026-10-08-v1',
               'accuracy_scope': 'tested corpus on recorded default-IEEE host; not a portable libm guarantee',
               'requests_sha256': hashlib.sha256(requests.encode()).hexdigest(), 'fixtures': data}
    args.output.write_text(json.dumps(payload, indent=2) + '\n')
    args.requests.write_text(requests)
    print(json.dumps({'fixtures': len(data), 'requests_bytes': len(requests),
                      'requests_sha256': payload['requests_sha256']}))


if __name__ == '__main__':
    main()

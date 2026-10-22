"""Oracle regression tests with independent identities and enclosure checks."""
from decimal import Decimal, localcontext
from fractions import Fraction
import unittest

import decimal_oracle as oracle
import ieee_oracle as ieee
import run as gate


class DecimalOracleTests(unittest.TestCase):
    def test_exact_dyadic_input_and_ambient_precision(self):
        bits = 0x3fb999999999999a  # exact binary64 input nearest decimal 0.1
        expected = None
        for ambient in (6, 28, 73):
            with localcontext() as context:
                context.prec = ambient
                value = oracle.exact_decimal(bits)
                self.assertEqual(Fraction(value), ieee.fraction_from_bits(bits))
                result = oracle.references('sigmoid', [bits])
            if expected is None:
                expected = result
            self.assertEqual(result, expected)
        with localcontext() as context:
            context.prec = 180
            value = oracle.exact_decimal(bits)
            separate = Decimal(1) / (Decimal(1) + value.copy_negate().exp())
        interval = expected[0][0]
        self.assertLessEqual(interval.lo, separate)
        self.assertGreaterEqual(interval.hi, separate)

    def test_exact_zero_identities(self):
        for zero in (0, 1 << 63):
            _, rounded, _ = oracle.references('sigmoid', [zero])
            self.assertEqual(rounded, [0x3fe0000000000000])
            _, rounded, _ = oracle.references('logsumexp', [zero])
            self.assertEqual(rounded, [zero])
        _, rounded, _ = oracle.references('softmax', [0, 0])
        self.assertEqual(rounded, [0x3fe0000000000000] * 2)

    def test_tiny_logsumexp_correction_and_underflow(self):
        expected = [(40, 0x3c539792499b1a24), (745, 1), (800, 0)]
        for magnitude, target in expected:
            _, rounded, _ = oracle.references('logsumexp', [0, ieee.round_fraction(Fraction(-magnitude))])
            self.assertEqual(rounded, [target])

    def test_gate_rejects_naive_log_sum_that_loses_correction(self):
        inputs = [0, ieee.round_fraction(Fraction(-40))]
        intervals, rounded, precision = oracle.references('logsumexp', inputs)
        fixture = {'id': 'tiny', 'expected': {
            'kind': 'accuracy', 'shape': [], 'reference_bits': [f'{rounded[0]:016x}'],
            'intervals': [[str(intervals[0].lo), str(intervals[0].hi)]],
            'absolute_budgets': list(map(str, oracle.accuracy_budgets('logsumexp', inputs, intervals))),
            'oracle_decimal_precision': precision, 'normalization_length': None,
            'exact_bits': None, 'maximum_ulps': 4}}
        with self.assertRaises(AssertionError):
            gate.compare(fixture, {'kind': 'values', 'shape': [], 'bits': ['0000000000000000']})

    def test_gate_requires_farthest_endpoint_with_exact_rationals(self):
        fixture = {'id': 'enclosure', 'expected': {
            'kind': 'accuracy', 'shape': [], 'reference_bits': ['3ff0000000000000'],
            'intervals': [['0.9', '1.1']], 'absolute_budgets': ['0.01'],
            'normalization_length': None, 'exact_bits': None, 'maximum_ulps': None}}
        with self.assertRaises(AssertionError):
            gate.compare(fixture, {'kind': 'values', 'shape': [], 'bits': ['3ff0000000000000']})

    def test_probability_range_is_not_relaxed_by_subnormal_budget(self):
        fixture = {'id': 'negative_probability', 'op': 'softmax', 'expected': {
            'kind': 'accuracy', 'shape': [1], 'reference_bits': ['0000000000000000'],
            'intervals': [['0', '0']], 'absolute_budgets': ['1e-323'],
            'normalization_length': None, 'exact_bits': None, 'maximum_ulps': None}}
        with self.assertRaises(AssertionError):
            gate.compare(fixture, {'kind': 'values', 'shape': [1], 'bits': ['8000000000000001']})


if __name__ == '__main__':
    unittest.main()

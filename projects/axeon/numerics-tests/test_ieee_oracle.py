"""Independent integer/rational fixtures for the binary64 oracle.

Run: python3 -m unittest discover -s numerics-tests -p test_ieee_oracle.py -v
No fixture performs Python floating-point arithmetic or converts through float.
"""

from fractions import Fraction
import unittest

from ieee_oracle import (
    CANONICAL_NAN as NAN,
    NEGATIVE_INFINITY as NINF,
    NEGATIVE_ZERO as NZERO,
    POSITIVE_INFINITY as INF,
    add,
    canonical,
    decode,
    div,
    fraction_from_bits,
    is_finite,
    mul,
    neg,
    round_fraction,
    sub,
)


ONE = 0x3FF0_0000_0000_0000
NEG_ONE = 0xBFF0_0000_0000_0000
TWO = 0x4000_0000_0000_0000
NEG_TWO = 0xC000_0000_0000_0000
HALF = 0x3FE0_0000_0000_0000
MIN_NORMAL = 0x0010_0000_0000_0000
MAX_SUBNORMAL = 0x000F_FFFF_FFFF_FFFF
MAX_FINITE = 0x7FEF_FFFF_FFFF_FFFF


class DecodeTests(unittest.TestCase):
    def test_exact_dyadic_values(self):
        cases = [
            (0, Fraction(0), "zero", False),
            (NZERO, Fraction(0), "zero", True),
            (ONE, Fraction(1), "normal", False),
            (NEG_ONE, Fraction(-1), "normal", True),
            (0x3FF8_0000_0000_0000, Fraction(3, 2), "normal", False),
            (0x3FB9_9999_9999_999A, Fraction(3602879701896397, 36028797018963968), "normal", False),
            (1, Fraction(1, 1 << 1074), "subnormal", False),
            (NZERO | 1, Fraction(-1, 1 << 1074), "subnormal", True),
            (MAX_SUBNORMAL, Fraction((1 << 52) - 1, 1 << 1074), "subnormal", False),
            (MIN_NORMAL, Fraction(1, 1 << 1022), "normal", False),
            (MAX_FINITE, Fraction(((1 << 53) - 1) << 971), "normal", False),
        ]
        for bits, value, category, negative in cases:
            with self.subTest(bits=hex(bits)):
                decoded = decode(bits)
                self.assertEqual(decoded.value, value)
                self.assertEqual(decoded.category, category)
                self.assertEqual(decoded.negative, negative)
                self.assertEqual(fraction_from_bits(bits), value)
                self.assertTrue(is_finite(bits))
                self.assertEqual(canonical(bits), bits)

    def test_infinities_and_all_nan_classes(self):
        for bits, negative in [(INF, False), (NINF, True)]:
            with self.subTest(bits=hex(bits)):
                decoded = decode(bits)
                self.assertEqual(decoded.category, "infinity")
                self.assertEqual(decoded.negative, negative)
                self.assertIsNone(decoded.value)
                self.assertEqual(canonical(bits), bits)
                self.assertFalse(is_finite(bits))
                with self.assertRaises(ValueError):
                    fraction_from_bits(bits)
        for bits in [NAN, 0x7FF0_0000_0000_0001, 0xFFF0_0000_0000_0001,
                     0x7FF7_FFFF_FFFF_FFFF, 0xFFF7_FFFF_FFFF_FFFF,
                     0x7FFF_FFFF_FFFF_FFFF, 0xFFFF_FFFF_FFFF_FFFF]:
            with self.subTest(bits=hex(bits)):
                self.assertEqual(decode(bits).category, "nan")
                self.assertIsNone(decode(bits).value)
                self.assertFalse(is_finite(bits))
                self.assertEqual(canonical(bits), NAN)
                self.assertEqual(neg(bits), NAN)
                with self.assertRaises(ValueError):
                    fraction_from_bits(bits)

    def test_rejects_invalid_encodings_without_truncation(self):
        for operation in [decode, canonical, is_finite, fraction_from_bits, neg]:
            for bits in [-1, 1 << 64]:
                with self.subTest(operation=operation.__name__, bits=bits):
                    with self.assertRaises(ValueError):
                        operation(bits)
            for bits in [True, "0", Fraction(1)]:
                with self.subTest(operation=operation.__name__, bits=bits):
                    with self.assertRaises(TypeError):
                        operation(bits)


class RoundingTests(unittest.TestCase):
    def test_normal_halfway_rounding_and_signs(self):
        cases = [
            (Fraction((1 << 53) + 1, 1 << 53), ONE),
            (Fraction((1 << 53) + 3, 1 << 53), ONE + 2),
            (Fraction((1 << 54) + 1, 1 << 54), ONE),
            (Fraction((1 << 54) + 3, 1 << 54), ONE + 1),
            (Fraction(1, 3), 0x3FD5_5555_5555_5555),
            (Fraction(1, 10), 0x3FB9_9999_9999_999A),
            (Fraction(9007199254740993), 0x4340_0000_0000_0000),
            (Fraction(9007199254740995), 0x4340_0000_0000_0002),
            (Fraction((1 << 63) - 1), 0x43E0_0000_0000_0000),
        ]
        for value, expected in cases:
            with self.subTest(value=value):
                self.assertEqual(round_fraction(value), expected)
                self.assertEqual(round_fraction(-value), expected | NZERO)

    def test_normal_significand_carry_rounds_to_next_binade(self):
        midpoint = Fraction((1 << 54) - 1, 1 << 53)
        self.assertEqual(round_fraction(midpoint), TWO)
        self.assertEqual(round_fraction(midpoint - Fraction(1, 1 << 55)), TWO - 1)
        self.assertEqual(round_fraction(-midpoint), NEG_TWO)

    def test_subnormal_halfway_and_normal_boundary_rounding(self):
        cases = [
            (Fraction(1, 1 << 1075), 0),
            (Fraction(3, 1 << 1075), 2),
            (Fraction(5, 1 << 1075), 2),
            (Fraction(7, 1 << 1075), 4),
            (Fraction(3, 1 << 1076), 1),
            (Fraction(1, 1 << 1074), 1),
            (Fraction((1 << 53) - 1, 1 << 1075), MIN_NORMAL),
            (Fraction((1 << 54) - 3, 1 << 1076), MAX_SUBNORMAL),
            (Fraction(1, 1 << 20000), 0),
        ]
        for value, expected in cases:
            with self.subTest(value=value):
                self.assertEqual(round_fraction(value), expected)
                self.assertEqual(round_fraction(-value), expected | NZERO)

    def test_overflow_boundary_is_rounded_not_saturated(self):
        threshold = Fraction(((1 << 54) - 1) << 970)
        for value, expected in [
            (threshold - 1, MAX_FINITE),
            (threshold, INF),
            (threshold + 1, INF),
            (Fraction(1 << 1024), INF),
            (Fraction(1 << 20000), INF),
        ]:
            with self.subTest(expected=hex(expected)):
                self.assertEqual(round_fraction(value), expected)
                self.assertEqual(round_fraction(-value), expected | NZERO)

    def test_zero_sign_override_and_fraction_type_requirement(self):
        self.assertEqual(round_fraction(Fraction(0)), 0)
        self.assertEqual(round_fraction(Fraction(0), negative_zero=True), NZERO)
        self.assertEqual(round_fraction(Fraction(1), negative_zero=True), ONE)
        self.assertEqual(round_fraction(Fraction(-1), negative_zero=False), NEG_ONE)
        with self.assertRaises(TypeError):
            round_fraction("1")

    def test_roundtrip_deterministic_full_width_encodings(self):
        state = 0x53F0_87AD_9246_B1C5
        for _ in range(2048):
            state = (state * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
            if is_finite(state):
                self.assertEqual(
                    round_fraction(fraction_from_bits(state), negative_zero=state == NZERO),
                    state,
                )


class ArithmeticTests(unittest.TestCase):
    def test_finite_expected_bit_fixtures(self):
        cases = [
            (add, 0x3FB9_9999_9999_999A, 0x3FC9_9999_9999_999A, 0x3FD3_3333_3333_3334),
            (sub, ONE, 0x3FB9_9999_9999_999A, 0x3FEC_CCCC_CCCC_CCCD),
            (mul, 0x3FF8_0000_0000_0000, TWO, 0x4008_0000_0000_0000),
            (div, ONE, 0x4024_0000_0000_0000, 0x3FB9_9999_9999_999A),
            (div, ONE, 0x4008_0000_0000_0000, 0x3FD5_5555_5555_5555),
            (div, NEG_ONE, 0x4008_0000_0000_0000, 0xBFD5_5555_5555_5555),
            (mul, MIN_NORMAL, HALF, 0x0008_0000_0000_0000),
            (add, MAX_SUBNORMAL, 1, MIN_NORMAL),
            (sub, MIN_NORMAL, 1, MAX_SUBNORMAL),
            (mul, 1, HALF, 0),
            (mul, NZERO | 1, HALF, NZERO),
            (div, 3, TWO, 2),
            (mul, MAX_FINITE, TWO, INF),
            (mul, MAX_FINITE | NZERO, TWO, NINF),
        ]
        for operation, a, b, expected in cases:
            with self.subTest(operation=operation.__name__, a=hex(a), b=hex(b)):
                self.assertEqual(operation(a, b), expected)

    def test_signed_zero_rules(self):
        cases = [
            (add, 0, 0, 0), (add, NZERO, NZERO, NZERO),
            (add, NZERO, 0, 0), (add, 0, NZERO, 0),
            (add, ONE, NEG_ONE, 0), (add, NEG_ONE, ONE, 0),
            (sub, ONE, ONE, 0), (sub, NEG_ONE, NEG_ONE, 0),
            (sub, NZERO, 0, NZERO), (sub, NZERO, NZERO, 0),
            (sub, 0, NZERO, 0), (sub, 0, 0, 0),
            (mul, NZERO, TWO, NZERO), (mul, NZERO, NEG_TWO, 0),
            (mul, 0, NEG_TWO, NZERO), (mul, NZERO, NZERO, 0),
            (div, NZERO, TWO, NZERO), (div, 0, NEG_TWO, NZERO),
            (div, NZERO, NEG_TWO, 0), (div, ONE, NINF, NZERO),
            (div, NEG_ONE, NINF, 0), (div, NZERO, INF, NZERO),
        ]
        for operation, a, b, expected in cases:
            with self.subTest(operation=operation.__name__, a=hex(a), b=hex(b)):
                self.assertEqual(operation(a, b), expected)
        self.assertEqual(neg(0), NZERO)
        self.assertEqual(neg(NZERO), 0)
        self.assertEqual(neg(1), NZERO | 1)

    def test_infinite_and_invalid_operation_tables(self):
        cases = [
            (add, INF, ONE, INF), (add, NEG_ONE, NINF, NINF),
            (add, INF, INF, INF), (add, NINF, NINF, NINF),
            (add, INF, NINF, NAN), (add, NINF, INF, NAN),
            (sub, INF, INF, NAN), (sub, NINF, NINF, NAN),
            (sub, INF, NINF, INF), (sub, NINF, INF, NINF),
            (mul, INF, NEG_ONE, NINF), (mul, NINF, NINF, INF),
            (mul, 0, INF, NAN), (mul, NINF, NZERO, NAN),
            (div, ONE, 0, INF), (div, ONE, NZERO, NINF),
            (div, NEG_ONE, NZERO, INF), (div, INF, 0, INF),
            (div, INF, NZERO, NINF), (div, NINF, NEG_TWO, INF),
            (div, INF, INF, NAN), (div, INF, NINF, NAN),
            (div, 0, 0, NAN), (div, NZERO, 0, NAN),
            (div, 0, NZERO, NAN), (div, NZERO, NZERO, NAN),
        ]
        for operation, a, b, expected in cases:
            with self.subTest(operation=operation.__name__, a=hex(a), b=hex(b)):
                self.assertEqual(operation(a, b), expected)
        self.assertEqual(neg(INF), NINF)
        self.assertEqual(neg(NINF), INF)

    def test_nan_propagation_is_canonical_for_both_argument_orders(self):
        for nan in [NAN, 0x7FF0_0000_0000_0001, 0xFFF0_0000_0000_0001,
                    0x7FFF_FFFF_FFFF_FFFF, 0xFFFF_FFFF_FFFF_FFFF]:
            for other in [0, NZERO, ONE, NEG_ONE, INF, NINF, 1, NAN]:
                for operation in [add, sub, mul, div]:
                    with self.subTest(operation=operation.__name__, nan=hex(nan), other=hex(other)):
                        self.assertEqual(operation(nan, other), NAN)
                        self.assertEqual(operation(other, nan), NAN)

    def test_reductions_observe_each_rounding_and_order(self):
        large = 0x4341_C379_37E0_8000  # Exact integer 10000000000000000.
        self.assertEqual(fraction_from_bits(large), Fraction(10000000000000000))
        self.assertEqual(add(add(large, ONE), neg(large)), 0)
        self.assertEqual(add(add(large, neg(large)), ONE), ONE)
        self.assertEqual(sub(mul(0x3FF0_0000_0000_0001, 0x3FEF_FFFF_FFFF_FFFE), ONE), 0)
        self.assertEqual(add(add(0, NZERO), NZERO), 0)
        self.assertEqual(sub(mul(MAX_FINITE, TWO), INF), NAN)


if __name__ == "__main__":
    unittest.main()

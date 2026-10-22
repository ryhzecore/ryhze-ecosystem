"""Independent binary64 arithmetic oracle using only integers and Fractions.

Inputs and results are unsigned 64-bit encodings, never Python floats. Every
arithmetic operation rounds once to nearest, ties to even, with gradual
underflow. NaNs are observed as the Axeon canonical positive quiet NaN.

This module models the default floating-point environment: exceptions produce
IEEE values and exception flags are not observable. It does not emulate a host
math library or promise a particular internal NaN payload.
"""

from __future__ import annotations

from dataclasses import dataclass
from fractions import Fraction


SIGN_MASK = 0x8000_0000_0000_0000
EXPONENT_MASK = 0x7FF0_0000_0000_0000
FRACTION_MASK = 0x000F_FFFF_FFFF_FFFF
POSITIVE_ZERO = 0x0000_0000_0000_0000
NEGATIVE_ZERO = 0x8000_0000_0000_0000
POSITIVE_INFINITY = 0x7FF0_0000_0000_0000
NEGATIVE_INFINITY = 0xFFF0_0000_0000_0000
CANONICAL_NAN = 0x7FF8_0000_0000_0000
MAX_BITS = 0xFFFF_FFFF_FFFF_FFFF


def _validate_bits(bits: int) -> int:
    if type(bits) is not int:
        raise TypeError("binary64 encoding must be an integer")
    if not 0 <= bits <= MAX_BITS:
        raise ValueError("binary64 encoding must be in 0..2**64-1")
    return bits


def canonical(bits: int) -> int:
    """Preserve non-NaN encodings and normalize every NaN, including signaling."""
    bits = _validate_bits(bits)
    return CANONICAL_NAN if (bits & ~SIGN_MASK) > POSITIVE_INFINITY else bits


def is_finite(bits: int) -> bool:
    """Return whether an encoding represents a finite value, including zeros."""
    return (_validate_bits(bits) & EXPONENT_MASK) != EXPONENT_MASK


@dataclass(frozen=True)
class Decoded:
    """A value's exact magnitude class, sign, and finite signed rational value.

    ``category`` is zero, subnormal, normal, infinity, or nan. ``value`` is None
    only for infinity and NaN. The sign field preserves negative zero, which a
    Fraction alone cannot represent. Raw NaN sign is available for inspection
    but arithmetic and canonical() do not preserve it.
    """

    category: str
    negative: bool
    value: Fraction | None


def decode(bits: int) -> Decoded:
    """Decode a binary64 encoding without executing floating-point arithmetic."""
    bits = _validate_bits(bits)
    negative = bool(bits & SIGN_MASK)
    exponent = (bits >> 52) & 0x7FF
    significand = bits & FRACTION_MASK
    if exponent == 0x7FF:
        return Decoded("nan" if significand else "infinity", negative, None)
    if exponent == 0:
        if significand == 0:
            return Decoded("zero", negative, Fraction(0))
        category = "subnormal"
        power = -1074
    else:
        category = "normal"
        significand |= 1 << 52
        power = exponent - 1023 - 52
    if negative:
        significand = -significand
    value = (
        Fraction(significand << power)
        if power >= 0
        else Fraction(significand, 1 << -power)
    )
    return Decoded(category, negative, value)


def fraction_from_bits(bits: int) -> Fraction:
    """Return a finite encoding's exact value; both signed zeros become zero.

    Raise ValueError for infinities and NaNs. Use decode() when zero's sign also
    matters; round_fraction(..., negative_zero=True) can restore negative zero.
    """
    value = decode(bits).value
    if value is None:
        raise ValueError("a nonfinite binary64 value has no rational value")
    return value


def _floor_log2_ratio(numerator: int, denominator: int) -> int:
    """Find floor(log2(numerator / denominator)) for strictly positive inputs."""
    exponent = numerator.bit_length() - denominator.bit_length()
    if exponent >= 0:
        if numerator < denominator << exponent:
            exponent -= 1
    elif numerator << -exponent < denominator:
        exponent -= 1
    return exponent


def _round_scaled_ratio(numerator: int, denominator: int, power: int) -> int:
    """Round (numerator/denominator)/2**power to an integer, ties to even."""
    if power >= 0:
        denominator <<= power
    else:
        numerator <<= -power
    quotient, remainder = divmod(numerator, denominator)
    twice_remainder = remainder << 1
    if twice_remainder > denominator or (
        twice_remainder == denominator and quotient & 1
    ):
        quotient += 1
    return quotient


def round_fraction(value: Fraction, negative_zero: bool = False) -> int:
    """Round an exact rational to binary64, nearest/ties-even, returning bits.

    ``negative_zero`` selects the sign only when the exact input is zero. A
    negative nonzero value that underflows always produces negative zero.
    Finite overflow rounds to the appropriately signed infinity.
    """
    if not isinstance(value, Fraction):
        raise TypeError("round_fraction requires an exact fractions.Fraction")
    if value == 0:
        return NEGATIVE_ZERO if negative_zero else POSITIVE_ZERO
    sign = SIGN_MASK if value < 0 else 0
    numerator = abs(value.numerator)
    denominator = value.denominator
    exponent = _floor_log2_ratio(numerator, denominator)

    # These early exits avoid unnecessary shifts for extremely large inputs.
    if exponent >= 1024:
        return sign | POSITIVE_INFINITY
    if exponent < -1075:
        return sign

    if exponent < -1022:
        # Subnormal spacing is fixed. Rounding may promote the largest
        # subnormal to the minimum normal, whose encoding is exactly 2**52.
        significand = _round_scaled_ratio(numerator, denominator, -1074)
        return sign | significand

    significand = _round_scaled_ratio(numerator, denominator, exponent - 52)
    if significand == 1 << 53:
        significand >>= 1
        exponent += 1
    if exponent == 1024:
        return sign | POSITIVE_INFINITY
    return sign | ((exponent + 1023) << 52) | (significand - (1 << 52))


def _infinity(negative: bool) -> int:
    return NEGATIVE_INFINITY if negative else POSITIVE_INFINITY


def _zero(negative: bool) -> int:
    return NEGATIVE_ZERO if negative else POSITIVE_ZERO


def neg(bits: int) -> int:
    """Negate a value exactly, with canonical NaN observation."""
    bits = canonical(bits)
    return bits if bits == CANONICAL_NAN else bits ^ SIGN_MASK


def add(a_bits: int, b_bits: int) -> int:
    """Add two binary64 values, rounding the exact finite sum once."""
    a, b = decode(a_bits), decode(b_bits)
    if a.category == "nan" or b.category == "nan":
        return CANONICAL_NAN
    if a.category == "infinity" and b.category == "infinity":
        return CANONICAL_NAN if a.negative != b.negative else _infinity(a.negative)
    if a.category == "infinity":
        return _infinity(a.negative)
    if b.category == "infinity":
        return _infinity(b.negative)
    # Exact finite cancellation yields +0 in nearest mode; only -0 + -0
    # retains a negative zero sign.
    negative_zero = a.category == b.category == "zero" and a.negative and b.negative
    assert a.value is not None and b.value is not None
    return round_fraction(a.value + b.value, negative_zero=negative_zero)


def sub(a_bits: int, b_bits: int) -> int:
    """Subtract with one rounding, including signed-zero and infinity rules."""
    return add(a_bits, neg(b_bits))


def mul(a_bits: int, b_bits: int) -> int:
    """Multiply with one rounding; zero times infinity is canonical NaN."""
    a, b = decode(a_bits), decode(b_bits)
    if a.category == "nan" or b.category == "nan":
        return CANONICAL_NAN
    negative = a.negative != b.negative
    if a.category == "infinity" or b.category == "infinity":
        if a.category == "zero" or b.category == "zero":
            return CANONICAL_NAN
        return _infinity(negative)
    assert a.value is not None and b.value is not None
    return round_fraction(a.value * b.value, negative_zero=negative)


def div(a_bits: int, b_bits: int) -> int:
    """Divide with one rounding and IEEE nontrapping exceptional values."""
    a, b = decode(a_bits), decode(b_bits)
    if a.category == "nan" or b.category == "nan":
        return CANONICAL_NAN
    negative = a.negative != b.negative
    if a.category == b.category == "infinity":
        return CANONICAL_NAN
    if a.category == "infinity":
        return _infinity(negative)
    if b.category == "infinity":
        return _zero(negative)
    if b.category == "zero":
        return CANONICAL_NAN if a.category == "zero" else _infinity(negative)
    if a.category == "zero":
        return _zero(negative)
    assert a.value is not None and b.value is not None
    return round_fraction(a.value / b.value)


__all__ = [
    "add", "sub", "mul", "div", "neg", "round_fraction", "fraction_from_bits",
    "canonical", "is_finite", "decode", "Decoded", "CANONICAL_NAN",
    "POSITIVE_INFINITY", "NEGATIVE_INFINITY", "POSITIVE_ZERO", "NEGATIVE_ZERO",
]

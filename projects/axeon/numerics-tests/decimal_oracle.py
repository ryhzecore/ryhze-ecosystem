"""Conservative high-precision references; no host binary64 arithmetic.

Decimal exp/ln are nearest-even even when the context requests directed
rounding. Each call is therefore widened to adjacent Decimal representables;
ordinary interval arithmetic uses directed rounding. Endpoints are exact decimal
rationals. These enclosures are not a claim about Rust/libc accuracy.
"""
from dataclasses import dataclass
from decimal import Decimal, localcontext, ROUND_FLOOR, ROUND_CEILING
from fractions import Fraction

import ieee_oracle as ieee

D0 = Decimal(0)
D1 = Decimal(1)


def exact_decimal(bits):
    value = ieee.fraction_from_bits(bits)
    sign = bool(bits >> 63)
    denominator = value.denominator
    exponent = denominator.bit_length() - 1
    assert denominator == 1 << exponent
    coefficient = abs(value.numerator) * 5**exponent
    return Decimal((int(sign), tuple(map(int, str(coefficient))), -exponent))


@dataclass(frozen=True)
class Interval:
    lo: Decimal
    hi: Decimal

    def __post_init__(self):
        assert self.lo <= self.hi


class Arithmetic:
    def __init__(self, precision):
        self.precision = precision

    def directed(self, function, rounding):
        with localcontext() as context:
            context.prec = self.precision
            context.rounding = rounding
            return function()

    def add(self, a, b):
        return Interval(self.directed(lambda: a.lo + b.lo, ROUND_FLOOR),
                        self.directed(lambda: a.hi + b.hi, ROUND_CEILING))

    def sub(self, a, b):
        return Interval(self.directed(lambda: a.lo - b.hi, ROUND_FLOOR),
                        self.directed(lambda: a.hi - b.lo, ROUND_CEILING))

    def div(self, a, b):
        assert b.lo > 0
        pairs = [(x, y) for x in (a.lo, a.hi) for y in (b.lo, b.hi)]
        return Interval(min(self.directed(lambda x=x, y=y: x / y, ROUND_FLOOR)
                            for x, y in pairs),
                        max(self.directed(lambda x=x, y=y: x / y, ROUND_CEILING)
                            for x, y in pairs))

    def transcendental(self, x, name):
        if name == 'exp' and x == 0:
            return Interval(D1, D1)
        if name == 'ln' and x == 1:
            return Interval(D0, D0)
        with localcontext() as context:
            context.prec = self.precision
            value = getattr(x, name)()
            return Interval(value.next_minus(context), value.next_plus(context))

    def exp(self, interval):
        # All callers use nonpositive arguments. This cutoff is an upper bound,
        # not a substituted value: exp(x) <= exp(-800) for x <= -800. Its entire
        # enclosure is below half the least positive binary64 subnormal.
        assert interval.hi <= 0
        cutoff = Decimal(-800)
        lower = D0 if interval.lo < cutoff else self.transcendental(interval.lo, 'exp').lo
        upper = self.transcendental(max(interval.hi, cutoff), 'exp').hi
        return Interval(max(D0, lower), upper)

    def ln(self, interval):
        assert interval.lo > 0
        return Interval(self.transcendental(interval.lo, 'ln').lo,
                        self.transcendental(interval.hi, 'ln').hi)


def evaluate(op, bits, precision):
    arithmetic = Arithmetic(precision)
    values = [exact_decimal(value) for value in bits]
    one = Interval(D1, D1)
    zero = Interval(D0, D0)
    if op == 'sigmoid':
        result = []
        for value in values:
            argument = value.copy_negate() if value >= 0 else value
            exp = arithmetic.exp(Interval(argument, argument))
            numerator = one if value >= 0 else exp
            result.append(arithmetic.div(numerator, arithmetic.add(one, exp)))
        return result
    if op not in ('softmax', 'logsumexp') or not values:
        raise ValueError('reference requires a supported nonempty reduction')
    if op == 'logsumexp' and len(values) == 1:
        return [Interval(values[0], values[0])]
    maximum = max(values)
    max_interval = Interval(maximum, maximum)
    exps = [arithmetic.exp(arithmetic.sub(Interval(x, x), max_interval))
            for x in values]
    total = zero
    first_maximum = values.index(maximum)
    for index, exp in enumerate(exps):
        if op != 'logsumexp' or index != first_maximum:
            total = arithmetic.add(total, exp)
    if op == 'softmax':
        return [arithmetic.div(exp, total) for exp in exps]
    logarithm = arithmetic.ln(arithmetic.add(one, total))
    return [arithmetic.add(max_interval, logarithm)]


def references(op, bits):
    """Refine until every enclosure chooses the same binary64 rounding cell."""
    for precision in (100, 200, 400):
        intervals = evaluate(op, bits, precision)
        rounded = []
        for interval in intervals:
            low = ieee.round_fraction(Fraction(interval.lo))
            high = ieee.round_fraction(Fraction(interval.hi))
            if low != high:
                break
            rounded.append(low)
        else:
            if op == 'logsumexp' and len(bits) == 1:
                rounded = bits[:]
            return intervals, rounded, precision
    raise ArithmeticError(f'{op}: unresolved oracle rounding boundary at 400 digits')


def accuracy_budgets(op, bits, intervals):
    """Constants approved before observing either native implementation.

    Return conservative lower bounds on each permitted absolute error, so the
    comparison cannot pass by rounding a tolerance upward.
    """
    with localcontext() as context:
        context.prec = 100
        context.rounding = ROUND_FLOOR
        epsilon = D1 / Decimal(2**52)
        absolute_floor = Decimal(2) / Decimal(2**1074)
        if op == 'logsumexp':
            maximum = max(exact_decimal(value) for value in bits)
            ln_n = Arithmetic(100).transcendental(Decimal(len(bits)), 'ln').lo
            return [Decimal(16) * epsilon * (D1 + abs(maximum) + ln_n)]
        relative = Decimal('2e-14') if op == 'sigmoid' else (
            Decimal('2e-12') + Decimal(8 * len(bits)) * epsilon)
        return [relative * min(abs(value.lo), abs(value.hi)) + absolute_floor
                for value in intervals]

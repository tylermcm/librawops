"""Independent exact-rational, staged IEEE nearest-even alpha fixture oracle.

No engine, NumPy, native floating arithmetic, or platform casts are used.
Run with --check for a read-only verification of the checked-in fixture header.
"""
from fractions import Fraction as Q
from pathlib import Path
import argparse
import random


def power(n):
    return Q(1 << n) if n >= 0 else Q(1, 1 << -n)


def decode(bits, precision=24, exponent_bits=8):
    fraction_bits = precision - 1
    bias = (1 << (exponent_bits - 1)) - 1
    exponent = (bits >> fraction_bits) & ((1 << exponent_bits) - 1)
    mantissa = bits & ((1 << fraction_bits) - 1)
    assert exponent != (1 << exponent_bits) - 1
    value = Q(mantissa) * power(1 - bias - fraction_bits) if exponent == 0 else \
        Q((1 << fraction_bits) + mantissa) * power(exponent - bias - fraction_bits)
    return -value if bits >> (fraction_bits + exponent_bits) else value


def encode(value, precision=24, exponent_bits=8):
    negative = value < 0
    value = abs(value)
    sign = int(negative) << (precision - 1 + exponent_bits)
    if not value:
        return sign
    bias = (1 << (exponent_bits - 1)) - 1
    exponent = value.numerator.bit_length() - value.denominator.bit_length()
    if value < power(exponent):
        exponent -= 1
    shift = max(exponent, 1 - bias) - (precision - 1)
    scaled = value / power(shift)
    quotient, remainder = divmod(scaled.numerator, scaled.denominator)
    twice = remainder * 2
    rounded = quotient + int(twice > scaled.denominator or
                             (twice == scaled.denominator and quotient & 1))
    if rounded == 0:
        return sign
    if exponent < 1 - bias and rounded < (1 << (precision - 1)):
        return sign | rounded
    exponent = max(exponent, 1 - bias)
    if rounded == 1 << precision:
        rounded >>= 1
        exponent += 1
    assert exponent <= bias, 'overflow must be admitted separately'
    return sign | ((exponent + bias) << (precision - 1)) | \
        (rounded - (1 << (precision - 1)))


def double(value):
    return decode(encode(value, 53, 11), 53, 11)


FMAX = decode(0x7f7fffff)


def stored(value):
    if abs(value) > FMAX:
        raise OverflowError
    return encode(value)


def expected(kind, words):
    s, b = [decode(v) for v in words[:4]], [decode(v) for v in words[4:]]
    if kind == 0:  # premultiply straight RGB
        if s[3] == 0:
            return [0] * 4
        if s[3] == 1:
            return words[:4]
        return [stored(double(v * s[3])) for v in s[:3]] + [words[3]]
    if kind == 1:  # apply scalar coverage b.r
        if b[0] == 0 or s[3] == 0:
            return [0] * 4
        if b[0] == 1:
            return words[:4]
        alpha = stored(double(s[3] * b[0]))
        if alpha == 0:
            return [0] * 4
        return [stored(double(v * b[0])) for v in s[:3]] + [alpha]
    if kind == 2:  # source over backdrop
        if s[3] == 0:
            return words[4:] if b[3] else [0] * 4
        if s[3] == 1 or b[3] == 0:
            return words[:4]
        t = double(1 - s[3])
        return [stored(double(s[c] + double(b[c] * t))) for c in range(4)]
    assert kind == 3  # explicit float32 straight access
    if not s[3]:
        return [0] * 4
    return [stored(double(v / s[3])) for v in s[:3]] + [0]


def generate():
    rng = random.Random(0xA1FA2026)
    alphas = [0, 1, 2, 0x007fffff, 0x00800000, 0x33800000,
              0x3e800000, 0x3f000000, 0x3f7fffff, 0x3f800000]
    colors = [0, 1, 0x80000001, 0x007fffff, 0x00800000, 0x3f000001,
              0xbf800000, 0x40000000, 0x7f7fffff, 0xff7fffff]
    def color():
        # Full finite binary32 exponent range, either sign, including subnormals.
        return rng.choice(colors) if rng.randrange(3) == 0 else \
            (rng.randrange(0x7f800000) | (rng.randrange(2) << 31))
    def pixel(alpha=None):
        a = rng.choice(alphas) if alpha is None else alpha
        return [color(), color(), color(), a] if a else [0] * 4
    rows, doubles = [], []
    for index in range(120):
        a = alphas[index % len(alphas)]
        straight = [color(), color(), color(), a]
        p, backdrop = pixel(a), pixel()
        for kind, words in [(0, straight + [0] * 4),
                            (1, p + [alphas[(index // 10) % len(alphas)], 0, 0, 0]),
                            (2, p + backdrop), (3, p + [0] * 4)]:
            try:
                output, overflow = expected(kind, words), False
            except OverflowError:
                output, overflow = [0] * 4, True
            rows.append((kind, words, output, overflow))
        value = [decode(x) for x in p]
        output = [encode(v / value[3], 53, 11) if value[3] else 0 for v in value[:3]]
        doubles.append((p, output))
    # Same-sign headroom overflow and exact cancellation, partial transparency.
    for sign in (0, 0x80000000):
        for alpha in (1, 0x3e800000, 0x3f000000, 0x3f7fffff):
            p = [0x7f7fffff | sign] * 3 + [alpha]
            for other_sign in (sign, sign ^ 0x80000000):
                words = p + [0x7f7fffff | other_sign] * 3 + [0x3f000000]
                try:
                    output, overflow = expected(2, words), False
                except OverflowError:
                    output, overflow = [0] * 4, True
                rows.append((2, words, output, overflow))
    lines = ['// Generated by alpha_numeric_oracle.py; exact Fraction arithmetic.',
             '#pragma once', '#include <array>', '#include <cstdint>',
             'namespace alpha_reference {',
             'struct Case { int kind; std::array<std::uint32_t,8> input; '
             'std::array<std::uint32_t,4> output; bool overflow; };',
             'inline constexpr Case cases[] = {']
    fmt = lambda values: '{' + ','.join('0x%08xu' % v for v in values) + '}'
    for kind, words, output, overflow in rows:
        lines.append('    {%d,%s,%s,%s},' % (kind, fmt(words), fmt(output), str(overflow).lower()))
    lines += ['};', 'struct StraightCase { std::array<std::uint32_t,4> input; '
              'std::array<std::uint64_t,3> output; };',
              'inline constexpr StraightCase straight_cases[] = {']
    for words, output in doubles:
        formatted = '{' + ','.join('0x%016xull' % v for v in output) + '}'
        lines.append('    {%s,%s},' % (fmt(words), formatted))
    lines += ['};', '} // namespace alpha_reference', '']
    return '\n'.join(lines), len(rows), len(doubles)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    # Self checks independent of engine fixtures: nearest-even midpoint and carry.
    assert encode(Q(1) + power(-24)) == 0x3f800000
    assert encode(Q(1) + 3 * power(-24)) == 0x3f800002
    assert encode(power(-150)) == 0
    assert encode(3 * power(-150)) == 2
    assert encode(FMAX) == 0x7f7fffff
    content, count, straight_count = generate()
    target = Path(__file__).with_name('alpha_numeric_v1.hpp')
    if args.check:
        assert target.read_text(encoding='utf-8') == content, 'fixture differs from oracle'
    else:
        with target.open('w', encoding='utf-8', newline='\n') as stream:
            stream.write(content)
    print('%d staged float32 cases and %d binary64 straight cases verified' %
          (count, straight_count))


if __name__ == '__main__':
    main()

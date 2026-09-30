"""Render a pinned 16-bit sRGB TIFF through the native raster API.

This is a reference/diagnostic bridge, not a product TIFF codec.  It decodes
the checked-in sRGB fixture transfer function, converts the pixels to the
engine's canonical linear ProPhoto/D50 space, calls ``rawengine_native``, and
writes the encoded sRGB result with the original ICC bytes preserved.
"""

from __future__ import annotations

import argparse
import array
import math
from pathlib import Path
import sys
from typing import Iterator

import reference_harness as harness


Matrix = tuple[float, ...]


def multiply(a: Matrix, b: Matrix) -> Matrix:
    return tuple(
        sum(a[row * 3 + k] * b[k * 3 + col] for k in range(3))
        for row in range(3)
        for col in range(3)
    )


def multiply_vector(a: Matrix, v: tuple[float, float, float]) -> tuple[float, float, float]:
    return (
        a[0] * v[0] + a[1] * v[1] + a[2] * v[2],
        a[3] * v[0] + a[4] * v[1] + a[5] * v[2],
        a[6] * v[0] + a[7] * v[1] + a[8] * v[2],
    )


def inverse(m: Matrix) -> Matrix:
    determinant = (
        m[0] * (m[4] * m[8] - m[5] * m[7])
        - m[1] * (m[3] * m[8] - m[5] * m[6])
        + m[2] * (m[3] * m[7] - m[4] * m[6])
    )
    if not math.isfinite(determinant) or abs(determinant) < 1e-12:
        raise ValueError("color matrix is singular")
    d = 1.0 / determinant
    return (
        (m[4] * m[8] - m[5] * m[7]) * d,
        (m[2] * m[7] - m[1] * m[8]) * d,
        (m[1] * m[5] - m[2] * m[4]) * d,
        (m[5] * m[6] - m[3] * m[8]) * d,
        (m[0] * m[8] - m[2] * m[6]) * d,
        (m[2] * m[3] - m[0] * m[5]) * d,
        (m[3] * m[7] - m[4] * m[6]) * d,
        (m[1] * m[6] - m[0] * m[7]) * d,
        (m[0] * m[4] - m[1] * m[3]) * d,
    )


def rgb_to_xyz(xy: tuple[float, ...], wx: float, wy: float) -> Matrix:
    basis = [0.0] * 9
    for col in range(3):
        x, y = xy[2 * col], xy[2 * col + 1]
        basis[col] = x / y
        basis[3 + col] = 1.0
        basis[6 + col] = (1.0 - x - y) / y
    white = (wx / wy, 1.0, (1.0 - wx - wy) / wy)
    scales = multiply_vector(inverse(tuple(basis)), white)
    return tuple(basis[row * 3 + col] * scales[col]
                 for row in range(3) for col in range(3))


def d50_to_d65() -> Matrix:
    bradford = (0.8951, 0.2664, -0.1614,
                -0.7502, 1.7135, 0.0367,
                0.0389, -0.0685, 1.0296)
    d50 = multiply_vector(bradford, (0.3457 / 0.3585, 1.0,
                                     (1.0 - 0.3457 - 0.3585) / 0.3585))
    d65 = multiply_vector(bradford, (0.3127 / 0.3290, 1.0,
                                     (1.0 - 0.3127 - 0.3290) / 0.3290))
    scale = (d65[0] / d50[0], 0.0, 0.0,
             0.0, d65[1] / d50[1], 0.0,
             0.0, 0.0, d65[2] / d50[2])
    return multiply(inverse(bradford), multiply(scale, bradford))


def srgb_to_prophoto_d50() -> Matrix:
    srgb_to_xyz_d65 = rgb_to_xyz(
        (0.640, 0.330, 0.300, 0.600, 0.150, 0.060), 0.3127, 0.3290)
    prophoto_to_xyz_d50 = rgb_to_xyz(
        (0.7347, 0.2653, 0.1596, 0.8404, 0.0366, 0.0001),
        0.3457, 0.3585)
    return multiply(inverse(prophoto_to_xyz_d50),
                    multiply(inverse(d50_to_d65()), srgb_to_xyz_d65))


def decode_srgb(value: float) -> float:
    return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4


def load_scene_linear(path: Path) -> tuple[int, int, bytes, array.array]:
    matrix = srgb_to_prophoto_d50()
    with harness.Tiff16(path) as image:
        if image.profile != harness.checked_profile():
            raise ValueError("input TIFF does not use the pinned sRGB2014 ICC profile")
        pixels = array.array("f")
        for y in range(image.height):
            row = image.row(y)
            for x in range(image.width):
                base = x * 3
                encoded = (row[base] / 65535.0,
                           row[base + 1] / 65535.0,
                           row[base + 2] / 65535.0)
                pixels.extend(multiply_vector(matrix, tuple(decode_srgb(v) for v in encoded)))
        return image.width, image.height, image.profile, pixels


def quantized_rows(values: array.array, width: int, height: int) -> Iterator[bytes]:
    for y in range(height):
        row = array.array("H")
        for value in values[y * width * 3:(y + 1) * width * 3]:
            if not math.isfinite(value):
                raise ValueError("native render returned a non-finite sample")
            row.append(max(0, min(65535, int(value * 65535.0 + 0.5))))
        if sys.byteorder != "little":
            row.byteswap()
        yield row.tobytes()


def render(input_path: Path, output_path: Path, build_dir: Path,
           exposure_stops: float, tone_shoulder: float,
           tone_gamma: float, tile_size: int) -> dict:
    sys.path.insert(0, str(build_dir))
    import rawengine_native as raw

    width, height, profile, pixels = load_scene_linear(input_path)
    out_width, out_height, output = raw.render_raster(
        pixels, width, height,
        {"working_space": "prophoto-d50",
         "exposure_stops": exposure_stops,
         "tone_shoulder": tone_shoulder,
         "tone_gamma": tone_gamma,
         "tile_size": tile_size,
         "output_mode": "srgb-preview"})
    values = array.array("f")
    values.frombytes(output)
    if (out_width, out_height) != (width, height):
        raise ValueError("native render changed fixture dimensions")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    harness.write_tiff(output_path, width, height,
                       quantized_rows(values, width, height), profile)
    return {"input": str(input_path), "output": str(output_path),
            "width": width, "height": height,
            "exposure_stops": exposure_stops,
            "tone_shoulder": tone_shoulder, "tone_gamma": tone_gamma,
            "tile_size": tile_size}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--exposure-stops", type=float, default=0.0)
    parser.add_argument("--tone-shoulder", type=float, default=0.25)
    parser.add_argument("--tone-gamma", type=float, default=1.0)
    parser.add_argument("--tile-size", type=int, default=64)
    args = parser.parse_args()
    print(render(args.input, args.output, args.build_dir,
                  args.exposure_stops, args.tone_shoulder,
                  args.tone_gamma, args.tile_size))


if __name__ == "__main__":
    main()

"""Decoded-Bayer preview against independently demosaiced/calibrated float32 RGB."""
import array
import math
import struct
import sys
import unittest

if __name__ == "__main__":
    sys.path.insert(0, sys.argv.pop(1))
import rawengine_native as raw


def f32(value):
    return struct.unpack("f", struct.pack("f", value))[0]


def values(result):
    pixels = array.array("f")
    pixels.frombytes(result[2])
    assert len(pixels) == result[0] * result[1] * 3
    return pixels


def multiply(a, b):
    return [sum(a[r * 3 + k] * b[k * 3 + c] for k in range(3))
            for r in range(3) for c in range(3)]


def inverse(m):
    # Gauss-Jordan inverse, independent of the core's cofactor implementation.
    rows = [[*m[r * 3:r * 3 + 3], *[float(r == c) for c in range(3)]] for r in range(3)]
    for col in range(3):
        pivot = max(range(col, 3), key=lambda r: abs(rows[r][col]))
        rows[col], rows[pivot] = rows[pivot], rows[col]
        scale = rows[col][col]
        rows[col] = [v / scale for v in rows[col]]
        for r in range(3):
            if r != col:
                scale = rows[r][col]
                rows[r] = [a - scale * b for a, b in zip(rows[r], rows[col])]
    return [v for row in rows for v in row[3:]]


def white(x, y):
    return [x / y, 1.0, (1.0 - x - y) / y]


def rgb_matrix(primaries, illuminant):
    columns = [white(*xy) for xy in primaries]
    basis = [columns[c][r] for r in range(3) for c in range(3)]
    inv = inverse(basis)
    w = white(*illuminant)
    scales = [sum(inv[r * 3 + c] * w[c] for c in range(3)) for r in range(3)]
    return [basis[r * 3 + c] * scales[c] for r in range(3) for c in range(3)]


def xyz_to_working(space):
    if space == "prophoto-d50":
        return inverse(rgb_matrix(((0.7347, 0.2653), (0.1596, 0.8404), (0.0366, 0.0001)), (0.3457, 0.3585)))
    bradford = [0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296]
    def cone(w):
        return [sum(bradford[r * 3 + c] * w[c] for c in range(3)) for r in range(3)]
    source, target = cone(white(0.3457, 0.3585)), cone(white(0.3127, 0.3290))
    diagonal = [target[r] / source[r] if r == c else 0 for r in range(3) for c in range(3)]
    adaptation = multiply(inverse(bradford), multiply(diagonal, bradford))
    return multiply(inverse(rgb_matrix(((0.708, 0.292), (0.170, 0.797), (0.131, 0.046)), (0.3127, 0.3290))), adaptation)


class RawPreviewTests(unittest.TestCase):
    width, height, stride = 11, 10, 14
    black = (4000, 7000, 3000, 8000)
    white = (19000, 23000, 16000, 27000)
    matrix = (0.55, 0.22, 0.13, 0.17, 0.68, 0.09, 0.03, 0.11, 0.73)

    def samples(self):
        return array.array("H", ((x * 7919 + y * 5987 + x * y * 677) % 41000
                                  if x < self.width else 65535
                                  for y in range(self.height) for x in range(self.stride)))

    def settings(self, area, pattern, phase, space):
        x, y, w, h = area
        return {"row_stride_samples": self.stride, "pattern": pattern,
                "cfa_phase_x": phase % 2, "cfa_phase_y": phase // 2,
                "active_x": x, "active_y": y, "active_width": w, "active_height": h,
                "black_levels": self.black, "white_levels": self.white,
                "red_gain": 1.25, "green_gain": 0.8, "blue_gain": 1.6,
                "exposure_stops": 0.5, "camera_to_xyz_d50": self.matrix,
                "working_space": space, "output_mode": "srgb-preview",
                "tone_shoulder": 0.4, "tone_gamma": 1.15, "tile_size": 50}

    def native_reference(self, samples, area, pattern, phase, space):
        ox, oy, width, height = area
        colors = ((0, 1, 1, 2), (2, 1, 1, 0), (1, 0, 2, 1), (1, 2, 0, 1))[pattern]
        matrix = [f32(v) for v in multiply(xyz_to_working(space), self.matrix)]
        def site(x, y):
            return ((y + phase // 2) % 2) * 2 + (x + phase % 2) % 2
        def sample(x, y):
            i = site(x, y)
            return f32((samples[y * self.stride + x] - self.black[i]) / (self.white[i] - self.black[i]))
        result = array.array("f")
        for y in range(oy, oy + height):
            for x in range(ox, ox + width):
                rgb = []
                for c, gain in enumerate((1.25, 0.8, 1.6)):
                    if colors[site(x, y)] == c:
                        v = sample(x, y)
                    else:
                        neighbors = [sample(sx, sy)
                                     for sy in range(max(oy, y - 1), min(oy + height, y + 2))
                                     for sx in range(max(ox, x - 1), min(ox + width, x + 2))
                                     if colors[site(sx, sy)] == c]
                        total = 0.0
                        for v in neighbors:
                            total = f32(total + v)
                        v = f32(total / len(neighbors)) if neighbors else 0.0
                    rgb.append(f32(f32(v * f32(gain)) * f32(math.sqrt(2))))
                for r in range(3):
                    products = [f32(matrix[r * 3 + c] * rgb[c]) for c in range(3)]
                    result.append(f32(f32(products[0] + products[1]) + products[2]))
        return result

    def reduced(self, pixels, width, height, mip):
        scale = 1 << mip
        w, h = (width + scale - 1) // scale, (height + scale - 1) // scale
        result = array.array("f")
        for y in range(h):
            for x in range(w):
                offsets = [(sy * width + sx) * 3
                           for sy in range(y * scale, min((y + 1) * scale, height))
                           for sx in range(x * scale, min((x + 1) * scale, width))]
                for c in range(3):
                    result.append(math.fsum(pixels[i + c] for i in offsets) / len(offsets))
        return result, w, h

    def assert_pixels(self, actual, expected):
        self.assertEqual(actual[:2], expected[:2])
        for a, b in zip(values(actual), values(expected)):
            self.assertAlmostEqual(a, b, delta=1e-6)

    def test_independent_reference_patterns_phases_and_edges(self):
        samples = self.samples()
        for space in ("prophoto-d50", "rec2020-d65"):
            for pattern in range(4):
                for phase in range(4):
                    for area in ((0, 0, 7, 5), (1, 3, 7, 5), (2, 2, 1, 5), (3, 1, 5, 1), (4, 4, 1, 1)):
                        opts = self.settings(area, pattern, phase, space)
                        native = self.native_reference(samples, area, pattern, phase, space)
                        raster_opts = {"working_space": space, "output_mode": "srgb-preview", "tone_shoulder": 0.4, "tone_gamma": 1.15}
                        native_output = raw.render(samples, self.width, self.height, opts)
                        self.assert_pixels(native_output, raw.render_raster(native, area[2], area[3], raster_opts))
                        self.assertEqual(native_output, raw.render(samples, self.width, self.height, {**opts, "mip": 0, "quality": "final", "tile_size": 1}))
                        for mip in (1, 2):
                            reduced, w, h = self.reduced(native, area[2], area[3], mip)
                            expected = raw.render_raster(reduced, w, h, raster_opts)
                            preview_opts = {**opts, "mip": mip, "quality": "preview"}
                            actual = raw.render(samples, self.width, self.height, preview_opts)
                            self.assert_pixels(actual, expected)
                            self.assertEqual(actual, raw.render(samples, self.width, self.height, {**preview_opts, "tile_size": 1}))
                            edge = {**preview_opts, "x": w - 1, "y": h - 1, "roi_width": 1, "roi_height": 1}
                            self.assertEqual(values(raw.render(samples, self.width, self.height, edge)), values(actual)[-3:])
                            empty = {**preview_opts, "roi_width": 0}
                            self.assertEqual(raw.render(samples, self.width, self.height, empty), (0, h, b""))

    def test_coordinate_defaults_gates_and_recovery(self):
        samples = self.samples()
        opts = self.settings((1, 3, 7, 5), 3, 3, "prophoto-d50")
        preview = {**opts, "mip": 1, "quality": "preview"}
        expected = raw.render(samples, self.width, self.height, preview)
        self.assertEqual(expected[:2], (4, 3))
        # A supplied reduced x leaves the default reduced extent unchanged;
        # callers choose a fitting roi_width, just as with native ROI requests.
        partial = {**preview, "x": 1, "roi_width": 3}
        result = raw.render(samples, self.width, self.height, partial)
        for y in range(3):
            self.assertEqual(values(result)[y * 9:y * 9 + 9], values(expected)[y * 12 + 3:y * 12 + 12])
        for changes in ({"mip": 1, "quality": "final"}, {"mip": 3}, {"mip": 0},
                        {"camera_to_xyz_d50": None}, {"output_mode": "legacy"},
                        {"x": 4, "roi_width": 1}, {"tile_size": 0},
                        {"active_width": 0}, {"active_x": 10}, {"crop": (1, 1, 3, 3)},
                        {"rotate": 90}, {"resize": (3, 3)}):
            with self.assertRaises((ValueError, TypeError)):
                raw.render(samples, self.width, self.height, {**preview, **changes})
        for changes in ({"mip": 1.5}, {"quality": 2}):
            with self.assertRaises(TypeError):
                raw.render(samples, self.width, self.height, {**preview, **changes})
        no_calibration = {k: v for k, v in preview.items() if k != "camera_to_xyz_d50"}
        with self.assertRaises(ValueError):
            raw.render(samples, self.width, self.height, no_calibration)
        self.assertEqual(raw.render(samples, self.width, self.height, preview), expected)
        # Full-sensor default, including a clipped odd right/bottom footprint.
        full = {k: v for k, v in preview.items() if not k.startswith("active_")}
        self.assertEqual(raw.render(samples, self.width, self.height, full)[:2], (6, 5))


if __name__ == "__main__":
    unittest.main()

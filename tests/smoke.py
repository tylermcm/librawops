import array
import math
import sys

sys.path.insert(0, sys.argv[1])
import rawengine_native as raw

width, height = 7, 5
bayer = array.array(
    "H",
    (
        1000 if y % 2 == 0 and x % 2 == 0
        else 3000 if y % 2 == 1 and x % 2 == 1
        else 2000
        for y in range(height)
        for x in range(width)
    ),
)
options = {
    "red_gain": 2.0,
    "green_gain": 1.0,
    "blue_gain": 0.5,
    "exposure_stops": 1.0,
    "tone_shoulder": 0.25,
}
w, h, output = raw.render(bayer, width, height, options)
assert (w, h, len(output)) == (width, height, width * height * 3 * 4)
full = array.array("f")
full.frombytes(output)
linear = [1000 / 65535 * 4, 2000 / 65535 * 2, 3000 / 65535]
expected = [value / (value + 0.25) for value in linear]
assert all(math.isclose(a, b, rel_tol=1e-6) for a, b in zip(full[:3], expected))

roi = {"x": 2, "y": 1, "roi_width": 3, "roi_height": 3,
       "tile_size": 2, **options}
rw, rh, cropped = raw.render(bayer, width, height, roi)
assert (rw, rh) == (3, 3)
small = array.array("f")
small.frombytes(cropped)
for y in range(rh):
    for x in range(rw):
        src = ((y + 1) * width + x + 2) * 3
        dst = (y * rw + x) * 3
        assert small[dst:dst + 3] == full[src:src + 3]

try:
    raw.render(bayer, width, height, {"x": width})
except ValueError:
    pass
else:
    raise AssertionError("out-of-range ROI accepted")

# Padded decoded RAW rows, a shifted CFA, per-site levels, and an active crop.
padded = array.array("H", [65535] * (6 * 4))
black_levels = [100, 200, 300, 400]
white_levels = [1100, 1200, 1300, 1400]
for y in (1, 2):
    for x in (1, 2):
        site = (y % 2) * 2 + ((x + 1) % 2)
        padded[y * 6 + x] = black_levels[site] + 500
pw, ph, rendered = raw.render(padded, 4, 4, {
    "row_stride_samples": 6,
    "cfa_phase_x": 1,
    "active_x": 1, "active_y": 1,
    "active_width": 2, "active_height": 2,
    "black_levels": black_levels, "white_levels": white_levels,
})
assert (pw, ph) == (2, 2)
values = array.array("f")
values.frombytes(rendered)
assert all(math.isclose(value, 2 / 3, rel_tol=1e-6) for value in values)

try:
    raw.render(bayer, width, height, {"black_levels": [1, 2, 3]})
except ValueError:
    pass
else:
    raise AssertionError("invalid per-site levels accepted")

# A calibrated camera neutral maps to neutral in either linear target space.
d50_x = 0.3457 / 0.3585
d50_z = (1 - 0.3457 - 0.3585) / 0.3585
neutral_bayer = array.array("H", [32768] * 16)
for space in ("prophoto-d50", "rec2020-d65"):
    _, _, rendered = raw.render(neutral_bayer, 4, 4, {
        "camera_to_xyz_d50": [d50_x, 0, 0, 0, 1, 0, 0, 0, d50_z],
        "working_space": space,
    })
    values = array.array("f")
    values.frombytes(rendered)
    assert all(math.isclose(v, values[0], rel_tol=1e-5) for v in values)

try:
    raw.render(neutral_bayer, 4, 4, {"working_space": "rec2020-d65"})
except ValueError:
    pass
else:
    raise AssertionError("working space without camera matrix accepted")

print("RAW engine smoke test passed")

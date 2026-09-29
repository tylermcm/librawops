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

print("RAW engine smoke test passed")

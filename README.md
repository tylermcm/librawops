# RawEngine

A small C++20 foundation for nondestructive Bayer RAW rendering. The C++ core
uses only the standard library, with optional OpenMP. The optional Python module
uses the CPython C API. No GPL or copyleft components are used by this project.
The source is MIT licensed.

## Build

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`RAWENGINE_BUILD_PYTHON=OFF` omits the Python module. `RAWENGINE_USE_OPENMP=OFF`
omits OpenMP. CMake detects Python development headers when present. Native
core tests run under CTest even when Python is disabled.

## C++ API

Construct a `RawImage` from an owned row-major `std::vector<uint16_t>` and
`RawMetadata`, then create `ImageGraph(image, recipe)`. `RawMetadata` accepts
sensor dimensions, a row stride in samples, Bayer pattern and 0/1 CFA phase,
a sensor-relative active area, and four 2×2-site black and white levels.
The old packed-buffer constructor remains available for uniform levels.
Requests use sensor coordinates and must stay within the active area; demosaic
neighbors outside that area are excluded. A zero stride means packed rows and
an all-zero active area means the whole sensor.
The graph is immutable. Create a new graph to apply another recipe; its Bayer
sample storage is shared. `Renderer::render_tiles` invokes a callback per output
tile without allocating the full output. `render_image` assembles only the requested
viewport and returns a tile with its image descriptor; `render_roi` retains the
legacy floats-only convenience result.

RAW samples are normalized relative to the declared black and white levels as
camera-linear float32 RGB. Values below 0 and above 1 are preserved through
white balance, exposure, and the signed tone curve. A separate output node
clips to `[0, 1]` and rejects non-finite values. `Tile::descriptor` identifies
camera-linear, tone-mapped, and bounded output stages. All current RGB values
retain camera-native primaries with an unspecified white point; the output is
not color-managed or ready to be labeled with a display ICC profile.

## Python API

```python
import array
import rawengine_native

samples = array.array("H", [1000, 2000, 2000, 3000])  # 2x2 RGGB
w, h, rgb_bytes = rawengine_native.render(
    samples, 2, 2,
    {"pattern": 0, "exposure_stops": 1.0, "red_gain": 1.2}
)
rgb = memoryview(rgb_bytes).cast("f")  # interleaved, native-endian float32
```

The input buffer must contain `row_stride_samples * height` contiguous
native-endian `uint16` samples, with stride defaulting to width. Options include
`x`, `y`, `roi_width`, `roi_height`, `row_stride_samples`, `cfa_phase_x`,
`cfa_phase_y`, `active_x`, `active_y`, `active_width`, `active_height`, scalar
`black_level` and `white_level`, optional
four-element `black_levels` and `white_levels` overrides, `pattern` (0 RGGB,
1 BGGR, 2 GRBG, 3 GBRG), `tile_size`, white balance gains, `exposure_stops`,
`tone_shoulder`, and `tone_gamma`. With an active area and no explicit ROI,
the ROI defaults to that area. Python's convenience call returns a materialized
ROI and copies the input; the C++ tile callback is the low-memory interface.

## Scope

This implements a clean RAW input boundary, basic bilinear demosaic, white
balance, exposure, and a highlight-compressing tone curve. It does not yet
implement the complete Camera Raw control set, camera profiles and color-space
transforms, lens corrections, denoise, sharpening, or color-managed export.
Input decoding stays behind the decoded-RAW boundary. Any optional file
decoder must pass the separate no-copyleft dependency gate in the plan.

## Native benchmark

Configure with `-DRAWENGINE_BUILD_BENCHMARK=ON`, then run
`build/Release/RawEngineBenchmark.exe 7500 6000 3 256` on Windows. Arguments
are width, height, repetitions, and tile size. The tool prints JSON medians
for a 1024×768 materialized ROI and a full-image **streaming** render. It uses
a synthetic Bayer source and does not decode files, build reduced previews,
encode exports, or measure Adobe compatibility. Record the machine, compiler,
power mode, and build configuration alongside the JSON output.

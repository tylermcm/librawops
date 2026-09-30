# RawEngine

A small C++20 foundation for nondestructive Bayer RAW rendering. The C++ core
uses only the standard library, with optional OpenMP. The optional Python module
uses the CPython C API. No GPL or copyleft components are used by this project.
The source is MIT licensed.

For a new development run or a different workstation, read and update the
[current handoff](docs/plan/LIBRAWOPS_PLAN.md#current-handoff--read-first)
in the single living plan file before continuing implementation.

## Build

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`RAWENGINE_BUILD_PYTHON=OFF` omits the Python module. `RAWENGINE_USE_OPENMP=OFF`
omits OpenMP. CMake detects Python development headers when present. Native
core tests run under CTest even when Python is disabled.

`RAWENGINE_WITH_LCMS=ON` opts into the pinned LittleCMS 2.19.1 core for ICC
display/output transforms. CMake downloads the SHA-256-checked upstream archive.
The build disables LittleCMS tools, tests, optional codecs, and its GPL fast-float
and threaded plugins. The default build has no LittleCMS dependency. The
LittleCMS MIT notice is installed with the library.

## C++ API

Construct a `RawImage` from an owned row-major `std::vector<uint16_t>` and
`RawMetadata`, then create `ImageGraph(image, recipe)`. `RawMetadata` accepts
sensor dimensions, a row stride in samples, Bayer pattern and 0/1 CFA phase,
a sensor-relative active area, and four 2×2-site black and white levels.
The old packed-buffer constructor remains available for uniform levels.
Requests use sensor coordinates and must stay within the active area; demosaic
neighbors outside that area are excluded. A zero stride means packed rows and
an all-zero active area means the whole sensor.
For already decoded scene-linear RGB, construct a `RasterImage` from
`RasterMetadata` and an owned interleaved `std::vector<float>`. Declare
`LinearProPhotoD50` or `LinearRec2020D65`; the default is linear ProPhoto/D50
and a zero row stride means packed rows.
Raster source pixels may be negative or above 1 but must be finite. The raster
graph accepts exposure and tone controls, and rejects RAW white-balance gains
and camera calibration matrices.
The graph is immutable. Create a new graph to apply another recipe; its Bayer
or raster sample storage is shared. `Renderer::render_tiles` invokes a callback per output
tile without allocating the full output. `render_image` assembles only the requested
viewport and returns a tile with its image descriptor; `render_roi` retains the
legacy floats-only convenience result.

For versioned edits, serialize an `EditManifest` and build an
`ExecutableEditGraph` from its saved source records and runtime source nodes.
Set each `EditSource::content_sha256` with `fingerprint_raw_source`,
`fingerprint_raster_source`, or (with LittleCMS) `fingerprint_icc_raster_source`.
Construction checks the runtime source fingerprint, bounds, color domain, and
ICC policy against the manifest. The version-1 digests use SHA-256 over a
type/version tag, rendering metadata, and visible samples in row-major order.
Integer and float bits are encoded little-endian; row padding is excluded.
For Bayer sources only the active area is sampled, since demosaic neighbors
outside it are never read. ICC-encoded raster fingerprints cover encoded pixels;
the profile bytes and transform policy have separate saved identities. Digests
are memoized by immutable source objects after their first calculation.

Pass an optional shared `TileCache` as the fourth `ExecutableEditGraph`
constructor argument to reuse unchanged upstream tiles across graph revisions.
The process-local LRU cache keys include source and operation identity, versions,
color policy, and tile bounds. Its byte budget charges pixel storage plus a
fixed allowance per entry; oversized tiles bypass it. `clear()` invalidates
entries, including work that was rendering when clear was called. This is a
same-process render cache, not a persistent disk cache or a complete scheduler.
The optional `CancellationToken` parameter on renderer calls aborts at tile
boundaries with `RenderCancelled`. For queued work, `TileScheduler` runs
requests from retained node handles with background, normal, or interactive
priority. It bounds pending requests, runs FIFO within a priority, and returns
`std::future<Tile>`; running requests are not preempted. A host can pass
`ExecutableEditGraph::output_handle()` and `source_bounds()` to `submit()`.
Use `submit_latest(group, ...)` for a viewport or slider request stream: a
new request in the same group removes an older queued request and cancels a
running one at its next tile boundary. Superseded futures raise
`RenderCancelled`. The group name is chosen by the host; different groups do
not supersede one another.

`BoxBlurNode` and the `rawengine.box_blur` manifest operation provide a small
scene-linear neighborhood example with an integer radius from 1 to 8.
`Node::input_region()` reports the source-clipped halo needed for an output
rectangle. The blur requests that upstream region and crops its result back
to the output rectangle; tiled and full renders use the same edge rule. This
is a reference spatial operation, not a production blur or a general
coordinate-transform/mipmap system.

RAW samples are normalized relative to the declared black and white levels as
camera-linear float32 RGB. Values below 0 and above 1 are preserved through
white balance, exposure, optional camera color conversion, and the signed tone
curve. A separate output node clips to `[0, 1]` and rejects non-finite values.
`Tile::descriptor` identifies camera-linear, scene-linear working, tone-mapped,
and bounded output stages, including declared primaries and white point.

Set `GraphRecipe::camera_color` to a row-major 3×3 matrix from **already
white-balanced** camera RGB to XYZ D50 (diffuse white Y=1) and choose
`LinearProPhotoD50` or `LinearRec2020D65`. The transform and Bradford white
adaptation run in linear light without clipping. A raw DNG `ForwardMatrix`
is not necessarily this matrix: camera calibration and white-balance semantics
must be resolved by the caller. Without a matrix, the legacy camera-native
path remains available. The default bounded result is still **not display-ready**:
the current tone curve is a prototype, and no display ICC conversion, output
transfer function, or gamut mapping is applied. Its descriptor records the
underlying primaries even after the tone curve.
Linear ProPhoto/D50 is the current canonical editing default; Rec.2020/D65
remains an explicit supported domain. `WorkingSpaceConvertNode` converts
between them without clipping signed or over-range values.

Set `GraphRecipe::output_mode = OutputMode::SrgbPreview` for an explicitly
tagged sRGB preview. This requires declared scene-linear RGB: a raster working
space or a RAW camera matrix. The graph converts to linear sRGB, applies the
current prototype tone curve to produce display-linear sRGB, clips
negative/out-of-gamut channels, and applies
the standard sRGB transfer function. The result is encoded float32 RGB in
`[0, 1]` with `ImageDescriptor::srgb_output()`. It is intended for an sRGB
consumer; this mode does not inspect a monitor ICC profile, use a configurable
rendering intent, or perform perceptual gamut mapping. The default
`LegacyBounded` mode retains the prior output and is not display encoded.

For a C++ host, set
`output_mode = OutputMode::IccDisplay` and pass a thread-safe
`IccDisplayTransform` to the `ImageGraph` constructor. The adapter receives
display-linear sRGB float32 tiles after tone mapping and must return finite,
bounded RGB encoded for one exact output profile. It reports the SHA-256 of
that profile's bytes; the hash is retained in `Tile::descriptor` so outputs
from different profiles cannot be confused. The core checks domain, profile
identity, and pixel bounds. With `RAWENGINE_WITH_LCMS=ON`, include
`LittleCmsBackend.hpp` and call `make_lcms_display_transform(profile_bytes,
options)` to use the built-in LittleCMS core adapter. It accepts RGB
display/output profiles, offers the four ICC intents and optional black-point
compensation, and hashes the exact profile bytes. Its current out-of-gamut
policy is hard clipping to `[0, 1]`; soft proofing and gamut warnings are not
implemented. The factory is unavailable in the default build.
The Python one-shot API does not yet accept a host ICC adapter.

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
For color conversion, pass `camera_to_xyz_d50` as nine row-major numbers and
`working_space` as `"prophoto-d50"` or `"rec2020-d65"` (the former is the
default when a matrix is supplied). `working_space` alone is rejected.
Both render calls accept `output_mode="srgb-preview"` for encoded sRGB float32
output; `"legacy"` is the default. RAW sRGB preview requires
`camera_to_xyz_d50`. The result tuple does not carry the C++ image descriptor,
so callers must retain their chosen output mode alongside the returned bytes.
`rawengine_native.render_raster(rgb, width, height, options)` accepts a
contiguous native-endian interleaved float32 RGB buffer already in scene-linear
light. Its required `working_space` option declares `"prophoto-d50"` or
`"rec2020-d65"`; optional `row_stride_pixels`, ROI, tile, exposure, and tone
settings follow the same conventions. It also returns `(width, height,
float32_rgb_bytes)` and materializes only the requested ROI.

## Scope

This implements a clean RAW input boundary, basic bilinear demosaic, white
balance, exposure, an explicit camera-to-working-space matrix, a
highlight-compressing tone curve, a typed scene-linear raster memory source,
optional ICC raster import, a versioned unary edit graph, bounded tile cache,
tile-boundary cancellation, a basic priority request queue, and a reference
box blur with clipped halo requests. It does not
yet implement the complete Camera Raw control set, DNG profile interpretation,
JPEG/PNG/TIFF file decoding, lens corrections, denoise, sharpening, or
color-managed export.
It also lacks monitor-profile previews and configurable output profiles.
Input decoding stays behind the decoded-RAW boundary. Any optional file
decoder must pass the separate no-copyleft dependency gate in the plan.

## Native benchmark

Configure with `-DRAWENGINE_BUILD_BENCHMARK=ON`, then run
`build/Release/RawEngineBenchmark.exe 7500 6000 3 256 legacy` on Windows.
Arguments are width, height, repetitions, tile size, and mode (`legacy` or
`srgb-preview`). The preview mode includes a synthetic camera-to-Rec.2020
matrix and the full sRGB preview chain. The tool prints JSON medians
for a 1024×768 materialized ROI and a full-image **streaming** render. It uses
a synthetic Bayer source and does not decode files, build reduced previews,
encode exports, or measure Adobe compatibility. Record the machine, compiler,
power mode, and build configuration alongside the JSON output.
`RawEngineColorBenchmark` uses tile-generated scene-linear pixels to compare
ProPhoto and Rec.2020 working-space conversion cost without a full-image
working buffer. It reports source and converted streaming times; those timing
differences are indicative, not a fit-preview or export latency measurement.
`RawEngineCacheBenchmark` separately measures a synthetic 2048×1536
scene-linear raster, 1024×768 tiled ROI, first source fingerprint, cold and
warm render, and graph rebuild/first render after a late tone edit. Its output
is a narrow cache measurement, not fit preview or export latency.

## Reference fixtures

Run `python tools/reference_harness.py generate build/reference-fixtures` to
create deterministic 16-bit RGB TIFF ramps, color patches, edges, a manifest,
and an Adobe capture template. The generated TIFFs embed the pinned ICC
`sRGB2014.icc` profile. Run
`python tools/reference_harness.py compare first.tif second.tif --diff diff.tif`
to report code-value error metrics and save an exact absolute-difference TIFF.
The comparison reads rows rather than materializing the whole image. It
requires matching embedded ICC bytes and upright, uncompressed, interleaved
16-bit RGB; unsupported TIFF layouts fail with an error. Use `--capture` and
`--capture-source` with completed capture metadata to link a future Adobe
reference to its source and output hashes. This harness contains **no Adobe
measurements yet** and the library has no TIFF export adapter. The capture
workflow and limitations are in the [single living plan](docs/plan/LIBRAWOPS_PLAN.md#adobe-compatibility-method-and-evidence-ledger).

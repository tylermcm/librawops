# RawEngine

A small C++20 foundation for nondestructive Bayer RAW rendering. The C++ core
uses only the standard library, with optional OpenMP. The optional Python module
uses the CPython C API. No GPL or copyleft components are used by this project.
The source is MIT licensed.

For a new development run or a different workstation, read and update the
[current handoff](docs/plan/LIBRAWOPS_PLAN.md#current-handoff--read-first)
in the single living plan file before continuing implementation.

The viewer is a local development test harness; UI work is limited to engine
testing needs. It and its GUI/decoder runtimes will not ship alongside the library.
For a plain local NEF test window, double-click `tools/launch_raw_viewer.cmd`.
It provides Fit/100% views, basic adjustments, demosaicer selection and Before/Reset
using the existing external decoder and native engine runtimes. See the
[viewer instructions](docs/RAW_TEST_VIEWER.md); this optional tool is separate
from the engine install.

## Build

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`RAWENGINE_BUILD_PYTHON=OFF` omits the Python module. `RAWENGINE_USE_OPENMP=OFF`
omits OpenMP. CMake detects Python 3.9+ development headers when present. Native
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
a sensor-relative active area, and four 2Ã—2-site black and white levels.
The old packed-buffer constructor remains available for uniform levels.
Native requests use sensor coordinates and must stay within the active area; demosaic
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
`RenderRequest` groups viewport, tile size, mip and quality for renderer and
scheduler calls. Raster source nodes accept mip 0 at `Final` or `Preview`
quality (identical samples), and mip 1 or 2 at `Preview` quality. At mip m,
the scale is 2^m and output dimensions are ceil(width/scale) Ã—
ceil(height/scale). Output pixel (x,y) averages its direct scaleÃ—scale
source footprint starting at (scale*x,scale*y) in scene-linear light. Its
nominal source-center is (scale*x+(scale-1)/2,scale*y+(scale-1)/2); edge
footprints divide by their actual source-sample count. Mip 2 averages
original source samples directly, preserving equal weight at odd edges.
Viewports and tile sizes use reduced pixels.
The explicit sRGB output chain also accepts mip 1/2 preview: working-space
conversion to linear sRGB, prototype tone mapping, then hard clipping and sRGB
encoding all run after source reduction. The output retains its encoded sRGB
descriptor. This applies to raster `ImageGraph` with `SrgbPreview` and executable
edit graphs built from supported reduced operations. Reduction before nonlinear
tone/encoding differs from averaging a full-resolution encoded render; it is
not an exact thumbnail of final output. Calibrated RAW `ImageGraph` with
`SrgbPreview` and typed RAW edit graphs also support mip-1/2 preview. Their
reduction anchor averages native demosaiced, white-balanced, calibrated
scene-linear float32 RGB before downstream edits and nonlinear output. The
compatibility recipe keeps exposure before camera calibration, so that exposure
also runs natively before averaging. An edit graph may place scene-linear
exposure after calibration to process reduced pixels.

RAW previews use zero-based coordinates within the active area. Pixel (x,y)
averages the native footprint beginning at
`(active_x + scale*x, active_y + scale*y)`, clipped to the active area, with
dimensions `ceil(active_width/scale)` Ã— `ceil(active_height/scale)`. CFA phase
and site levels continue to use original sensor coordinates. Source planning
maps these groups back to sensor coordinates and adds the clipped one-pixel
demosaic halo. Native-final coordinates and samples retain their existing
behavior. Direct `CameraToWorkingNode` callers supply the third
`native_input_bounds` argument to enable this bounded preview contract; omitting
it keeps native-only behavior. WB and unpack execute natively upstream of this
anchor. This bilinear reference path still processes native pixels and its
temporary tiles are outside the cache budget. It supplies no RAW-file decoder
or production demosaic quality claim.

Uncalibrated RAW, ICC import/display and unmanaged tone/output paths still
reject reduced requests. Python RAW and raster calls accept mip-1/2 sRGB previews.
Other reduced paths reject before rendering or queueing. Existing rectangle
overloads remain native final requests.

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
color policy, tile bounds, mip and quality. A direct `TileCache::render` caller
must supply a node whose coordinates and processing match its `RenderLevel`.
Graph-backed raster source caching separates native and reduced levels.
Scene-linear exposure and working-space conversion can run after source
reduction at mip 1 or 2 preview, with each stage cached under its own level.
`BoxBlurNode` also runs after reduction; its radius and clipped halo are
measured in reduced pixels. The graph's level-aware source-region planner
maps that halo back to the original source footprint.
`ExecutableEditGraph::required_source_region(roi, level)` maps supported
point, blur and crop ROIs to full-resolution source footprints. Calibrated RAW
previews include sensor offsets and native demosaic halos. ICC display,
unmanaged tone/output and other unsupported nodes still accept native final
requests only. The cache byte
budget charges pixel storage plus a
fixed allowance per entry; oversized tiles bypass it. `clear()` invalidates
entries, including work that was rendering when clear was called. This is a
same-process render cache, not a persistent disk cache or a complete scheduler.
The optional `CancellationToken` parameter on renderer calls aborts at tile
boundaries with `RenderCancelled`. For queued work, `TileScheduler` runs
requests from retained node handles with background, normal, or interactive
priority. It bounds pending requests, runs FIFO within a priority, and returns
`std::future<Tile>`; running requests are not preempted. A host can pass
`ExecutableEditGraph::output_handle()` and `output_bounds()` to `submit()`.
For outputs depending on one source ID, the original extent remains available through `source_bounds()`;
rendering uses the transformed output extent.
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
coordinate-transform/mipmap system. `RawUnpackNode` reports a one-pixel
input halo for bilinear and a six-pixel halo for Menon base (one for singleton
axes), clipped to the true active area. For built-in unary edit graphs,
`ExecutableEditGraph::required_source_region()` composes these regions through
the chain, including point operations and cached wrappers.

`LinearMixNode` and schema-1 `rawengine.linear_mix` provide the first bounded
two-input reference operation. Its named `base` and `layer` ports must have
matching native extents and the same linear ProPhoto or Rec.2020 descriptor.
The sole parameter, `amount` in `[0, 1]`, computes
`(1 - amount) * base + amount * layer`, preserving signed values and headroom.
Endpoints reproduce their input exactly. Both branches render at native final
or supported mip-1/2 preview; this operation has no alpha, masks or layer blend
modes. A disabled mix bypasses to `base`. Both upstream signatures participate
in its cache identity, so edits to either branch refresh the combined result
while retaining unchanged upstream tiles.

Use `graph.required_source_regions(roi, level)` for a source-ID-to-native-ROI
map. It follows both branches through crop and blur coordinate/level changes,
and unions footprints when paths share a source ID. Each value is a conservative
bounding rectangle, not a disjoint region list. Only sources reachable from the
selected output are included; disabled mixes require only their base branch.
The singular `required_source_region()` and `source_bounds()` helpers reject
outputs that depend on multiple source IDs. Rendering and scheduling always use
`output_bounds()`. Python saved-manifest calls can execute linear mix through
`RasterSession` branches or independently owned `RasterGraphSession` sources.
The recipe API has no mix control.

`CropNode` and the schema-1/current-processing `rawengine.crop` operation
accept integer `x`, `y`, `width`, and `height` in upstream native coordinates.
They require a nonempty rectangle inside scene-linear input and rebase output
coordinates to zero without resampling native pixels. `output_bounds()` gives
the resulting extent. Disabled crop operations preserve upstream bounds.
The graph planner tracks each stage's input extent, so nested crops and blur
halos compose in the correct coordinate system.

For mip 1/2 preview, a crop is a reduction anchor: it requests native upstream
pixels and averages direct clipped 2Ã—2/4Ã—4 footprints starting at the crop
origin. Edge footprints divide by their actual sample count. This is equivalent
to extracting native scene-linear crop pixels into a new source, then reducing
that source before downstream operations. Blur before a crop runs natively;
blur after it uses reduced pixels. With nested crops, the last crop supplies
the reduction anchor. This can differ from cropping an already reduced
uncropped preview. `Node::input_level()` declares changes in upstream level;
ordinary nodes preserve it and cached wrappers delegate it. Crop reduction
requires an upstream chain that supports preview, including bounded calibrated
RAW; uncalibrated RAW and ICC reduced-input gates remain. Arbitrary-angle rotation and perspective remain
future geometry work.

`ResizeNode` and schema-1 `rawengine.resize` implement a scene-linear reference
resampler in linear ProPhoto or Rec.2020. Manifest parameters are positive
integer `width`/`height` and `filter` (`nearest`, `bilinear` or `area`). Output coordinates
start at zero. An output center maps to input coordinate
`(output + 0.5) * input_extent / output_extent - 0.5`, clamped to the edge pixel
centers; nearest rounds half positions upward, while bilinear uses adjacent
pixels. Signed values and headroom are preserved; identity resize reproduces
input samples exactly. The ROI planner includes the interpolation footprint
and composes it through crop, blur and mix branches. Filter/size changes have
distinct cache identities. Disabled resize preserves its upstream extent.

Nearest and bilinear can alias fine detail when downsampling. The `area`
filter integrates piecewise-constant source pixel cells over each output
footprint `[output * input_extent / output_extent, (output + 1) * input_extent / output_extent)`.
Weights are the horizontal/vertical cell overlaps, normalized by footprint
area. Integer rational boundaries exclude zero-overlap neighbors exactly;
double accumulation produces float32 output. The same rule handles enlargement,
identity and one-pixel outputs. This is a box-area reference filter; sharper
production reconstruction filters and arbitrary-angle rotation/perspective
remain future work.

All three resize filters support mip-1/2 preview when the upstream chain
supports preview, including bounded calibrated RAW. Resize requests native upstream work, renders the
native resized scene-linear footprint and averages its float32 pixels in direct
clipped 2Ã—2/4Ã—4 groups, dividing by actual sample counts at odd edges.
Downstream tone/clipping/encoding run after reduction. This equals separately
materializing the native resized linear image and reducing it; it can differ
from resizing directly to the reduced dimensions or averaging encoded output.
Spatial edits before resize run natively; blur after resize uses reduced-pixel
radius. The last crop/orientation/resize reduction anchor sets the averaging grid.
Source planning composes the native sampling footprint through earlier stages,
and cache identity separates native/mip levels. Uncalibrated RAW and ICC
reduced-input gates remain enforced. Mip 0 preview and mip-1/2 final still reject. Native resized
temporary tiles are outside the cache budget, and this reference path processes
native output samples before averaging; it is not a fast direct-resize shortcut.

`OrientationNode` and schema-1 `rawengine.orientation` perform exact clockwise
quarter turns, followed by horizontal/vertical flips in the rotated coordinates.
Manifest parameters are integer `quarter_turns` in `[0, 3]` and boolean
`flip_horizontal`/`flip_vertical`. Output bounds start at zero; 90Â°/270Â° swap
width and height. Native pixels, including signed zero, are copied without
interpolation or color changes. The integer inverse map transforms requested
rectangles into upstream coordinates, including nonzero input origins, and
composes with crop/resize/blur and multiple-source planning. Parameters take
part in cache identity; disabled orientation preserves upstream bounds.

Mip-1/2 orientation previews average direct clipped 2Ã—2/4Ã—4 transformed native
pixels from the output origin before downstream tone/encoding. At odd edges
they divide by actual sample counts. This equals orienting a native linear
buffer and then reducing it; rotating/flipping an already reduced preview can
differ because its edge groups were anchored differently. Like crop and resize,
orientation requests native upstream work; bounded calibrated RAW is supported,
while uncalibrated RAW and ICC reduced gates remain.
The last crop/orientation/resize anchor determines the averaging grid. Perspective
and EXIF/file orientation handling remain separate gates.

`GeometryOps.hpp` exports `RotateSettings`, `validate_rotate_settings` and
`RotateNode` for centered, clockwise arbitrary-angle rotation/straightening.
The saved type is `rawengine.rotate`, schema1/processing2, with exactly one
finite numeric `angle_degrees` in `[-180,180]`. It preserves the current W/H
canvas and rebases enabled output to zero; disabled output retains upstream
bounds. Native identity preserves bits. Signed/headroom RGB uses ordered
bilinear interpolation with replicated true image borders. Rotated corners
can streak and the fixed canvas crops content; bilinear checker aliasing and
detail smoothing remain declared limitations.

Native Final and mip1/2 Preview follow the existing geometry anchor: rotate
once-rounded native pixels before direct 2x2/4x4 output-cell averaging. Native
Preview and reduced Final reject. Exact floating tap scans compose source
footprints through earlier geometry/RAW halos. Rendering uses at most128-square
native output blocks and257-square source rectangles; logical source/scratch
payload is at most989,196 bytes per block, excluding caller output, upstream
work/cache and allocator/global accounting. Platform math-runtime rounding can
differ; cross-libm exact pixels are not promised. Saved graphs work through
the existing session/jobs/history/analysis APIs; no recipe default or UI changes.
See [the frozen rotation contract](docs/ROTATE_CONTRACT_V1.md).

`GeometryOps.hpp` also exports `ProjectiveSettings`, its validator and
`ProjectiveNode` for an explicit positive output `width`/`height` and copied
row-major inverse `source_from_output` matrix. The saved type
`rawengine.projective` uses exactly those three parameters, schema1/processing2.
Coordinates are normalized local pixel centers with endpoint alignment;
identity at equal extents preserves native bits. Finite coefficients in [-16,16],
m22=1, absolute determinant at least2^-20 and denominator corner margins at
least0.25 bound the admitted manual mapping. Invalid disabled parameters reject.

It uses the same scene-linear signed/headroom bilinear and native-before-mip
rules as rotation. Exact source tap scans drive whole-output-cell block splitting
until each source fetch is at most257-square; oversized single preview cells
use ordered tiny-row fallback. Logical source/native scratch remains at most
989,196 bytes, with caller output/upstream/cache/allocator budgets separate.
Replicated corner streaks, cropping and minification aliasing are declared limits.
This provides manual perspective/reflection/affine mapping without matrix fitting,
automatic crop, alpha/fill, production quality qualification or UI changes.
See [the frozen projective contract](docs/PROJECTIVE_CONTRACT_V1.md).

`GeometryOps.hpp` exports `CubicResizeSettings`, its bounds-aware validator and
`CubicResizeNode` for scale-aware Catmull-Rom reconstruction. The saved type
`rawengine.cubic_resize` uses exactly positive uint32 `width,height`,
schema1/processing2. Each axis admits at most fourfold native shrink; invalid
relative canvases or unsupported working domains reject even when disabled.
Existing nearest/bilinear/area resize and geometry defaults retain their behavior.

Replicated true borders and ordered normalized double taps preserve native
identity/constants. Horizontal values remain double until final vertical
reconstruction; native float32 output is rounded once before direct mip1/2
averaging. Negative lobes can ring or overflow finite float32 and then reject;
signed/headroom values are preserved. Rendering caps each source at145-square
and native scratch at32-square, or264,588 logical pixel bytes; fixed axis tables,
caller output/upstream/cache/allocator budgets are additional. This is a bounded
sharper-resampling foundation with declared grain/ringing/aliasing tradeoffs.
See [the frozen cubic contract](docs/CUBIC_RESIZE_CONTRACT_V1.md).

RAW samples are normalized relative to the declared black and white levels as
camera-linear float32 RGB. Values below 0 and above 1 are preserved through
white balance, exposure, optional camera color conversion, and the signed tone
curve. A separate output node clips to `[0, 1]` and rejects non-finite values.
`Tile::descriptor` identifies camera-linear, scene-linear working, tone-mapped,
and bounded output stages, including declared primaries and white point.

Set `GraphRecipe::camera_color` to a row-major 3Ã—3 matrix from **already
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
With both `RAWENGINE_WITH_LCMS=ON` and `RAWENGINE_BUILD_PYTHON=ON`, Python exposes
owned ICC profiles through `create_icc_profile` and reports backend availability
through `icc_available()`. Supply exact profile bytes and explicit intent/BPC;
`icc_profile_info` returns copied digest/policy metadata. No monitor/profile is
inferred. See the [Python ICC ownership and API contract](docs/PYTHON_ICC_CONTRACT_V1.md).

```python
profile = rawengine_native.create_icc_profile(
    profile_bytes, intent="relative_colorimetric", black_point_compensation=False)
session = rawengine_native.RasterSession(
    scene_linear_float32_rgb, width, height, "prophoto-d50", output_profile=profile)
manifest = session.export_manifest({"output_mode": "icc-display"})
pixels = session.render_manifest(manifest)
history = session.history(manifest)
```

For profile-encoded RGB uint16 input, pass `input_profile=profile` to RasterSession
or include it in a RasterGraphSession source spec; working space remains explicit.
Without input_profile the existing float32 source contract is unchanged. RawSession
and RasterGraphSession also accept a fixed output_profile. One-shot render options
accept output_profile with `output_mode="icc-display"`; render_raster options also
accept input_profile with the same uint16 semantics. Profile handles and source
buffers may be discarded after construction: graphs/jobs/history retain their
native ownership. Saved JSON records identities, not profile bytes, so restoration
requires matching sources and output profile. ICC input/output remain native Final
only. Backend-off builds keep profile functions available and reject profile
creation with RuntimeError. Arbitrary host ICC callback adapters are C++ only.

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

Raster options also accept `mip` (0, 1, or 2; default 0) and `quality`
(`"final"` or `"preview"`; default `"final"`). Mip 1/2 requires both
`quality="preview"` and `output_mode="srgb-preview"`. The default viewport is
the full reduced image, with dimensions `ceil(width / 2**mip)` Ã—
`ceil(height / 2**mip)`. ROI coordinates and tile sizes use reduced pixels;
the input buffer and row stride always describe the original image. ROI
dimensions default to the full level dimensions, so set them explicitly for
a nonzero origin. Mip 0 preview quality and reduced final quality reject.
Empty ROIs return empty bytes, preserving the C++ behavior. RAW `render`
also accepts mip 1/2 with `quality="preview"`, a valid `camera_to_xyz_d50`
matrix and `output_mode="srgb-preview"`. Reduced defaults cover the zero-based
active-area preview; explicit ROI and tile size use reduced pixels. Native-final
ROIs remain sensor-relative. For example:

```python
preview = raw.render(bayer, sensor_width, sensor_height, {
    "camera_to_xyz_d50": calibrated_matrix,
    "output_mode": "srgb-preview", "mip": 2, "quality": "preview",
    "active_x": 1, "active_y": 3, "active_width": 7, "active_height": 5,
})  # 2 x 2 output, anchored at sensor (1,3)
```

Unsupported levels raise an exception rather than
silently producing native pixels.

Raster options can include `crop=(x, y, width, height)` in original-source
native pixels, before exposure, tone and output encoding. It applies to
`render_raster`, `RasterSession.render`, `submit` and `submit_latest`. Default
ROI dimensions then describe the cropped output at the requested mip, with
zero-based coordinates. Source dimensions and row stride still describe the
original buffer. `crop=None` or omission preserves the full source; crops are
not inherited across session calls. RAW `render` rejects this raster option.

Raster calls also accept `resize=(width, height)` and optional
`resize_filter="bilinear"` (default), `"nearest"` or `"area"`. Resize follows crop/orientation and
precedes exposure/tone/output, so filtering uses scene-linear pixels. ROI and
job progress use the resized output dimensions; source stride/dimensions still
describe the original buffer. Resize is per-call, and omission or `None`
restores the upstream dimensions. A filter option requires an active resize.
Resize supports native final or mip-1/2 preview with `output_mode="srgb-preview"`;
RAW one-shot calls reject it.

Use `rotate=0`, `90`, `180` or `270` for clockwise degrees and boolean
`flip_horizontal`/`flip_vertical` for flips after rotation. The recipe order is
crop in original-source coordinates â†’ rotation â†’ flips â†’ resize â†’ exposure,
tone and output. ROI defaults and job progress describe the resulting native
or reduced output. Orientation options are per-call; omitted options reset to
zero rotation/no flips. Floats, negative/arbitrary angles and non-boolean flips
reject before scheduling. These options work in one-shot/session sync/async
raster calls; RAW one-shot rejects them.

```python
session = rawengine_native.RasterSession(linear_rgb, width, height, "prophoto-d50")
options = {"crop": (10, 10, 800, 600), "resize": (400, 300),
           "rotate": 90, "flip_horizontal": True,
           "resize_filter": "area", "output_mode": "srgb-preview"}
job = session.submit_latest("resized-viewport", options)
w, h, rgb_bytes = job.result()
```

For repeated decoded-Bayer renders, retain a `RawSession`:

```python
session = rawengine_native.RawSession(bayer, sensor_width, sensor_height, {
    "row_stride_samples": sensor_stride,
    "pattern": 0, "cfa_phase_x": 0, "cfa_phase_y": 0,
    "active_x": 1, "active_y": 3, "active_width": 7, "active_height": 5,
    "black_levels": (4000, 7000, 3000, 8000),
    "white_levels": (19000, 23000, 16000, 27000),
}, cache_bytes=64 * 1024 * 1024, workers=1, max_pending=8)
recipe = {"camera_to_xyz_d50": calibrated_matrix,
          "working_space": "prophoto-d50", "output_mode": "srgb-preview",
          "red_gain": 1.25, "green_gain": 0.8, "blue_gain": 1.6}
request = {"mip": 2, "quality": "preview", "tile_size": 256}
job = session.submit_latest("viewport", {**recipe, **request})
w, h, rgb_bytes = job.result()  # 2 x 2 preview anchored at sensor (1,3)
saved = session.export_manifest(recipe)
assert session.render_manifest(saved, request) == (w, h, rgb_bytes)
history = session.history(saved)
history.commit(session.export_manifest({**recipe, "tone_shoulder": 0.8}))
history.undo()
print(session.source_info(), session.cache_stats())
session.close()
history.close()
```

`RawSession(bayer, width, height, metadata=None, *, cache_bytes=67108864,
workers=1, max_pending=8)` copies, validates and fingerprints decoded native-endian
uint16 Bayer samples once. Dimensions describe the sensor. Metadata accepts
the same stride, pattern, CFA phase, active-area and black/white-level fields as
one-shot RAW rendering; uniform `black_level`/`white_level` defaults are 0/65535,
and four-site arrays override them. An omitted metadata dict uses packed RGGB
and the full sensor. Source samples and metadata are immutable and owned;
changing or deleting the caller's buffer/dict has no effect. Constructor metadata
rejects unknown fields and recipe controls.

Each `render`, `submit` or `submit_latest` call supplies a complete recipe:
WB gains, calibrated camera matrix/target working space, exposure, tone and
output policy reset to their defaults on every call. Native stage order matches
one-shot RAW: demosaic â†’ WB â†’ exposure â†’ optional camera calibration â†’ output.
Sensor metadata and budgets cannot change through a recipe. Native-final default
ROI covers the active area in sensor coordinates; mip-1/2 preview defaults cover
its zero-based reduced extent. Explicit ROI sizes and tile sizes use the requested
level. Reduced recipes require valid calibration and sRGB preview output.
RAW recipe geometry is still rejected; saved typed manifests can use the
supported calibrated crop/orientation/resize and scene-linear graph operations.

RawSession exposes the same cache, RenderJob, manifest export/replay,
source-footprint inspection and bounded history methods as RasterSession.
`source_info()` returns copied `id`, `kind="decoded_bayer_u16"`, `content_sha256`,
sensor dimensions, normalized row stride, pattern/phase, active-area tuple and
site-level tuples and a copied `demosaic` policy. New RAW recipes export format 3
with source ID/kind/fingerprint plus `demosaic={"algorithm":"rawengine.bilinear",
"processing_version":1}`. Pass that same dictionary as the keyword-only
`RawSession(..., demosaic=...)` constructor option; `None` pins the same policy.
Bilinear version 1 remains the default. Menon base version 1 is available as
an opt-in reconstruction policy. Unknown algorithm/version,
extra policy fields and missing format-3 RAW policy reject before rendering.
Formats 1/2 permanently imply bilinear version 1; explicit policy is forbidden
in those older formats. Raster recipes continue exporting format 2.
Select the native Menon base implementation at source construction:

```python
session = raw.RawSession(
    bayer, width, height, metadata,
    demosaic={"algorithm": "rawengine.menon_base", "processing_version": 1},
)
preview = session.render(recipe)  # Existing WB/calibration/output and mip controls.
```

In C++, construct `RawUnpackNode(image, {"rawengine.menon_base", 1})` and
use the existing node/graph renderer. The reconstruction uses normalized
sensor-site float32 samples, double intermediates, horizontal ties, and the
original Menon base stages and true-image mirror/zero boundary rules. It preserves
observed samples, signed shadows and headroom. Singleton active axes use the
existing valid-neighbor bilinear behavior. A composed six-pixel halo participates
in source-footprint planning. Scratch processing is internally divided into at
most 256Ã—256 output blocks, even for a direct large node request; scratch planes
use at most approximately 5.1 MiB per concurrent reconstruction. Returned pixels,
source storage, cache and renderer/job state are separate memory costs.

Only Menon **base** version 1 is integrated; refinement has a separate research
prototype. Existing one-shot/legacy recipes retain bilinear. Menon sessions support
format-3 replay, native/calibrated reduced rendering, caches, async jobs and history;
a manifest bound to a different reconstruction policy is rejected. This is a
prototype candidate with frozen synthetic/photographic parity evidence. Actual-camera
quality, calibration coverage, noise/alias artifacts and release/shipment disposition
remain open. See [the native contract and verification](docs/research/MENON_ENGINE_V1.md).
The BSD notice is retained at `third_party/colour-demosaicing/LICENSE` and installed
as `share/licenses/RawEngine/Colour-Demosaicing-LICENSE`.

The RAW sample fingerprint stays independent of reconstruction policy; runtime
bindings verify both. Canonical effective policy participates in tile signatures
and pinned history source identities, so legacy/explicit bilinear replay shares
cache entries and supports mixed-format history save/restore.
The manifest source record does not embed sensor metadata or pixels;
sensor metadata/pixels must be supplied by an equivalent owned source at replay.
Source-only RAW manifests render native camera-linear RGB and reject reduction.
Saved requests accept ROI/tile/mip/quality only; edits live in the manifest.
Native and reduced cache entries stay separate, unchanged upstream tiles survive
late tone edits, and changed WB/calibration produces new stage identities.

History pins the RAW source and shares the session cache, with its own lazy
scheduler and job groups. Closing the parent session cancels its unfinished jobs;
retained history remains usable. Jobs retain their source/output after session
deletion. Dropping an unfinished job requests cancellation; explicit close rejects
new work. Source info and cache inspection/clearing remain available after close.
Cache budgets exclude sources, transient tiles, returned bytes and history state.
This remains decoded input with separately pinned bilinear or Menon base
reconstruction; file decoding and production RAW image quality are separate work.

For read-only histograms, include `ImageAnalysis.hpp` and call `histogram_rgb`
on an existing C++ graph and render request. Python `RawSession`, `RasterSession`
and `RasterGraphSession` expose
`histogram_manifest(manifest, options=None, *, bins=256, lower=0.0, upper=1.0)`.
It streams tiles and returns R/G/B uint64 counts, signed/headroom range counts,
finite extrema and the actual output color descriptor. Select the intended saved
graph stage explicitly. See the [analysis contract](docs/IMAGE_ANALYSIS_V1.md)
for endpoint conventions, memory, cache and cancellation behavior.

`SpatialOps.hpp` adds stable local RGB mean/population variance and finite counts,
with complete halos and true-image-edge windows. Python sessions stream owned
tiles through `analyze_local_manifest(manifest, callback, options=None, *, radius=3)`.
The same header exports a bounded scene-linear `ConvolutionNode`; explicit saved
`rawengine.convolution` operations participate in graph/cache/jobs/history and
source-footprint planning. See [spatial foundations](docs/SPATIAL_FOUNDATIONS_V1.md)
for numerical, kernel orientation, border, reduced-level and callback contracts.

`ToneOps.hpp` exports scene-linear per-channel `CurvesNode` and affine `LevelsNode`.
Explicit `rawengine.curves` / `rawengine.levels` saved operations work through the
existing C++ graphs and Python manifest/jobs/history/analysis APIs. Endpoint lines
extrapolate signed values/headroom; identity channels preserve pixels exactly.
See [curves and levels](docs/CURVES_LEVELS_V1.md) for knot/endpoint/version limits
and reduced-preview order. Midtone gamma and spline curves are separate work.

`ToneOps.hpp` also exports `SaturationNode` and finite `SaturationSettings.amount`
in `[0,4]`. Explicit `rawengine.saturation` schema1/process2 manifests scale
scene-linear chroma around native working-space luminance: 0 is grayscale,
1 is exact finite identity, higher amounts increase chroma. Neutrals preserve
bits, signed/headroom values stay unclipped, nonfinite/overflow inputs reject,
and mapping follows requested-level upstream rendering. Existing cache/jobs/history
and analysis APIs apply. See the [saturation contract](docs/SATURATION_V1.md);
the simple viewer exposes saturation and vibrance. `ToneOps.hpp` exports
`VibranceNode`/`VibranceSettings.amount` in `[-1,1]`, with exact zero identity and
an original bounded working-Y adaptive chroma map. Explicit `rawengine.vibrance`
schema1/process2 manifests reuse graph/cache/jobs/history/analysis and requested-level
rendering. See the [vibrance contract and evidence](docs/VIBRANCE_CONTRACT_V1.md).
Skin/perceptual/profile and production color qualification remain open.

`ToneOps.hpp` exports `ChannelMixerSettings` and `ChannelMixerNode` for an
immutable row-major 3Ã—3 scene-linear RGB matrix. Explicit `rawengine.channel_mixer`
schema1/process2 manifests accept exactly nine finite coefficients of magnitude
at most 64, retain the working-space descriptor and reuse cache/jobs/history/
analysis/geometry/mip APIs. Identity and unit channel-selection rows copy bits;
other rows use strict float64 dot products, with finite input/output checks and
no clipping, offsets or row normalization. See the [channel mixer contract](docs/CHANNEL_MIXER_V1.md).
The simple viewer retains its existing controls; production color quality remains open.

`ToneOps.hpp` exports `Lut1DSettings` and `Lut1DNode` for three uniformly sampled
scene-linear RGB tables, with equal 2..256 sample counts and an explicit shared
input range. `rawengine.lut1d` schema1/process2 requires `input_min`, `input_max`
and three `channels` arrays. Strict linear interpolation and endpoint
extrapolation retain signed values/headroom; exact identity channels preserve
float32 bits. Finite coordinate/sample/slope bounds and overflow checks apply.
The node reuses the existing curve mapper and graph/cache/jobs/history/analysis/
requested-mip APIs. See the [1D LUT contract](docs/LUT1D_V1.md).
File loading, larger tables and viewer controls remain follow-ups.

`ToneOps.hpp` exports `Lut3DSettings`, its validator and `Lut3DNode` for a 2..17
cubic grid, explicit RGB-axis ranges and red-fastest flat RGB values.
`rawengine.lut3d` schema1/process2 requires `size`, `input_min`, `input_max` and
`values`. Ordered trilinear interpolation extrapolates boundary cells without
clipping or transfer conversion. Exact identity components preserve float32 bits;
finite coordinate/value/edge-slope bounds and output overflow checks apply.
The node uses the existing saved graph/cache/jobs/history/analysis/requested-mip
APIs. See the [3D LUT contract and evidence](docs/LUT3D_CONTRACT_V1.md).
File formats, larger grids, UI and production color quality remain follow-ups.

`ToneOps.hpp` exports `ColorMixerSettings`, its validator and `ColorMixerNode`.
The [original color mixer contract](docs/COLOR_MIXER_CONTRACT_V1.md) defines
eight overlapping scene-linear hue bands with separate hue, chroma and native-Y
adjustments. Signed values/headroom and exact neutral/identity bypasses are
explicit; luminance edits fade near neutrals. Saved `rawengine.color_mixer`
schema1/process2 requires exactly three eight-number arrays: `hue_shift` in
[-60,60] degrees, `saturation_delta` and `luminance_delta` in [-1,1], all
defaulting to zero. The fixed order is red, orange, yellow, green, aqua, blue,
purple, magenta. It maps requested native/mip working RGB without a halo.
This is original scene-linear hue/chroma/native-Y behavior; conventional HSL
and selective color remain follow-ups. The simple viewer exposes the original
mixer through a hue-band selector and three sliders in its Color mixer tab.

`ToneOps.hpp` exports `ColorBalanceSettings`, its validator and `ColorBalanceNode`.
The [original tonal color-balance contract](docs/COLOR_BALANCE_CONTRACT_V1.md)
defines three working-RGB offset bands with original native-Y quadratic weights
and optional luminance preservation. Saved `rawengine.color_balance`
schema1/process2 requires exactly `shadows`, `midtones` and `highlights` arrays
of three finite numbers in [-1,1], and a strict `preserve_luminance` boolean.
Native settings default to zero offsets and preservation on. It maps requested
native/mip scene-linear ProPhoto/D50 or Rec.2020/D65 RGB without a halo, retaining
exact zero-offset channel bits and signed/headroom values. Neutral tint and black
lift are intentional; broader grading and production color qualification remain open.

`ToneOps.hpp` exports `GrayscaleNode` for [fixed working-Y monochrome](docs/GRAYSCALE_V1.md).
Saved `rawengine.grayscale` schema1/process2 requires empty `{}` parameters.
It replicates luminance into scene-linear RGB, preserves exact neutral bits,
and supports native/mip working-space rendering with signed values and headroom.
Creative channel-weighted monochrome uses the existing channel mixer.

`SpatialOps.hpp` exports `ClaritySettings`, its validator and
`ClarityNode(input, native_bounds, settings)`. Saved `rawengine.clarity`
schema1/process2 requires `{"amount":0.5,"radius":3}`: amount [-1,1], radius1..8.
Defaults are amount0/radius3. It adds midtone-weighted local working-Y contrast
using actual clipped true-image neighborhoods and complete tile halos. Identity,
flat and endpoint bypasses retain original bits; signed RGB/headroom remain
unclipped. Native and mip1/2 preview rendering apply clarity after upstream
reduction with radius measured in requested-level pixels. Existing manifest,
cache, jobs, history, footprints and analysis APIs exercise the node; no new
Python convenience renderer or UI is needed. See the original
[clarity contract and verification](docs/CLARITY_CONTRACT_V1.md) for order,
border/halo rules, expected edge response and measured API costs.

`SpatialOps.hpp` also exports `TextureSettings`, its validator and
`TextureNode(input, native_bounds, settings)`. Saved `rawengine.texture`
schema1/process2 requires `{"amount":0.5,"scale":2}`: amount [-1,1], scale1..4,
defaults amount0/scale2. Texture adjusts the difference between fine and coarse
native-Y blur planes. Each unit blur is horizontal then vertical [1,2,1], with
actual-weight normalization at true image borders; complete support is2*scale
requested-level pixels. Strict double arithmetic, exact bypass bits, unclipped
signed/headroom RGB and upstream-reduction-first mip behavior follow the
[texture contract](docs/TEXTURE_CONTRACT_V1.md). Saved graph, cache, jobs,
history, source footprints and analysis use existing C++/Python APIs. No new
viewer panel or default recipe is added.

`SpatialOps.hpp` exports `WorkingYGuidedFilterSettings`, its validator,
`working_y_guided_filter_region`, `working_y_guided_filter_rgb` and
`WorkingYGuidedFilterNode(input, native_bounds, settings)`. Saved
`rawengine.guided_filter_working_y` schema1/process2 requires exactly
`{"radius":3,"epsilon":0.000244140625}`: integral numeric radius0..8 and finite
epsilon2^-24..65536. The scene-linear ProPhoto/D50 or Rec.2020/D65 RGB primitive
uses shared scalar working-Y guidance and independent channel regressions,
actual-count clipped true borders and complete2r support. Radius0 preserves
finite input bits; signed values/headroom remain unclipped. Mip1/2 Preview
filters the upstream reduced input with radius in requested-level pixels.
Existing manifest/session/cache/jobs/history/analysis APIs provide Python access.
Equal-Y color edges can smooth, weights can be negative and output overflow
rejects; this primitive is not camera noise reduction. The
[frozen mathematical contract](docs/GUIDED_FILTER_CONTRACT_V1.md) records the
pre-native specification; current implementation and engineering acceptance are
in the [build plan](docs/plan/LIBRAWOPS_PLAN.md). A representation-gated
[finite-check proof](docs/GUIDED_FILTER_FINITE_CHECK_OPTIMIZATION_V1.md) supports
the accepted arithmetic-preserving optimization. Selected serial 256-square
cold API medians are about29ms at radius3 and134ms at radius8; PERF-036 tracks
remaining whole-engine resource qualification. These are bounded test timings.
[Opt-in resource benchmarks](bench/guided_filter_qualification/README.md) cover
exact full-frame/tiled/streamed delivery, thread scaling and fresh-process45MP
measurements. [Resource evidence](docs/GUIDED_FILTER_RESOURCE_EVIDENCE_V1.md)
records the selected results and ownership/timing limits.

`GradingSettings` and `GradingNode` provide bounded per-channel lift, gain and
signed gamma in the declared scene-linear working space. Saved
`rawengine.grading` schema1/process2 uses three RGB arrays: `lift` in [-1,1],
`gain` in [0,4], and `gamma` in [.25,4]. Defaults are [0,0,0], [1,1,1] and
[1,1,1]. Exact identity channels retain float32 bits. See the original
[grading contract](docs/GRADING_V1.md) for arithmetic and requested-mip order.

`LargeLut1DNode` and `LargeLut3DNode` support 2..4096 samples and cubic 2..33
tables. Their saved types are `rawengine.lut1d_large` and `rawengine.lut3d_large`,
schema1/process2. Existing LUT types retain their 256/17 limits and interpolation.
`CubeLut.hpp` exports bounded text/file import, matching-node construction and
copied saved-operation construction. Python imports return copied parameters:

```python
imported = rawengine_native.load_cube_lut(
    "original-scene-linear.cube", working_space="prophoto-d50"
)
# Start from a session-exported document with a scene-linear output.
operation = {
    key: imported[key]
    for key in ("type", "schema_version", "processing_version",
                "input_domain", "output_domain", "parameters")
}
operation.update(id="93000000-0000-0000-0000-000000000090", enabled=True,
                 inputs={"image": document["output"]}, masks={},
                 blend_mode="normal", opacity=1)
document["operations"].append(operation)
document["output"] = operation["id"]
```

`parse_cube_lut(text, working_space=...)` accepts the same bounded subset.
Declare the actual scene-linear space explicitly; import performs no inferred
transfer/profile conversion. The recipe stores numeric values, so changing or
deleting the file does not affect replay. Import supports one ASCII table,
4 MiB text/files and 250-byte lines; 3D mapping uses trilinear interpolation.
See [file policy and evidence](docs/LUT_FILE_V1.md) for domains and rejected
dialects. These controls reuse graph/cache/jobs/history/analysis and native/mip
rendering. The local viewer remains a test harness excluded from installation.

On Windows, the staged install places the extension in `lib` and RawEngine.dll
in `bin`. Configure the stage and host MSVC runtime DLL search directories
before importing; the Anaconda3.9 install smoke explicitly preloads RawEngine.dll
by absolute path and retains its DLL-directory handles. This verifies the library
payload with the host runtime. Clean-machine/runtime redistribution is a release
gate recorded in the build plan.

For repeated scene-linear raster viewport or slider renders, retain a `RasterSession`:

```python
session = rawengine_native.RasterSession(
    linear_rgb, width, height, "prophoto-d50",
    row_stride_pixels=width, cache_bytes=64 * 1024 * 1024,
)
preview = {"mip": 2, "quality": "preview", "output_mode": "srgb-preview"}
w, h, rgb_bytes = session.render(preview)
w, h, changed_bytes = session.render({**preview, "exposure_stops": 1.0})
print(session.cache_stats())
session.clear_cache()
```

Construction copies, validates and fingerprints the scene-linear source once.
The session owns its pixels; changing or deleting the caller's buffer has no
effect. Each `render` supplies a complete recipe with the same defaults as
`render_raster`; omitted controls do not inherit a previous call's edits.
Working space and stride are fixed at construction. Source options, RAW
calibration, cache budget and scheduler budgets cannot be supplied in render
options. Optional constructor arguments `workers=1` (1â€“64) and `max_pending=8`
(positive) bound the asynchronous scheduler, which starts on the first submit.

The session builds immutable executable edit graphs and shares a bounded stage
cache across revisions. `cache_stats()` returns `entries`, `used_bytes`, `hits`,
`misses`, and `budget_bytes`. The default budget is 64 MiB; zero disables tile
retention. The budget covers cached tile pixels and entry charges, excluding
the owned source, graph allocations, transient render buffers and returned
Python bytes. `clear_cache()` removes entries, resets counters and prevents
in-flight old-generation work from repopulating them.

Both one-shot calls and session native rendering release the Python GIL after
copying/parsing Python data. Concurrent calls on a session use independent
recipes and its thread-safe cache. Returned bytes own their storage and remain
valid after later renders, cache clearing or session deletion. ICC adapters
are not yet exposed in Python.

Use saved edit manifests with the same owned source and cache:

```python
import json

print(session.source_info())  # ID, kind, working space, SHA-256, native dimensions
saved = session.export_manifest({"output_mode": "srgb-preview", "rotate": 90})
document = json.loads(saved)
exposure = next(op for op in document["operations"] if op["type"] == "rawengine.exposure")
exposure["parameters"]["stops"] = 1.0
edited = json.dumps(document)
request = {"mip": 2, "quality": "preview", "tile_size": 256}
w, h, rgb_bytes = session.render_manifest(edited, request)
job = session.submit_manifest_latest("viewport", edited, request)
w, h, rgb_bytes = job.result()
```

`export_manifest(options=None)` returns deterministic format-v2 JSON for a
recipe; ROI, tile size, mip and quality are request controls and are not saved.
`source_info()` returns an independent metadata dictionary using the manifest's
source kind/working-space names, plus native width/height. It remains available
after `close()`. The source UUID is stable within the session; the fingerprint
also binds pixels, dimensions and rendering metadata. An equivalent owned source
can replay the saved document. Changed pixels or source metadata reject.

`render_manifest(manifest, options=None)`, `submit_manifest(manifest, options=None,
priority="normal")` and `submit_manifest_latest(group, manifest, options=None,
priority="interactive")` accept JSON strings. Priorities are keyword-only.
The core reader accepts formats v2/v3 and its existing explicit v1 migration.
Format v3 additionally requires pinned demosaic policy on RAW sources; raster
recipes and graph skeletons continue using v2.
The graph must bind exactly the session's source record; branches may reuse it.
Additional sources, unsupported versions/operations, mismatched domains, masks,
general compositing metadata and ICC transforms reject through core validation.
These calls execute the saved output directly without appending a tone/output
chain. A source-only or scene-linear output therefore returns scene-linear RGB;
an encoded output returns its declared encoding. Use the manifest output domain
to interpret the native-endian float32 bytes.

Manifest request dictionaries accept only `x`, `y`, `roi_width`, `roi_height`,
`tile_size`, `mip` and `quality`. Defaults use the saved graph's output extent,
including geometry, at the requested level. Supply explicit ROI dimensions for
nonzero origins. Mip 1/2 requires preview quality and a graph whose output chain
supports that level. Recipe/edit keys and unknown request keys reject. Parsing,
binding and rendering release the GIL after copying the JSON. Submitted jobs
retain native graph/source/cache ownership, expose the same cancellation,
repeatable results and tile progress as recipe jobs, and share viewport groups
with `submit_latest`. Invalid submissions leave existing group jobs intact.

For independent images, use `RasterGraphSession` with explicit source IDs:

```python
base_id = "00000000-0000-0000-0000-000000000001"
layer_id = "00000000-0000-0000-0000-000000000002"
mix_id = "00000000-0000-0000-0000-000000000003"
sources = {
    base_id: {"rgb": base_rgb, "width": width, "height": height,
              "working_space": "prophoto-d50"},
    layer_id: {"rgb": layer_rgb, "width": width, "height": height,
               "working_space": "prophoto-d50"},
}
graph_session = rawengine_native.RasterGraphSession(sources, workers=2)
document = json.loads(graph_session.export_manifest(base_id))
document["operations"] = [{
    "id": mix_id, "type": "rawengine.linear_mix",
    "schema_version": 1, "processing_version": 2, "enabled": True,
    "input_domain": "scene_linear_prophoto_d50",
    "output_domain": "scene_linear_prophoto_d50",
    "inputs": {"base": base_id, "layer": layer_id},
    "parameters": {"amount": 0.25}, "masks": {},
    "blend_mode": "normal", "opacity": 1.0,
}]
document["output"] = mix_id
saved = json.dumps(document)
request = {"mip": 1, "quality": "preview", "tile_size": 256}
print(graph_session.required_source_regions(saved, request))
job = graph_session.submit_manifest_latest("viewport", saved, request)
w, h, linear_rgb_bytes = job.result()
```

Construction copies and fingerprints 1â€“64 scene-linear raster sources, keyed by
distinct stable lowercase UUID strings. Each spec requires `rgb`, positive integer
`width`/`height` and explicit `working_space` (`"prophoto-d50"` or `"rec2020-d65"`),
with optional integer `row_stride_pixels=0` for packed input. Unknown spec keys,
invalid dimensions/strides, nonfinite pixels and unsupported color spaces reject.
The same native-endian contiguous float32 RGB buffer contract applies. Constructor
budgets `cache_bytes=67108864`, `workers=1` and `max_pending=8` are keyword-only.
Source pixels and job/transient buffers remain outside the cache budget.

`source_info()` returns a copied source-ID-to-metadata dictionary.
`export_manifest(source_id)` creates a deterministic format-v2 skeleton containing
all owned source records, no operations, and the selected source as output; its
document working space follows that source. Add operations explicitly in JSON.
`render_manifest`, `submit_manifest`, `submit_manifest_latest`, `close`,
`cache_stats` and `clear_cache` have the same request/job contracts as `RasterSession`.
Each declared source record must match an owned identity; a manifest may declare
only a subset of owned sources. Undeclared owned images do not affect binding.
Linear mix requires matching branch domains and extents: use explicit working-space
conversion, crop/orientation/resize nodes to align independently sized/color-tagged
images. The saved output domain determines whether returned RGB is scene-linear
or encoded; the example's mix remains scene-linear.

Both session types expose `required_source_regions(manifest, options=None)`:
each reachable source ID maps to an original native `(x, y, width, height)`
footprint. It uses the same typed output ROI/level as rendering, composes geometry
and blur halos, and coalesces shared-source paths. Disabled mix needs only its
base branch. The map describes rectangles, including conservative bounding
unions, rather than disjoint regions.

`RasterGraphSession.replace_source(source_id, source_spec)` copies, validates and
atomically replaces an existing ID's immutable source; it returns `None`. It does
not add/remove IDs or rewrite saved JSON. Refresh the affected record from
`source_info()` or `export_manifest` before rendering a revised manifest. Older
saved fingerprints reject future binding, while submitted jobs keep the source
snapshot they already bound. Changed source signatures invalidate that branch's
downstream cache keys; unchanged branches retain cache reuse. Failed replacement
preserves the current source and jobs. Concurrent exports/bindings see coherent
snapshots; a replacement between export and binding may make that exported
fingerprint stale and raise `ValueError`. Replacement and new execution/export
reject after `close()`; copied source metadata remains available.

Create a bounded edit history from either session type:

```python
initial = session.export_manifest({"output_mode": "srgb-preview"})
history = session.history(initial, max_revisions=64, max_manifest_bytes=4 * 1024 * 1024)
original_id = history.stats()["current_id"]
edited = json.loads(history.snapshot())
exposure = next(op for op in edited["operations"] if op["type"] == "rawengine.exposure")
exposure["parameters"]["stops"] = 1.0
edited_id = history.commit(json.dumps(edited))
request = {"mip": 2, "quality": "preview"}
before, after = history.compare(original_id, edited_id, request)
history.undo()
w, h, rgb_bytes = history.render(request)
history.redo()
job = history.submit_latest("viewport", request, revision=edited_id)
saved_history = history.save()
restored = session.restore_history(saved_history)
```

`history(manifest, *, max_revisions=64, max_manifest_bytes=4194304)` returns
`EditHistory`, pinning the manifest's complete declared source identities and
native source snapshots. It shares the parent's tile cache and inherits worker/
queue budgets for its own lazy scheduler. Histories have independent navigation,
close state and viewport groups; closing or deleting a parent does not invalidate
its history. Parent source replacement leaves the pinned history renderable.
Commits changing source records reject; create a new history for a new source
version. Restoring a saved history against current sources checks every source
fingerprint and every executable revision; it cannot silently rebind old edits to
changed pixels. Pixel/source assets are referenced, not embedded in history JSON.

`commit(manifest)` validates a complete executable graph before publication and
returns a new positive revision ID. IDs start at 1 and increase without reuse,
even after eviction or redo truncation; identical commits still get a new ID.
A commit after undo discards redo states. Failed parse, binding, version/domain
validation or an oversized revision leaves history and IDs unchanged.
`undo()`/`redo()` return the selected ID; unavailable navigation or retained IDs
raise `IndexError` (`EditRevisionUnavailable` in C++). Other invalid requests or
manifests raise `ValueError`; revision arguments require positive integers.

Retention is bounded by both revision count (1â€“100000) and aggregate canonical
UTF-8 manifest bytes (1â€“16777216). Oldest retained states are evicted on commit
until both limits hold; the new state must fit by itself. `stats()` reports
`current_id`, `revision_ids`, `manifest_bytes`, `can_undo`, `can_redo`,
`max_revisions` and `max_manifest_bytes`. The byte budget covers retained manifest
text only. Compiled graph allocations, sources, tile cache, transient copies,
external C++ snapshots, Python strings and job/result buffers remain outside it.
Tile retention still follows the shared cache budget. External snapshots and
already submitted jobs can outlive metadata eviction.

`snapshot(revision=None)` returns immutable canonical manifest JSON for a retained
revision; omitted/`None` selects current. `render(options=None, *, revision=None)`,
`submit(options=None, *, revision=None, priority="normal")` and
`submit_latest(group, options=None, *, revision=None, priority="interactive")`
capture the selected revision before rendering/queueing. They use the existing
strict ROI/mip/quality request dictionary and RenderJob cancellation, progress,
timeout and repeatable-result contracts. Current navigation, new commits, redo
truncation or eviction cannot change a bound job's pixels. Invalid submissions
leave existing group jobs intact. Unchanged stage signatures reuse cached tiles
across revision edits and undo/redo.

`compare(first_id, second_id, options=None)` atomically captures two retained
revisions and returns their independent raster results. With omitted ROI dimensions,
each uses its own output extent, including geometry and requested mip. An explicit
ROI must be valid for both. It performs no implicit difference/color conversion;
use each snapshot's output domain to interpret its RGB bytes. Submit individual
revision jobs for concurrent comparisons.

`save()` returns deterministic history-format-1 JSON containing canonical manifest
strings, limits, retained revision IDs, current selection and next ID. The history
format is separate from edit-manifest formats v2/v3. `restore_history(saved_history)`
preserves cursor/redo and ID gaps, normalizes manifests through the core reader,
and rejects unknown history fields/versions, invalid IDs/order, budget violations
and any unsupported revision. Both serialized history input/output have a 16 MiB
JSON cap, including escaped strings and envelope overhead. `close()` rejects new
commits/navigation/renders/comparisons/jobs and cancels unfinished jobs;
`stats()`, `snapshot()` and `save()` remain readable. Dropping an unfinished job
requests cancellation; dropping a history preserves retained native job results.

The same core contract is available to C++ consumers:

```cpp
rawengine::EditHistory history(initial_manifest, bindings,
    rawengine::EditHistory::Limits{64, 4 * 1024 * 1024}, nullptr, cache);
auto original = history.current();
auto edited_id = history.commit(edited_manifest);
auto [before, after] = history.comparison(original->id, edited_id);
// Render either snapshot's immutable graph with Renderer or TileScheduler.
history.undo();
auto restored = rawengine::EditHistory::restore(history.serialize(), bindings, nullptr, cache);
```

For asynchronous previews, retain the returned job:

```python
job = session.submit_latest("viewport", {**preview, "exposure_stops": 0.5})
replacement = session.submit_latest("viewport", {**preview, "exposure_stops": 1.0})
try:
    job.result(timeout=5)
except rawengine_native.RenderCancelled:
    pass
w, h, rgb_bytes = replacement.result(timeout=5)
print(replacement.progress())  # completed_tiles and total_tiles
session.close()
```

`submit(options=None, *, priority="normal")` returns a `RenderJob` without
superseding other requests. `submit_latest(group, options=None,
*, priority="interactive")` removes older queued work in that nonempty group
and cancels a running request at its next tile boundary. Groups are scoped to
one session. Priorities are `"background"`, `"normal"`, and `"interactive"`;
pending requests run by priority and FIFO within one priority. Running tiles
are not preempted. A full pending queue raises `RuntimeError`; replacing queued
work in the same group can reuse its slot. Invalid requests reject before
superseding valid older work.

`job.result(timeout=None)` releases the GIL while waiting and returns the same
tuple as synchronous rendering. A finite nonnegative timeout raises
`TimeoutError` if not ready, without cancelling the request. Repeated or
concurrent result calls are supported and each returns an owned byte copy.
Cancelled and superseded jobs raise `rawengine_native.RenderCancelled`
(a `RuntimeError` subclass); completed results survive late cancellation.
`job.done()` means a result or exception is ready. `job.cancel()` requests
cancellation without waiting. `job.progress()` reports completed output tiles
and the planned total, including edge tiles; cached tiles count too. Cancelled
jobs can have partial progress. There is no intra-tile progress or ETA; empty
ROIs have zero tiles.

A retained job owns the native graph, source/cache handles and scheduler it
needs, so deleting its session does not invalidate it. Dropping an unfinished
job requests cancellation. Retaining a completed job also retains its native
materialized result, outside the tile-cache budget; delete jobs when their
results are no longer needed. `session.close()` rejects subsequent renders and
submissions and cancels unfinished async jobs; it is idempotent. Already running
synchronous calls can finish. Final scheduler destruction joins workers with
the GIL released. No Python callbacks execute on worker threads.

## RAW reconstruction diagnostics

`tools/raw_quality_harness.py` implements all six synthetic fixture families in the
[RAW quality specification](docs/plan/LIBRAWOPS_PLAN.md#raw-demosaic-quality-harness--specification-v1).
It generates independent float32 flats, affine ramps, slanted edges, impulses,
sinusoidal detail and aliasing probes,
Bayer-samples them with half-up uint16 encoding, and renders a source-only
`RawSession` manifest before WB, exposure, calibration or output processing.
The helpers and scalar tests use only the Python standard library; rendering
requires the optional native module.

```sh
python tests/raw_quality_harness_tests.py
python tools/raw_quality_harness.py build/Release build/raw-quality-complete-v3.json --build-description "VS Release, compiler/runtime versions"
```

Use the directory containing `rawengine_native` as the first argument (for a
Ninja build on this workstation, `build-msvc-release`). The default report
covers 2,368 cases: 37 probes, two sensor layouts, four Bayer patterns, four CFA
phases and two level policies. The probes comprise seven flat/affine, eight
slanted-edge, four impulse, eight smooth-detail and ten aliasing cases.
`--quick` limits patterns/phases to RGGB/(0,0), giving 148 cases; `--seed`
records an explicit nonnegative seed (these fixtures use no randomness).

Reports contain fixture parameters, complete sensor metadata, canonical
little-endian ground-truth/Bayer hashes, source fingerprints, saved manifests,
render requests, build/runtime identity, whole/interior RGB max/MAE/RMSE,
neutral residual chroma, separate observed-site quantization and fidelity,
and exact tile/ROI checks. Affine reconstruction gates use the fixed one-pixel
interior; full-frame border errors remain visible. Tile sizes 1, 2, 7 and 256
must agree exactly. A failed gate writes its report and exits with status 1;
invalid/nonfinite data raises an error. JSON uses stable field order and
rejects nonfinite numbers. The scalar and native integration suites also run
under CTest; scalar tests remain available when bindings are disabled.

Edge probes use neutral/chromatic endpoints, two pinned orientations and either
a hard step or a four-native-pixel linear transition. Their additional metrics
select pixel centers with perpendicular distance at most two native pixels
from the analytic edge, including the active-area borders. Neutral probes
also report residual chroma within that band. All geometry, endpoints and
the band selection hash/count are saved. Edge reconstruction is baseline
characterization: its analytical threshold/gate are null, while observed-site
quantization/fidelity and exact tile/ROI checks still apply. Summary counts
distinguish analytical cases from characterization cases; a passed report
does not imply good edge reconstruction.

Impulses have a neutral or individual-channel peak at active-local `(32,24)`
over a constant background. Center, 5Ã—5 neighborhood and surrounding 24-pixel
metrics describe lost samples and reconstruction spread. Sines run along x/y
at 1/32 and 1/8 cycles per native pixel, with pinned neutral/chromatic channel
phases. Aliasing probes alternate along x/y or in a checkerboard, plus x/y
sines at 7/16 cycles per pixel. All these families are characterization,
with no reconstruction acceptance threshold. Sinusoidal bit identity is pinned
to the recorded environment; cross-platform libm equality is not claimed.

The complete corpus advances report schema/generator to version 3; metric
formulas remain version 1. All 960 prior flat/affine/edge case records are
unchanged. The preserved Windows bilinear evidence lives in
[`bilinear_baseline_v3.index.json`](tests/reference/raw/bilinear_baseline_v3.index.json)
(readable per-probe maxima, runtime/build and hashes) and
[`bilinear_baseline_v3.json.gz`](tests/reference/raw/bilinear_baseline_v3.json.gz)
(all case records). Scalar tests validate the evidence and preservation rules.

Add `--preserve-baseline build/bilinear-local-v3.json.gz` to save another full
seed-0 report with its companion index. Quick/failed/incomplete reports reject.
The gzip header uses mtime 0; identical evidence is idempotent and differing
existing evidence rejects, requiring a new path. `read_baseline(path)` verifies
compressed/JSON hashes and full corpus coverage; `preserve_baseline(path, record)`
provides the same checks for Python tooling. Keep the repository snapshot as
the original measured reference when using another build or machine.

`tools/raw_quality_compare.py` pairs a new full report against the verified
compressed baseline. Fixture definitions/bytes, source fingerprints, native
requests, measurement selections, metric structure/denominators and ROI coverage
must match. Format-2 implicit bilinear policy and format-3 explicit bilinear
version 1 are equivalent for pairing. Runtime/build/source-code hashes may differ;
each report remains linked by SHA-256 in the comparison record.

```sh
python tools/raw_quality_compare.py tests/reference/raw/bilinear_baseline_v3.json.gz build/raw-quality.json build/raw-comparison.json --require parity
```

Default `--require replacement` exits 1 when the replacement gate fails;
`--require parity` exits 0 only for identical demosaic identity, rendered hashes,
metrics, tile/ROI checks and acceptance. Bilinear self/parity comparisons fail the
replacement gate, which requires a different algorithm/version and improvement.
Both outcomes are recorded in JSON.

[`replacement_policy_v1.json`](tests/reference/raw/replacement_policy_v1.json)
fixes provisional engineering targets before evaluating a candidate: every
edge/detail probe must improve the arithmetic mean of its 64 paired RGB RMSE
scores by at least 10% (edge band / detail interior); neutral probes must also
improve residual-chroma RMSE by 10%. Every reconstruction scope of every
characterization case must stay within `baseline * 1.05 + 1e-6` for RGB and
applicable neutral chroma RMSE/max error. Impulse/aliasing remain characterized,
with regression caps rather than an exact-recovery claim. Quantization,
observed-site fidelity, flat/affine, signed/headroom and exact tile/ROI gates
remain required; correctness is recomputed from reported scores/checks.
This validates harness evidence without rerendering or authenticating its pixels.
Altered metric test records exercise policy arithmetic only. It does not establish
representative-camera or production acceptance.

The current report measures synthetic native reconstruction only;
preview consistency, representative camera quality and Adobe comparisons
retain their separate gates.

[The initial candidate review](docs/research/RAW_DEMOSAIC_CANDIDATES.md) compares
MHC, Hamiltonâ€“Adams and Menon DDFAPD, records primary provenance findings, and
describes the proposed real-camera corpus. Native Menon base v1 is now an opt-in
prototype; [the first local camera study](docs/research/CAMERA_RAW_LOCAL_STUDY.md)
records its initial Nikon Z7 II checks, diagnostic-view limits and open gates.
Its follow-up records the proposed Hamiltonâ€“Adams stage-support and radius-three
halo contract, border fallback and provenance findings.
`tools/raw_ha_reference.py` now provides an original standard-library scalar
reference with separate research reports. Its full 2,368-case evaluation rejects
v1 under the frozen analytical, regression and chromatic improvement gates;
native bilinear remains unchanged. Twelve tests include hand calculations and
a per-site quantization counterexample. The inspected synthetic crop comparison
and complete findings are in the research note.

```sh
python tests/raw_ha_reference_tests.py
python tools/raw_ha_reference.py tests/reference/raw/bilinear_baseline_v3.json.gz build/ha-scalar-v1.json
```

Exit 0 means evaluation completed, including a rejected candidate; `--quick`
measures only 148 cases. These reports never claim native replacement acceptance
or provide native cache/history/preview evidence.

`tools/raw_camera_corpus.py` checks recorded capture/decode/rights fields, confined
local file paths and hashes, padded Bayer layout, sensor ROIs and proposed
camera/ISO/illumination/content coverage using only the standard library.

```sh
python tests/raw_camera_corpus_tests.py
python tools/raw_camera_corpus.py tests/reference/raw/camera_corpus_plan_v1.json build/camera-corpus-status-v1.json --require-coverage
```

The repository plan has zero actual captures, so the coverage command writes
an incomplete report and exits 1. Without `--require-coverage`, a valid partial
manifest exits 0; malformed evidence exits 2. Local-only assets remain explicitly
flagged against redistribution even if coverage is complete. Validation checks
recorded evidence; it does not decode/render, establish legal rights, verify
physical metadata or give real captures analytical RGB ground truth.

## Scope

For local high-ISO noise/detail inspection, optional NumPy/Pillow research tooling
uses a frozen native ROI plan, scene-linear statistics and paired encoded crops.
See the [protocol, measured findings and reproduction command](docs/research/RAW_NOISE_CHARACTERIZATION_V1.md).
Run `python tests/raw_noise_characterize_tests.py` in that research runtime.
These diagnostics combine scene texture, noise and artifacts; they add no denoise
operation or runtime dependency to the engine.

An [original smoothing-control study](docs/research/RAW_NOISE_CONTROL_V1.md)
now separates known injected-noise response from edge/texture damage, alongside
the fixed camera crops. Optional tests: `python tests/raw_noise_control_tests.py`.
It provides an evaluation baseline; native RAW denoise remains unimplemented.

The [supplied NR research review](docs/research/NR_RESEARCH_REVIEW_2026_10_01.md)
and [camera NR graph contract](docs/research/NR_GRAPH_CONTRACT_V1.md) define a
future optional camera-linear stage before WB and calibrated preview reduction.
This operation is not registered; exact candidate equations and evaluation
remain the next research checkpoint.

The Python extension uses module-associated heap types introduced in
[CPython 3.9](https://docs.python.org/3/c-api/type.html#c.PyType_FromModuleAndSpec).
Windows runs have verified Python 3.9 and 3.13; the complete supported-version
and platform matrix remains part of the packaging/CI gate.

This implements a clean RAW input boundary, basic bilinear demosaic, white
balance, exposure, an explicit camera-to-working-space matrix, a
highlight-compressing tone curve, a typed scene-linear raster memory source,
optional ICC raster import, a versioned edit graph with unary nodes and a two-input linear mix, bounded tile cache,
tile-boundary cancellation, a basic priority request queue, and a reference
box blur with clipped halo requests. It does not
yet implement the complete Camera Raw control set, DNG profile interpretation,
JPEG/PNG/TIFF file decoding, lens corrections, denoise, production capture/creative sharpening, or
color-managed export.
It also lacks monitor-profile previews and configurable output profiles.
Input decoding stays behind the decoded-RAW boundary. Any optional file
decoder must pass the separate no-copyleft dependency gate in the plan.

## Native benchmark

For local decoded-camera profiling, the optional NumPy-equipped research runtime
can run `bench/raw_session_benchmark.py` with a built module directory, camera
folder, retained extraction JSON, new output JSON path and `--capture ID`.
It reports native stages, cold/warm mip-2 previews, viewport edits, cache counters
and Windows process peak memory. See the [camera profiling results and command](docs/research/CAMERA_RAW_LOCAL_STUDY.md#preview-profiling-and-bounded-block-parallelism--2026-10-01).
Menon requests spanning at least four internal blocks use at most four OpenMP
threads; smaller requests/core-only builds stay serial. Per-request scratch can
reach 20.21 MiB, multiplied by concurrent requests. Reconstruction pixels and
algorithm identity are unchanged.

For Python raster previews, run the benchmark after building the module:

```sh
python bench/raster_session_benchmark.py build/Release --mip 2 --repeats 5
```

This compares
one-shot, cold/warm session and late-tone render medians on a synthetic
2048Ã—1536 scene-linear source; source initialization is timed separately.
It checks exact byte parity against one-shot renders and reports cache reuse.
Optional dimensions, mip, tile size and cache budget are explicit arguments.
Use `--crop X Y WIDTH HEIGHT` to include a crop/reduction anchor; native
footprint tiles can increase cache and transient memory cost.
Use `--resize WIDTH HEIGHT --resize-filter area` (or `nearest`/`bilinear`) to measure
native resize, optionally after crop. This defaults to mip 0 / final quality;
add `--mip 1` or `--mip 2` to measure resize followed by preview reduction.
Without resize, the default
remains mip 2 preview; `--mip 0` measures native final.
Add `--rotate 90`, `--flip-horizontal` or `--flip-vertical` to include orientation
between crop and resize; all geometry choices are recorded in the JSON output.
It excludes fixture creation, file decoding and export; returned-byte copies
and graph construction are included. Record hardware, power mode and load
before interpreting timings as a performance target.

Configure with `-DRAWENGINE_BUILD_BENCHMARK=ON`, then run
`build/Release/RawEngineBenchmark.exe 7500 6000 3 256 legacy` on Windows.
Arguments are width, height, repetitions, tile size, and mode (`legacy` or
`srgb-preview`). The preview mode includes a synthetic camera-to-Rec.2020
matrix and the full sRGB preview chain. The tool prints JSON medians
for a 1024Ã—768 materialized ROI and a full-image **streaming** render. It uses
a synthetic Bayer source and does not decode files, build reduced previews,
encode exports, or measure Adobe compatibility. Record the machine, compiler,
power mode, and build configuration alongside the JSON output.
`RawEngineColorBenchmark` uses tile-generated scene-linear pixels to compare
ProPhoto and Rec.2020 working-space conversion cost without a full-image
working buffer. It reports source and converted streaming times; those timing
differences are indicative, not a fit-preview or export latency measurement.
`RawEngineCacheBenchmark` separately measures a synthetic 2048Ã—1536
scene-linear raster, 1024Ã—768 tiled ROI, first source fingerprint, cold and
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

The opt-in `rawengine.dehaze` schema1/processing2 operation exports `DehazeSettings`,
`validate_dehaze_settings` and `DehazeNode` in ToneOps.hpp. It uses explicit uniform
transmission with amount[-1,1] and scene-linear atmospheric RGB[0,4];amount0 preserves
bits,positive removal can amplify noise and over-correct non-hazy inputs. No spatial
or automatic atmosphere estimator,clipping or UI panel is added. See the
[bounded original contract](docs/DEHAZE_CONTRACT_V1.md) and canonical build plan for
independent verification and open photographic qualification gates.

`SharpenSettings`, `validate_sharpen_settings` and `SharpenNode` in SpatialOps.hpp
provide an opt-in `rawengine.sharpen` schema1/processing2 RGB unsharp residual,
amount0..2/radius1..3. Complete true-border halos and strict centered double sums
preserve constant channel bits; signed/headroom edits are unclipped, overflow
rejects. Sharpening can amplify chromatic noise and create step lobes. See the
[original bounded contract](docs/SHARPEN_CONTRACT_V1.md) for evidence and open
capture/creative/profile/photographic qualification. No viewer panel is added.


### Original highlights, shadows, whites and blacks

`ToneOps.hpp` exports `TonalRangeSettings`, `validate_tonal_range_settings` and
`TonalRangeNode`. The four copied binary64 amounts (`blacks`, `shadows`,
`highlights`, `whites`) lie in [-1,1], default 0. They compose fixed compact
working-luminance warps and apply one positive RGB gain. These are original
normalized brightness controls, not EV stops, black-floor lift or sensor highlight
recovery. Signed/headroom data stays unclipped; identity preserves float32 bits.

Use explicit `rawengine.tonal_range` schema1/processing2 manifests with all four
parameters on existing RAW/raster/multisource Python session, job, history and
analysis paths. The node consumes requested-level input before the nonlinear
map; native Final/Preview and mip1/2 Preview require upstream support. It adds
point support and no image-sized scratch. See the frozen
[contract](docs/TONAL_RANGE_CONTRACT_V1.md) for intervals, order, rounding, domain,
rejection and resource rules. Representative photographic/profile and production
qualification remain open. The library installation contains no viewer.


### Master curves, cubic interpolation and gamma levels

`ExtendedCurvesNode` owns a master curve and three channel curves with explicit
linear or minmod shape-preserving cubic interpolation. The master runs before
the channel curve in binary64, with one final float32 cast and linear endpoint
extrapolation. `GammaLevelsNode` owns RGB black/white endpoints and signed
normalized gamma;gamma1 retains the existing affine levels arithmetic. Both
operate explicitly in scene-linear ProPhoto/D50 or Rec.2020/D65 with native or
requested-level reduction before the point map.

Saved types `rawengine.curves_extended` and `rawengine.levels_gamma` use distinct
schema1/process2 parameters described in
[the frozen contract](docs/CURVES_LEVELS_CONTRACT_V1.md). Existing curves/levels
and default pipelines remain unchanged. Independent frozen numeric replay,
eight-case Python integration (including optional real ICC composition), all four
full configurations and fresh installed C++/Python consumers pass. Diagnostics
cover 480 curve/gamma cases across supplied ISO 100, 250 and 20000 images, with
independent numeric references and exact RAW/raster/tile/job/history replay.
Bounded API costs are recorded in the build plan. Production photographic/profile
qualification, wider camera coverage and portable/large-image measurements remain
open; low ISO alone does not establish a noise-free reference.

### Coverage and alpha numeric foundation

`CoverageOps.hpp` provides separate scalar coverage and scene-linear premultiplied RGBA payloads, validation, premultiplication, safe straight-color access, coverage scaling and normal source-over. Signed/headroom color is preserved; transparent hidden premultiplied color and nonfinite values are rejected, and narrowing overflow throws. See the [numeric contract](docs/COVERAGE_ALPHA_CONTRACT_V1.md) and [verified scope](docs/COVERAGE_ALPHA_FOUNDATION_EVIDENCE_V1.md). These helpers do not yet add mask/RGBA graph, painting, Python or saved-document support.

### Immutable scalar coverage sources

`CoverageSource.hpp` provides copied, canonical immutable coverage sources with thread-safe fingerprints, native output, direct mip-1/2 previews and native ROI footprint inspection. Padding is discarded; native origin and visible values define source identity. See the [source contract](docs/COVERAGE_SOURCE_CONTRACT_V1.md) and [acceptance scope](docs/COVERAGE_SOURCE_EVIDENCE_V1.md). Typed mask graph,painting and Python integration remain next.

### Typed core mask graphs

`MaskOps.hpp` adds scalar CoverageNode adapters, inversion and add/subtract/intersect algebra. Explicit edit format4 binds coverage sources and named mask edges for a scene-linear masked RGB adjustment. Scalar/RGB tiles share the existing bounded cache; source footprints and core history include mask dependencies. See the [contract](docs/MASK_GRAPH_CONTRACT_V1.md) and [bounded acceptance](docs/MASK_GRAPH_EVIDENCE_V1.md). Python coverage sessions, scalar jobs, brush/parametric/range/feather masks, RGBA layers and saved assets remain open.

### Scalar mask delivery and Python sessions

`CoverageDelivery.hpp` streams/assembles scalar CoverageNode output. TileScheduler shares workers, pending budget and supersession groups across RGB and scalar futures. Python RasterGraphSession accepts copied coverage specs alongside RGB, with explicit `render_coverage_manifest`/`submit_coverage_manifest` methods. EditHistory adds typed coverage render/submit/compare methods. Results contain one packed float32 sample per pixel. Existing RGB methods reject scalar output. See the [contract](docs/COVERAGE_DELIVERY_CONTRACT_V1.md) and [acceptance](docs/COVERAGE_DELIVERY_EVIDENCE_V1.md). Brush/local masks, RGBA layers and saved assets remain open.

### Raster brush masks

`BrushMask.hpp` exposes copied paint/erase polylines, `apply_brush_mask` and `CoverageBrushNode`. Saved `rawengine.mask_brush` operations replay native-coordinate pressure/flow/opacity and soft round coverage, then directly reduce painted native samples for mip previews. Existing scalar graph/session/history/job methods execute brush masks; shared masks also drive RGB adjustments. See the [contract](docs/BRUSH_MASK_CONTRACT_V1.md), [primitive API](docs/BRUSH_MASK_PRIMITIVE_API_V1.md) and [acceptance](docs/BRUSH_MASK_EVIDENCE_V1.md). Linear/radial/range/refinement masks and RGBA layers remain open.

### Geometric coverage masks

`ParametricMask.hpp` exposes copied linear ramps, forward-axis radial ellipses and even-odd polygon rings, scalar evaluators and CoverageNode implementations. Strict saved format-4 types generate native shape coverage and multiply an input mask before direct mip reduction. All-one input generates the shape directly. Existing scalar/masked-RGB graph/session/history/job methods execute them. See the [contract](docs/PARAMETRIC_MASK_CONTRACT_V1.md) and [acceptance](docs/PARAMETRIC_MASK_EVIDENCE_V1.md). Range/refinement masks and RGBA layers remain open.

Original luminance/color/supplied normalized-depth range masks now support strict saved mixed-guide scalar graphs, source-ID footprints, cache/history/jobs and Python. Full Release93/53/54/94 and fresh default/LCMS+Python installed consumers pass. [Range acceptance](docs/RANGE_MASK_EVIDENCE_V1.md) records the canonical metadata correction and deferred shared-DAG support-check scale limit. No UI expansion or performance remediation.

### Phase 5 mask refinement acceptance — 2026-10-04

Density/binomial feather/external working-Y scalar refinement now passes frozen native/saved/Python/ROI/halo/cache/history/jobs, full96/55/56/97 and fresh installed C++/Python gates. [Evidence](docs/MASK_REFINEMENT_EVIDENCE_V1.md). RGBA/layers/assets and broader quality/performance/release remain open; no UI expansion or agent commit/push.

Typed native RGBA source/composition and safe color/alpha adapters now pass independent native/ROI/mip and fresh installed-consumer gates. [Evidence](docs/RGBA_NATIVE_GRAPH_EVIDENCE_V1.md). Saved RGBA/cache/delivery/Python/history integration remains pending; the original coverage/RGBA checklist row stays open. Diagnostic UI remains outside library packaging.

Typed premultiplied RGBA now supports format-5 saved graphs, shared typed cache/jobs, source-ID footprints and explicit Python session/history methods. RGB and scalar coverage APIs preserve their output contracts. See [RGBA integration evidence](docs/RGBA_SAVED_GRAPH_EVIDENCE_V1.md) and [saved schema](docs/RGBA_SAVED_GRAPH_CONTRACT_V1.md).

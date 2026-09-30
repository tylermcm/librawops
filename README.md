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
a sensor-relative active area, and four 2×2-site black and white levels.
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
the scale is 2^m and output dimensions are ceil(width/scale) ×
ceil(height/scale). Output pixel (x,y) averages its direct scale×scale
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
dimensions `ceil(active_width/scale)` × `ceil(active_height/scale)`. CFA phase
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
coordinate-transform/mipmap system. `RawUnpackNode` reports the one-pixel
sensor halo used by its bilinear demosaic. For built-in unary edit graphs,
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
pixels and averages direct clipped 2×2/4×4 footprints starting at the crop
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
clipped 2×2/4×4 groups, dividing by actual sample counts at odd edges.
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
`flip_horizontal`/`flip_vertical`. Output bounds start at zero; 90°/270° swap
width and height. Native pixels, including signed zero, are copied without
interpolation or color changes. The integer inverse map transforms requested
rectangles into upstream coordinates, including nonzero input origins, and
composes with crop/resize/blur and multiple-source planning. Parameters take
part in cache identity; disabled orientation preserves upstream bounds.

Mip-1/2 orientation previews average direct clipped 2×2/4×4 transformed native
pixels from the output origin before downstream tone/encoding. At odd edges
they divide by actual sample counts. This equals orienting a native linear
buffer and then reducing it; rotating/flipping an already reduced preview can
differ because its edge groups were anchored differently. Like crop and resize,
orientation requests native upstream work; bounded calibrated RAW is supported,
while uncalibrated RAW and ICC reduced gates remain.
The last crop/orientation/resize anchor determines the averaging grid. Arbitrary
angles, perspective and EXIF/file orientation handling are not supplied by this
manual in-memory geometry operation.

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

Raster options also accept `mip` (0, 1, or 2; default 0) and `quality`
(`"final"` or `"preview"`; default `"final"`). Mip 1/2 requires both
`quality="preview"` and `output_mode="srgb-preview"`. The default viewport is
the full reduced image, with dimensions `ceil(width / 2**mip)` ×
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
crop in original-source coordinates → rotation → flips → resize → exposure,
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
one-shot RAW: demosaic → WB → exposure → optional camera calibration → output.
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
site-level tuples. The manifest source record contains only ID/kind/fingerprint;
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
This remains decoded input with the bilinear reference algorithm; file decoding
and production RAW image quality are separate work.

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
options. Optional constructor arguments `workers=1` (1–64) and `max_pending=8`
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
The core reader accepts format v2 and its existing explicit v1 migration.
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

Construction copies and fingerprints 1–64 scene-linear raster sources, keyed by
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

Retention is bounded by both revision count (1–100000) and aggregate canonical
UTF-8 manifest bytes (1–16777216). Oldest retained states are evicted on commit
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
format is separate from edit-manifest format v2. `restore_history(saved_history)`
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

## Scope

The Python extension uses module-associated heap types introduced in
[CPython 3.9](https://docs.python.org/3/c-api/type.html#c.PyType_FromModuleAndSpec).
The current verified runtime is Python 3.13; other supported-version builds
remain part of the packaging/CI gate.

This implements a clean RAW input boundary, basic bilinear demosaic, white
balance, exposure, an explicit camera-to-working-space matrix, a
highlight-compressing tone curve, a typed scene-linear raster memory source,
optional ICC raster import, a versioned edit graph with unary nodes and a two-input linear mix, bounded tile cache,
tile-boundary cancellation, a basic priority request queue, and a reference
box blur with clipped halo requests. It does not
yet implement the complete Camera Raw control set, DNG profile interpretation,
JPEG/PNG/TIFF file decoding, lens corrections, denoise, sharpening, or
color-managed export.
It also lacks monitor-profile previews and configurable output profiles.
Input decoding stays behind the decoded-RAW boundary. Any optional file
decoder must pass the separate no-copyleft dependency gate in the plan.

## Native benchmark

For Python raster previews, run the benchmark after building the module:

```sh
python bench/raster_session_benchmark.py build/Release --mip 2 --repeats 5
```

This compares
one-shot, cold/warm session and late-tone render medians on a synthetic
2048×1536 scene-linear source; source initialization is timed separately.
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

# Local statistics and convolution foundations, version 1

This checkpoint adds stable local RGB moments, streamed graph/session analysis
and an original bounded convolution operation. These support later controls;
adaptive NR and camera noise calibration remain separate work.

## Local mean and population variance

Include `SpatialOps.hpp`. `local_statistics_region(output, image_bounds, radius)`
returns the complete halo clipped at the true image boundary. Radius is an
integer in 0..8, measured in requested-level pixels. Square windows contain all
existing image pixels within that radius. They clip at the image edge, not the
inspection ROI or render tile edge; no reflected/replicated neighbors are added.
A singleton image remains a one-sample window.

`local_statistics_rgb(input_tile, output, image_bounds, radius)` requires exactly
that halo and returns a `LocalStatisticsTile`: interleaved RGB float64 `mean`,
float64 `variance`, uint32 `finite_count`, output bounds/radius and original
descriptor. Nonfinite values are excluded independently per channel. Zero-count
windows have NaN moments; one finite sample has variance zero. Signed values and
headroom survive. Variance is the population moment (divide by finite count),
in squared units of the selected output domain. It is not a noise estimate.

Horizontal Welford moments run left to right, followed by top-to-bottom centered
second-moment merges. This avoids subtracting nearly equal raw squared sums and
squared means. Each window uses the same arithmetic order across render tile
partitions. Exact partition parity is tested for identical upstream samples;
custom upstream nodes still own their render consistency. High-offset controls
include one-ULP deviations around float32 1e20. Floating rounding remains.

`analyze_local_rgb` overloads for Node + native output bounds, ImageGraph and
ExecutableEditGraph deliver temporary result tiles to a callback. They validate
viewport/level, request each full halo and retain no full-frame result. Callbacks
must copy retained data. C++ cancellation checks surround rendering, computation
and callback delivery, including the final callback. No inner-render or row-loop
preemption is claimed. Errors/cancellation stop delivery; earlier callbacks remain.

## Python streaming

All owned session types expose:

```python
import array

def inspect(tile):
    mean = array.array("d")
    mean.frombytes(tile["mean_f64"])
    variance = array.array("d")
    variance.frombytes(tile["variance_f64"])
    count = array.array("I")
    count.frombytes(tile["finite_count_u32"])
    # Consume this tile; retain copies only when needed.

progress = session.analyze_local_manifest(
    saved_manifest, inspect, {"tile_size": 256}, radius=3
)
```

The saved output tap determines the analyzed domain. Request dicts accept only
existing viewport/tile/mip/quality controls; radius is a separate keyword-only
integer. The method releases the GIL for native work, reacquires the calling
thread's original interpreter state for callbacks and returns
`{"completed_tiles": ..., "pixel_count": ...}` after successful completion.
Callback exceptions propagate unchanged and stop delivery. Return values are
ignored. This is synchronous streaming; no analysis scheduler jobs or Python
cancellation tokens are exposed.

Payloads are owned dicts: `version=1`, `bounds`, `radius`, `mip`, `quality`, copied
`descriptor`, and `mean_f64`, `variance_f64`, `finite_count_u32` bytes. Storage is
native-endian contiguous interleaved RGB, exactly width*height*3 entries. Counts
are at most 289. Retaining every payload retains a full-frame result by choice.
A callback can clear the cache or close the session; the already-built graph/source
snapshot remains owned through this call. New requests after close reject.
No history revision or source mutation is created.

## Convolution node and saved operation

`ConvolutionNode(input, native_bounds, ConvolutionKernel)` accepts scene-linear
ProPhoto/D50 or Rec.2020/D65 RGB. Odd kernel dimensions must each be in 1..17;
row-major coefficients must match width*height, be finite and have magnitude
<=65536. No automatic normalization. Center is width/2,height/2. Actual convolution
flips both axes: `(kx,ky)` samples `(x+width/2-kx,y+height/2-ky)`. An asymmetric
horizontal `[1,0,-1]` kernel gives positive 2 on a unit ramp's interior and positive
1 at its endpoints, using the declared replicated boundary.

Outside the true image bounds, replicate the nearest edge pixel. Internal tile
edges never replicate. Coefficients accumulate in saved row-major order in float64,
casting once to float32. The descriptor, signed values and headroom are preserved;
no clip/display transfer. Nonfinite inputs or out-of-finite-float32 outputs throw.
A centered delta kernel bypasses arithmetic/halo expansion exactly, including
signed zero. Disabled operations use existing exact graph bypass.

Saved type `rawengine.convolution` uses schema 1, processing version 2, one
`image` input, matching input/output domains (scene-linear when enabled), normal blend, opacity
1 and no masks/extensions. Parameters are exactly:

```json
{"width":3,"height":1,"coefficients":[0.25,0.5,0.25],"border":"replicate"}
```

It is opt-in through C++ or an explicit saved manifest; no recipe adds it.
Parameters/version/source/upstream signature participate in cache identity.
Replay, jobs, history and footprints use existing paths. Halos compose through
earlier filters and RAW demosaic/calibration. Mip 1/2 previews convolve reduced
upstream samples with the same radius in reduced pixels; this is not equivalent
to convolving native pixels before reduction. Old graphs/demosaic policies remain.

## Memory, complexity and validation

Local statistics retain one halo RGB tile, horizontal moments and one result
tile. Output storage is 60 bytes/pixel. At tile 256/radius 8, output is 3.75 MiB,
horizontal moments ~4.78 MiB, RGB halo ~0.85 MiB: ~9.38 MiB transient native storage,
plus upstream/cache allocations and a ~3.75 MiB Python payload during delivery.
Moment size uses current 64-bit MSVC layout; other ABIs can differ. A frame-sized
tile can allocate frame-sized intermediates. No allocator trace is claimed.
Local work scales with tile pixels times window diameter; direct convolution
scales with tile pixels times kernel area. No SIMD, separable-kernel detection,
GPU or parallel accumulation is included.

`tests/spatial_ops_tests.cpp` covers affine/constant truth, nonzero origins,
complete ROI halos, exact partition parity, nonfinite channels, high-offset
variance, independent concurrency, malformed tiles/options, cancellation/callback
errors, flipped kernels, borders/singletons, signed-zero delta identity, composed
support and overflow. Seven Python cases use independent neighborhood and reversed-
kernel oracles; both working spaces, RAW bilinear/Menon/calibrated taps, transformed
and reduced outputs, multi-source graphs, cache revisions, replay/jobs/history,
copied payloads, callback exceptions, close-during-callback snapshots and validation.

This is a functional bounded foundation. Production curves/levels, guided/bilateral
filters, richer statistics/boundary modes, performance qualification and adaptive
NR remain separately tracked in the build plan.

## Fixed-camera checkpoint

The ignored `build-msvc-release/research/spatial-camera-check.py` uses the existing
ISO20000 background/skin ROIs from `_DSC1793`, both demosaicers and unchanged
source/calibrated taps. All eight base crop hashes equal the earlier NR stage
report. Twenty-four local-statistics cases (radii 1/3/8) match independent centered
integral-window means/variance/counts; five explicit centered neighborhoods per
case provide a second oracle. Max mean error is 8.327e-17, variance error
3.880e-14. All 24 tile-256/64 result buffers are identical. Eight convolution
cases (horizontal signed derivative and 3x3 uniform kernel) match independently
shifted float64 accumulation/cast exactly and retain exact tile parity.

On i9-14900K, default MSVC Release/OpenMP build, five warm local-statistics repeats
per 256-square case include graph/cache lookup, allocation and discarded Python
payload copying. Median per-case medians are 4.812/7.447/14.169 ms for radii 1/3/8;
these warm requests add zero cache misses. Convolution medians are 1.510 ms for
3x1 and 2.479 ms for 3x3, with a freshly uncached convolution result and its whole
calibrated input halo deliberately warmed before each timing. These include graph
lookup/cache storage and materialized Python bytes, not just kernel arithmetic.

The full workflow is 3.228 s after input hash/loading checks; source initialization
297.115/291.404 ms remains PERF-003. Peak process working set 332,529,664 bytes
includes full Bayer ownership, NumPy/cache/other intermediates, not isolated scratch.
No previous comparable primitive baseline exists; these are measured costs, not
regressions. PERF-009 tracks repeated centered-window work/moment storage;
PERF-010 tracks direct kernel-area work. Larger frames/tiles, maximum kernels,
concurrent memory and allocator costs remain unmeasured. Optimize only after a
focused profile, retaining border/numerical/partition/version evidence.

Report `tests/rawfiles/_librawops_local/high-iso/spatial-v1/spatial-camera-check-v1.json`
has SHA-256 `63d1aa60189da5982c3a3740f99c9cdc237e5dad2940448ef2f878597406f56a`.
It binds current native/helper/source/decoded/ROI inputs and runtime settings.
Full rebuilt CTest passes 30/30 default, 18/18 core-only and 19/19 LittleCMS;
temporary installation includes the exported header, binaries and existing notices.

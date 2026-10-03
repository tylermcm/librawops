# Bounded cubic resize contract v1

Status: **bounded native foundation verified, 2026-10-02**.
This selects the measured sharper-resampling foundation in the
[build plan](plan/LIBRAWOPS_PLAN.md). The earlier
[candidate draft](RESAMPLE_CONTRACT_DRAFT_V1.md) and its reports remain historical.

## Exported control and admission

`CubicResizeSettings` owns positive uint32 `width,height`, default zero and
therefore invalid. `validate_cubic_resize_settings(settings,native_input_bounds)`
validates both canvas and source bounds. `CubicResizeNode(input,bounds,settings)`
requires explicit copied settings, immutable nonempty input and addressable
uint32 source pixels; exclusive source ends may equal 2^32. Output bounds are
`{0,0,width,height}`. Accept only scene-linear float32 RGB ProPhoto/D50 or
Rec.2020/D65, preserving its descriptor. Each axis admits at most fourfold
native shrink: `uint64(width)*4 >= input_width`, likewise height. Other shrink
ratios reject; existing area resize remains available. No alpha, color conversion,
automatic filter selection or kernel parameter is included.

## Coordinates, weights and arithmetic

Use strict binary64 without contraction/reassociation throughout. For native
output index o, source extent N and canvas extent O, equal N/O maps directly to
integer o with one weight1 tap, including when only the other axis changes.
Otherwise pin this order:

```text
z = clamp(((double(o)+0.5)*double(N)/double(O))-0.5, 0, double(N-1))
s = max(1, double(N)/double(O))
center = floor(z)
t = abs(distance)
K(distance) = 0                                  if t >= 2
              ((1.5*t-2.5)*t)*t+1               if t < 1
              (-0.5*(t-1))*((t-2)*(t-2))         otherwise
```

Enumerate logical integers i in ascending order from `floor(z-2*s)` through
`ceil(z+2*s)` inclusive. Raw weight is `K((double(i)-z)/s)/s`. Drop exactly zero
raw weights only. Clamp physical indices to `[0,N-1]`; retain duplicated edge
taps and their logical order, without merging. Logical indices use signed
64-bit storage. Sum raw weights in that order, starting at positive zero.
Require finite total strictly greater than0.5 and at most18 retained taps.
Normalize each raw weight by that total; require finite absolute weight at
most4. These are operational rejection rules, not condition-number guarantees.
The positive central tap ensures `center` lies inside fetched support.

Reconstruct each channel horizontally using source float32 promoted to double:
start `base = source[center]`, `v = base`. For each ordered active tap compute
`delta = weight*(sample-base)`; add `v = v+delta` only when delta is nonzero.
Reconstruct vertically with exactly the same centered-difference rule over
these horizontal **double** values. Do not cast an intermediate horizontal
plane to float32. Cast the final value once. Retain exact native identity and
constant component bits, including signed zeros, through the skipped-zero rule.
Reject final nonfinite values or magnitude beyond finite float32; underflow
is permitted. Negative lobes permit signed/headroom overshoot; do not clip.
Finite source values do not guarantee finite float32 output: real ringing can
overflow and must reject. Validate exact source rectangle/storage/descriptor
and every finite sample in its entire fetched bounding rectangle before any
identity shortcut, including unused holes.

Same-build full/tiled/ROI native and preview results must be bit-exact. Selected
independent ideal rational image comparisons allow `4e-7*max(1,max_abs_active_tap)`
plus float32 subnormal rounding. This is a diagnostic acceptance threshold,
not universal photographic or cross-toolchain equivalence. Pixel-center
arithmetic uses exactly represented integer/half-integer operands and three
rounded operations; gamma3 times2^32 bounds its absolute error by
1.4305114746093754e-6 pixels. Equal-axis integer mapping avoids phantom taps at
large coordinates. Planning and rendering share the actual floating weights.

## Levels, exact support and bounded work

Native Final, or mip1/2 Preview when upstream supports native Final and the
requested level. Upstream sampling is always native Final. Reject native
Preview, reduced Final and other mips. Each preview cell averages its 2x2/4x4
once-rounded native float32 samples directly in y-major/x-major double order,
using actual counts at final partial cells. Native Final copies directly.
Do not resize an upstream reduced image or recursively reduce previews.

`input_region` and its level variant scan active axis taps over the corresponding
native output rectangle and return their minimal Cartesian bounding rectangle.
Reject mismatched construction bounds or out-of-canvas requests. Empty output
returns empty support at source origin and renders without fetching. Use widened
origin/endpoint arithmetic. Planning uses scalar axis scans, O(width+height),
without full-frame axis tables or coordinate meshes. Compose support through
upstream geometry and native RAW demosaic halos.

Render blocks of at most `(32/scale)`-square output cells, hence at most32-square
native scratch. Each source bounding rectangle must be at most145-square;
check this before fetch. At shrink at most4, 32 centers span at most124 plus
two coordinate error bounds; support radius is at most8, giving an integer
extent below145. Guard tap counts/weights and source extents operationally
even though the selected model satisfies them. No adaptive giant source fetch.
One source payload and one native scratch coexist:

```text
145*145*3*4 + 32*32*3*4 = 264588 logical pixel bytes
```

Fixed bounded axis tables may hold at most64 axis records with at most18 taps
each; their storage and scalar double accumulators are additional bounded work.
No full horizontal intermediate plane. Release source before reduction/copy
into caller output. Caller output, upstream buffers/cache, graph copies,
allocator overhead and process working set are separate budgets. PERF-032 tags
scale-dependent tap products, duplicate edge-row work, support scans,
validation, upstream/cache/signature/copies and allocator costs for measurement.

## Saved graph and integration

`rawengine.cubic_resize`, schema1/processing2, has exactly `width,height` as
positive uint32 integers, excluding bool. Reject unknown/missing parameters and
unsupported versions, even disabled. Before enabled/disabled dispatch, validate
native upstream bounds, fourfold-shrink admission and unchanged scene-linear
working RGB domain. Thus disabling cannot make an invalid relative canvas or
unsupported input domain valid. Enabled bounds rebase to the declared zero
canvas; disabled operations preserve upstream bounds/coordinates after strict
validation. One image port, normal blend, opacity1, no masks; standard metadata
validation remains enforced.

Retain canonical operation/upstream/level cache identity, immutable history/jobs,
analysis, RAW origins/halos, multi-source conversion/mix and source replacement.
Existing ResizeNode nearest/bilinear/area, RotateNode and ProjectiveNode kernels,
versions, recipes and defaults remain unchanged. No testing UI additions.
Install the public GeometryOps API/library/binding; exclude the local testing
UI. Camera NR stays deferred until specifically resumed.

## Pre-native evidence and limits

Fixed synthetic comparisons in the candidate draft justify scale-aware
Catmull-Rom for this bounded node: enlargement preserves more selected analytic
detail, while widened support suppresses selected minification aliases compared
with unwidened interpolation. Step negative lobes, stronger enlarged grain,
replicated borders and residual aliasing remain explicit costs. This is no
universal best-filter, NR, Adobe, production-corpus or default-selection claim.
The kernel derives from [Keys (1981)](https://ncorr.com/download/publications/keysbicubic.pdf);
the compared [Mitchell-Netravali (1988)](https://www.cs.utexas.edu/~fussell/courses/cs384g-spring2016/lectures/mitchell/Mitchell.pdf)
tradeoff informs the candidate comparison. No external implementation is copied.
Our replicated boundary policy does not claim the paper's extrapolated-boundary
convergence guarantees.

Hash-bound reports under `build-msvc-release/research`: cubic-contract-probe-v1
has4097 exact dyadic kernel samples,120 rational coordinates(max7.963e-8 pixels),
1218 rational image channels(max scaled3.172e-8),51 exact ROI/mip partitions,
45 constant/extreme channels and8 expected rejections. cubic-extra-check-v1
adds5575 non-dyadic rational kernel cases(max2.498e-16),8385 phase/scale checks,
288 translated uint32 taps and mixed-zero/extreme/subnormal/ROI/mip checks.
Sampled normalization minimum0.9830668, axis L1 maximum1.2685358 and16 taps
are observations, not stronger universal normative bounds. cubic-reference-
equivalence-v1 binds72 exact scalar/vectorized tooling cases; neither prototype
establishes native memory/performance behavior.

cubic-camera-prototype-v1 binds24 selected crop-canvas camera maps and72 exact
ROI/mip partitions from twelve historical inputs. The factored polynomial
differs from the earlier general polynomial in8 maps by at most7.451e-9.
cubic-camera-artifact-check-v1 proves all24 display maps exactly match the
already inspected twelve fixed candidate board panels. Controls, inputs and
display were selected before evaluation. Full-image native camera mapping,
graph integration/full suites/performance/install are the next gates. Archive
this contract, references, reports and current precision-corrected native/log/
consumer evidence before implementing.

## Native checkpoint - 2026-10-02

Implemented [bounded scale-aware cubic resize](../CUBIC_RESIZE_CONTRACT_V1.md):exported copied CubicResizeSettings/bounds-aware validator/CubicResizeNode and strict rawengine.cubic_resize schema1/process2 canvas,including disabled fourfold-shrink/input-domain admission. Ordered Catmull-Rom normalized taps,replicated borders,double horizontal/vertical and final-once float32;32-square native blocks/145-square source cap/264588 logical pixel bytes. Full Release68/68 default,36/36 core,37/37 LittleCMS pass;seven Python integration cases,viewer9/9,adapter10/10 and installed C++/Python consumers pass. All414 frozen channel maps/276 tile-ROI requests/12 identities/54 constants/27 extreme-subnormal-zero maps exact. All108 full-image camera native/mip references/partitions exact,twelve historical inputs unchanged,twelve boards inspected. Enlarged grain/ringing/replicated borders/minification smoothing and residual aliasing declared. PERF-032 aggregate uncached/cached API9.240700/0.409550ms;identity 4.151400/0.398750ms; enlarge 9.240700/0.406550ms; shrink 36.392900/0.686400ms. Geometry kernels/defaults/policies/UI unchanged;graph3266/7378/160,142 checked/138 open,HEAD85c989d,local/no commit/push;camera NR deferred.

Source/native/full-test-log/report/install/archive bindings recorded in `build-msvc-release/research/cubic-native-final-verification-v1.json`. Normative body above unchanged from the pre-native archive. Full-image camera canvases inspected separately;no production/Adobe/cross-runtime equivalence claim.

## Bounded arithmetic-preserving performance checkpoint - 2026-10-03

Measured and reduced PERF-032 cubic work without changing frozen equations or source support. Validated identity blocks now move/rebase the fetched tile;nonidentity vertical accumulation reuses the center/consecutive replicated horizontal rows with scalar storage while retaining every ordered logical tap. Sequential old/new/new/old benchmark:36 selected synthetic cases,1008 exact repeats and unchanged source requests/payload/caps;median paired speedups identity5.1019x,enlarge1.1431x,shrink1.0436x. All414 frozen channel maps/276 tile-ROI/12 identities/54 constants/27 extreme-zero-subnormal maps exact. All108 camera references/partitions,12 historical inputs and12 already inspected boards byte-identical. Full default69/core37/LittleCMS38 and fresh installed C++/Python consumers pass. Graph3300/7437/159;142 checked/138 open,HEAD85c989d,no UI/commit/push,camera NR deferred.

The serial consumer owns a1025x769 deterministic signed/headroom raster in both working spaces. Identity/enlarge(1.5x)/shrink(approximately4x),center/true-edge,native Final/mip1/2 Preview use up-to128-square output ROIs. Seven repeats follow one warmup;old/new/new/old ordering reduces timing-order confounding but is not a confidence interval. Median aggregate before/after(ms):identity1.583325/0.350150,enlarge4.793400/4.178050,shrink34.332200/32.969975;paired per-case speedup medians are distinct statistics. Initial source-copy fractions1.59%/0.56%/3.22%;remaining time includes support scans,tap generation,complete finite validation,reconstruction,reduction,allocations/copy and instrumentation,not an isolated kernel. No horizontal intermediate plane or new allocation/cache was introduced;identity removes native allocation by transferring the validated source tile. Source/native logical ceiling264588 bytes and fixed axis tables unchanged,with only scalar row/value reuse. Camera API5.114600/0.320250ms under overlapping test work is diagnostic,not a paired camera speedup. Camera workflow22.7866182s,peak working set377544704/commit1080373248 bytes include source/cache/research arrays/boards. New native replay helper originally compared the status header to the pre-native contract;comparison now excludes only Status and appended checkpoint metadata,confirming the normative body is unchanged. No engine change was made for that tooling failure. Graphify Python-module invocation was unavailable;the installed graphify.exe CLI succeeded.

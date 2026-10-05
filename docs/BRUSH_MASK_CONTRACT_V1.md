# Raster brush mask and stroke replay contract v1

Frozen engineering contract, 2026-10-04. This is the next implementation gate
under Phase 5's existing reusable raster/brush mask row. It extends the accepted
[typed mask graph](MASK_GRAPH_CONTRACT_V1.md), [coverage source](COVERAGE_SOURCE_CONTRACT_V1.md)
and [scalar delivery/Python](COVERAGE_DELIVERY_CONTRACT_V1.md) contracts.
No broader checkbox is closed by freezing this contract. This specifies original
engine behavior; photographic or Adobe equivalence is not implied.

## Public primitive and ownership

`BrushMask.hpp/.cpp` expose `BrushMode { Paint, Erase }`,
`BrushPoint { double x, y, pressure; }`,
`BrushStroke { mode, radius, hardness, flow, opacity, vector<BrushPoint> points; }`,
`BrushMaskSettings { vector<BrushStroke> strokes; }`,
`validate_brush_mask_settings`, and `CoverageBrushNode` deriving from CoverageNode.
Construction copies settings into immutable node-owned storage and retains a
shared nonnull coverage input. Later caller edits cannot change a published node.
A raster source supplies initial coverage, including an all-zero or all-one mask.
Appending/removing/reordering a stroke creates a new edit/history revision;
there is no mutable painting buffer or implicit UI gesture state in the library.

Each stroke is one continuous polyline, with round endpoint dabs and round segment
caps. Paint increases coverage toward one; erase decreases it toward zero.
Radius and hardness are constant within a stroke. Pressure controls strength,
not radius. Flow and opacity are explicit multiplicative strength controls.
Overlapping primitives within one stroke use maximum strength, preventing repeated
points or self-overlap from accumulating extra paint. Separate strokes composite
in saved array order and can accumulate. An application that wants buildup along
a path may emit successive strokes. Variable radius, tilt, brush textures,
scatter and device event sampling remain future behaviors requiring versions.

## Admission and coordinate system

Coordinates are absolute native mask coordinates; the sample for pixel `(x,y)` is
the center `(double(x)+0.5, double(y)+0.5)`. The mask's integer native origin is
retained, and coordinates are independent of tiles, viewport and preview mip.
Finite x/y lie in `[-2^33, 2^33]`, covering every admitted uint32 mask origin.
Points outside the canvas are permitted: strokes are evaluated in that coordinate
system and clipped by the requested canvas pixels. No renormalization or edge
replication is applied to brush geometry.

Radius is finite in `[2^-8, 2^20]` native pixels. Hardness, flow, opacity and every
pressure are finite binary64 in `[0,1]`. Each stroke has 1..65536 points.
Settings admit 0..4096 strokes and at most 1048576 points in total. These explicit
resource limits are checked with overflow-safe totals before publishing a node.
An empty stroke is invalid, even when flow/opacity is zero or the operation is
disabled. An empty stroke list is valid. Unsupported enum values, null input,
invalid/nonempty-overflowing native extents and invalid controls are rejected.

Native extent admission matches existing mask/source behavior: width/height are
nonzero, and each uint64 origin+dimension is at most UINT32_MAX. Complete actual
input ROI/storage and finite `[0,1]` values are validated before any no-op or
endpoint return. No epsilon, coverage gamma or automatic edge refinement exists.

## Ordered numeric map

Replay uses IEEE binary64 nearest-even with gradual underflow; each arithmetic
stage below rounds separately, without FMA/reassociation or changes to the
caller's floating environment. Final alpha and each stroke's coverage output are
stored as binary32 nearest-even. Numerical zeros newly generated are positive.

For each stroke, evaluate every point dab and every adjacent-point segment at
the native pixel center. Each primitive contributes `kernel * pressure`, rounded
once to binary64; take the maximum of these values starting from positive zero.
All geometry and strengths are independent of traversal of other pixels.

For a point dab, the center and pressure are the saved point. For a segment A/B:

1. `vx = B.x-A.x`, `vy = B.y-A.y`, `dx = pixel.x-A.x`, `dy = pixel.y-A.y`.
2. `vvx = vx*vx`, `vvy = vy*vy`, `den = vvx+vvy`.
3. If den is zero after rounding, use A's center and `max(A.pressure,B.pressure)`.
   This includes duplicate points and underflowed tiny segment lengths.
4. Otherwise `nx = dx*vx`, `ny = dy*vy`, `num = nx+ny`.
   If num<=0, use A exactly; if num>=den, use B exactly. Otherwise
   `t = num/den`, `tx = t*vx`, `ty = t*vy`, `cx = A.x+tx`, `cy = A.y+ty`.
   Pressure uses `dp = B.pressure-A.pressure`, `tp = t*dp`,
   `p = clamp(A.pressure+tp,0,1)`. Endpoint coordinates/pressures are copied.

For the selected center, `qx = pixel.x-cx`, `qy = pixel.y-cy`,
`sx = qx*qx`, `sy = qy*qy`, `q = sx+sy`.
Prepare `r2 = radius*radius`, `h2 = hardness*hardness`, `inner2 = h2*r2`.
The original soft round kernel is linear in squared distance:

| Condition | Kernel |
|---|---|
| q >= r2 | positive zero |
| q <= inner2 | one |
| otherwise | clamp((r2-q)/(r2-inner2),0,1), subtracts then divide |

At hardness one this is an open hard disk: the circumference is zero, its
interior one. Hardness zero still gives one at the exact center. There is no
sqrt, transcendental function, implicit antialiasing or subpixel sampling.
Endpoint dabs are included independently of the projected segment pressure;
inserting additional variable-pressure points can change the envelope. There is
no promise of invariance to an application's event resampling.

After the maximum primitive strength M, compute `f = M*flow`, `w = f*opacity`
in that order, then store w to binary32 alpha. Alpha zero copies the current
coverage bits. Alpha one stores one for paint, positive zero for erase.
Otherwise promote current binary32 a and binary32 alpha to double and map:

| Mode | Explicit binary64 stages, then binary32 output |
|---|---|
| paint | remaining=1-a; scaled=alpha*remaining; mapped=a+scaled |
| erase | remaining=1-alpha; mapped=a*remaining |

The next stroke reads that stored binary32 output. There is no merge, reorder,
single-round fusion of strokes or saturation arithmetic borrowed from mask-add.
All admitted intermediates remain finite: bounded coordinates/differences and
squares are far below binary64 overflow; guarded projection divides only inside
`0 < num < den`; strengths and convex coverage maps remain `[0,1]`.
Kernel/pressure clamps are explicit rounding guards, not input repair.

## Native, reduced and source footprints

The enabled node supports native Final/Preview and mip-1/2 Preview when its input
supports native Final. It always requests the underlying input at native Final,
paints those native samples, then reduces for preview. Native Preview therefore
uses the same painted pixels as native Final. This choice also applies to an
empty stroke list or zero-strength strokes: upstream reduce-first algebra is
not silently substituted for native replay followed by reduction.

Reduced output extent is rebased to zero with ceil(width/2^mip),
ceil(height/2^mip). A reduced ROI requests exactly its clipped 2x2 or 4x4 native
blocks, translated by native origin. Each reduced sample averages painted
binary32 values in global row-major order using staged binary64 sum/division
and one binary32 store, with actual edge counts. Mip 2 is direct, not cascaded.
`input_level` returns native Final; `input_region_level` returns that native
block ROI, validating requested level and matching input extent. Upstream graph
planning composes this mapping to every source ID. Painting has no spatial read
halo: geometry is retained node metadata, not a sampled source dependency.

Rendering owns a requested native coverage tile and, for reduced output, an
output tile. Validate capacity before allocations. For native block N and output
P, scalar sample storage is analytically 4N+4P bytes during reduced assembly;
native output can reuse the owned 4P input tile after validation. This excludes
upstream working sets, settings copies, cached returns, job/Python result copies
and allocator overhead. No full-canvas painted image is retained or required
for a small ROI. A straightforward pixel-by-stroke/primitive implementation is
valid initially; optimization and controlled large-brush profiling are deferred.
Cooperative job cancellation remains at output tile boundaries, with no new
in-node preemption or latency claim.

## Saved graph, identity, cache and Python

Type `rawengine.mask_brush`, schema 1, processing 2, manifest format 4.
Input/output domains are coverage; exactly one input port `mask`, no named mask
ports, normal blend_mode, operation opacity one and no extension fields.
Parameters contain exactly `strokes`, an ordered array of objects with exactly:
`mode` ("paint"/"erase"), `radius`, `hardness`, `flow`, `opacity`, `points`.
Points are arrays of exactly three numeric values `[x,y,pressure]`; bool/string/
null are rejected as numbers. Unknown object keys, malformed nesting, invalid
controls/resource limits and wrong versions/domains/ports are rejected, including
disabled operations. The stroke opacity differs from the fixed operation opacity.

Example parameters:

```json
{"strokes":[{"mode":"paint","radius":8,"hardness":0.5,"flow":0.8,
"opacity":1,"points":[[10.5,20.5,1],[30.5,20.5,0.5]]}]}
```

An enabled signature includes the canonical complete saved operation and upstream
signature, including stroke/point order, every control and zero-strength/off-canvas
strokes. No cosmetic normalization drops saved settings. A disabled node validates
everything, then aliases its upstream signature, output and level/footprint rules.
Enabled upstream cache reuse follows exact native Final ROI keys; brush output
keys retain requested mip/quality and scalar payload kind. Clear generation,
shared budget, immutable cache returns and source binding stay unchanged.

The generic Python graph/history scalar render/submit/compare methods execute the
saved operation without adding a brush UI or mutable session API. JSON round
trips, undo/redo/reorder, old snapshot/job source lifetime, source replacement and
strict output typing must work. Brush masks can feed mask algebra and masked RGB
adjustment in either working space; they never acquire an ICC/color descriptor.
Format 1/2/3, existing mask numeric contracts and RGB/RAW defaults remain intact.

## Required evidence before implementation acceptance

Freeze independent Fraction/IEEE-stage scalar cases and native/mip image fixtures
before the C++ kernel. Include paint/erase, hard/soft center/circumference/transition,
tiny/large radius and coordinate cases, pressure endpoints/interpolation,
duplicate/zero-length paths, caps/corners/self-overlap, multiple-stroke ordering,
off-canvas clipping, shifted native origins, float32 subnormals/endpoints and
native-replay-before-reduction. Pin canonical serialized operation identity.
Prototype checks must include hand-derived anchors, endpoint/coverage bounds,
same-stroke duplicate idempotence and partition/direct mip consistency.

Native/Python integration must compare frozen bits for full/ROI/1-pixel and
multiple tile partitions at every admitted level; test actual damaged tiles even
for empty/no-op strokes, strict request/settings/saved-tree/typed-edge guards,
caller mutation isolation, source-ID footprints through upstream masks and RGB
transforms, cache reuse/change/clear/budget/generation, immutable history/restore,
async cancellation/replacement/recovery and installed exported-target/Python use.
Full configured Release suites and fresh versioned installed consumers are
required. Compare accepted prior numeric sources/fixtures byte-for-byte and
retain receipts/archives. Original 145/135/280 checklist remains unchanged until
a broader row's complete scope is proved. Keep routine cost observations in
[PERFORMANCE_ISSUES.md](PERFORMANCE_ISSUES.md); finish features before performance fixes.

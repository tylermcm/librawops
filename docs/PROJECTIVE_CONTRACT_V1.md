# Bounded projective transform contract v1

Status: **bounded native foundation verified, 2026-10-02**.
This is the Phase 4 perspective/geometric-transform foundation in the
[build plan](plan/LIBRAWOPS_PLAN.md). The historical
[scoping draft](PROJECTIVE_CONTRACT_DRAFT_V1.md) remains evidence of the earlier
row-only proposal. This contract selects whole-output-cell blocks instead.

## Exported control and admission

`ProjectiveSettings` owns positive uint32 `width`, `height` (default zero, hence
invalid), and `std::array<double,9> source_from_output` (default identity).
`validate_projective_settings` checks these fields. `ProjectiveNode(input,
native_input_bounds, settings)` requires explicit settings and copies them.
Input and bounds are immutable, nonempty, and addressable through the final
uint32 pixel; exclusive ends may equal 2^32. Output is `{0,0,width,height}`.
Accept only scene-linear float32 RGB ProPhoto/D50 or Rec.2020/D65, preserving
the descriptor. No alpha, color conversion, hidden crop, fill or auto expansion.

The nine row-major inverse-homography coefficients must be finite in [-16,16],
with m22 exactly one. Compute the determinant in strict binary64 as:

```text
det = (a*(e-f*h)-b*(d-f*g))+c*(d*h-e*g)
```

Require `abs(det) >= 2^-20`. This operational admission rule is not a condition
number or forward-inversion guarantee; negative determinants admit reflection.
At all four normalized corners u,v in {-1,+1}, require ordered denominator
`(g*u+h*v)+1 >= 0.25`. Multiplication/addition are monotone in each argument
over the normalized canvas, so interior denominators cannot fall below the
corner minimum. Recheck the denominator at each sample. No fitting, forward
matrix inversion, landmarks or automatic perspective controls are included.

## Coordinates and reconstruction

For integer native output x,y, pin these operations, with no contraction or
reassociation:

```text
u = 0 if width==1 else (2*double(x)-double(width-1))/double(width-1)
v = 0 if height==1 else (2*double(y)-double(height-1))/double(height-1)
d = (m20*u+m21*v)+1
nx = ((m00*u+m01*v)+m02)/d
ny = ((m10*u+m11*v)+m12)/d
sx = ((nx+1)*0.5)*double(input_width-1)
sy = ((ny+1)*0.5)*double(input_height-1)
```

Identity matrix with equal input/output extents maps integers directly, retaining
native identity bits. Identity with different extents uses endpoint alignment;
it intentionally differs from ResizeNode's extent-ratio pixel-center mapping.
Clamp sx,sy to the true source edge before choosing floor/next taps. Next equals
floor for zero fraction; otherwise cap next at the final source pixel. Add the
input origin after local tap selection using widened arithmetic.

Use the [rotation](ROTATE_CONTRACT_V1.md) ordered bilinear rules: horizontal
top/bottom double blends then vertical double blend, `a+t*(b-a)` with endpoints
and numerically equal values returning the first operand. Cast once to float32;
reject nonfinite or values beyond finite float32, permit representational
underflow and signed/headroom values. Validate bounds, exact RGB storage,
descriptor and every finite sample of each fetched rectangle, including unused
holes, before identity shortcuts. Do not clip color.

Same-build full/tiled/ROI native and preview pixels must be bit-exact. Native
identity and constants retain component bits, including mixed zero signs under
the first-operand rule. Independent ideal bilinear comparisons allow
`4e-7*max(1,max_abs_active_tap)` plus float32 subnormal rounding. Selected
independent normalized-coordinate diagnostics use a preselected 1e-4 pixel
threshold; this is not a universal real-homography or cross-toolchain guarantee.
Planning and sampling must share the actual floating map rather than ideal
corner extrema. Border replication can streak, explicit canvases crop, and
bilinear minification can alias; none implies production photographic quality.

## Levels, source support and bounded work

Native Final only, or mip1/2 Preview when upstream supports both native Final
and the requested level. All upstream sampling is native Final. Reject native
Preview, reduced Final and other mips. Each preview cell directly averages its
2x2/4x4 once-rounded native float32 samples in y-major/x-major double order,
using actual counts on final partial cells. Native Final copies values directly.
No recursive reduction or reduced-coordinate homography is allowed.

`input_region` and its level variant scan exact active taps of the corresponding
native output rectangle, returning their minimal source bounding rectangle.
Reject mismatched construction bounds or out-of-canvas requests. Empty output
returns empty support at source origin and renders without fetching. Planning
allocates no image/coordinate buffers; large viewports may cost a scalar scan.
Support composes through upstream geometry and native RAW halos.

Render top-level blocks of at most `(128/scale)`-square output cells, hence at
most128-square native scratch. Scan the block's native tap support. If its source
rectangle is at most257-square, fetch/validate and transform the native block,
release the source, then reduce/copy into the caller output. If oversized, split
the longest output-cell dimension in half (width wins ties); visit left/top
before right/bottom. Split only on whole output-cell boundaries. Parent frames
hold scalar rectangles, not buffers; at most14 splits from a128-square block.

If a single preview cell still has oversized source support, process its at
most4-square native scratch in global row order. Split each native row span
left-before-right until its fetched support is at most257-square. A single
native sample uses at most2-square taps, so the fallback terminates within two
row splits. Then reduce that cell in the same direct order. One preview cell
never straddles independently reduced leaves.

At most one source RGB payload `257*257*3*4 = 792588` bytes and one native scratch
`128*128*3*4 = 196608` bytes coexist: **989196 logical pixel bytes**. Scalar
reduction sums/recursion are additional bounded work. Caller output, upstream
buffers/cache, allocator overhead, graph copies and process working set are
separate budgets. No full-frame native temporary or coordinate mesh. PERF-031
tags repeated scans, tiny fetches, source bounding amplification, validation,
upstream/cache/copy costs and allocator behavior for native measurement.

## Saved graph and integration

`rawengine.projective`, schema1/processing2, has exactly `width`, `height`,
`source_from_output`. Width/height must be positive uint32 integers, not bool;
the matrix must contain exactly nine finite numeric non-bool values. Reject
unknown/missing parameters, unsupported versions and invalid matrices even when
disabled. Enabled bounds rebase to zero with the declared canvas; disabled
operations preserve upstream bounds/coordinates after strict validation.
One image port, unchanged scene-linear domain, normal blend, opacity1, no masks;
standard graph metadata validation remains enforced. No recipe/default/UI change.

Retain canonical upstream/operation/level cache identity, immutable history/jobs,
analysis, multi-source conversion/mix and source replacement behavior. Native
RAW origins must rebase through this geometry and compose demosaic footprints.
Installation exports GeometryOps and the library/binding; the local testing UI
is excluded. Camera NR remains deferred until explicitly resumed.

## Pre-native gate evidence

Reports under `build-msvc-release/research` bind their helpers and inputs:
`projective-extra-check-v1.json`:10395 independent rational bilinear samples
(max1.1920929e-7),378 constant/extreme channels,63 mixed extreme/subnormal maps,
30 amplified exact ROI/mip partitions, threshold/rejection and257-square cap.
`projective-precision-check-v1.json`:208 matrices,208 rational determinants,
60112 denominator checks and11648 rational coordinates; max determinant error
5.0185576e-14, coordinate error2.1631631e-6 pixels, four floor disagreements.
These are selected precision diagnostics, not universal error bounds.
`projective-block-check-v1.json`:240 exact row/block/direct partitions,
75 constant/extreme checks, max split depth11,880 stored amplified-cell fallbacks
and32 virtual uint32-source preview fallbacks. Sampled fetches29719 row vs10208
block are prototype work counts, not timing/performance claims.

`tests/rawfiles/_librawops_local/high-iso/projective-prototype-v1/`
`projective-artifact-check-v1.json`:144 exact crop-canvas prototype native/mip
checks,12 unchanged historical camera inputs and12 camera boards. Fixed identity,
shear, perspective and reflection controls and the fixed step/grid/checker/
impulse board were inspected before native implementation. Observed mirrored
content, smooth grid deformation, replicated corner streaks, checker aliasing
and local bilinear smoothing are declared behavior. Crop-canvas prototypes do
not establish full-image native mapping, NR benefit, Adobe equivalence or
production/corpus quality. Native integration/full suites/camera/timings/install
remain the next gate. Archive current rotation source/native/log/consumer and
these normative/prototype bindings before native edits.

## Native checkpoint - 2026-10-02

Implemented [bounded projective/perspective mapping](../PROJECTIVE_CONTRACT_V1.md): exported copied ProjectiveSettings/validator/ProjectiveNode,strict rawengine.projective schema1/process2 explicit canvas/inverse normalized matrix,determinant/pole admission,ordered bilinear and exact tap support. Whole-cell adaptive blocks preserve native-before-mip1/2 order with bounded257-square sources/128-square native scratch and tiny-row fallback. Full Release66/66 default,35/35 core,36/36 LittleCMS pass;seven Python integration cases,viewer9/9,adapter10/10 and installed C++/Python consumers pass. All1728 frozen channel maps,1152 tile/ROI requests,12 identities,144 constant channels and48 extreme/subnormal/zero maps exact. All144 full-image-normalized camera references/partitions exact,twelve historical inputs unchanged,twelve native boards inspected. Explicit crop/replicate streaks,flat corner clamp and bilinear smoothing/aliasing declared;production/Adobe/cross-runtime/full-frame/concurrency/allocator gates remain open. PERF-031 aggregate uncached/cached API5.083950/0.341100ms;PERF-028 logs longer concurrent full suites without causal regression claim. Rotation implementation body/defaults/UI unchanged;graph3186/7180/166,checklist140 checked/138 open,HEAD85c989d,local/no commit/push;camera NR deferred.

Native report SHA256 `e993999fedaadad2cb117540490cef517f39464fd967dadeb983f1cdd10832e7`;camera report SHA256 `d69d09d3f60a2dd6dc8409d852cdc0e4009af2a7b6680db11d481143f170d0bc`. Installed public consumers pass with eight headers/native/CMake exports/two notices and no testing UI. Current source/native/log/report/install bindings are recorded in `build-msvc-release/research/projective-native-final-verification-v1.json`. The normative body above is unchanged from the pre-native archive. Full-image camera mapping differs from crop-canvas prototypes and was inspected separately. No production/photo/profile/Adobe/cross-runtime equivalence claim.

## Saved-angle precision correction - 2026-10-02

Corrected saved rotation angle parsing to retain binary64 precision: the shared float scalar helper rounded near-quarter-turn angles and caused the prior90 nonexact frozen channel maps. New focused regression fails against the archived previous DLL and passes current native. All1134 rotation channel maps now EXACT;756 tile/ROI,14 identities,126 constants unchanged. Projective1728 maps/1152 partitions remain exact. Both144-case camera reruns are exact;all24 existing inspected board hashes and24 historical input records unchanged. Fresh default66/66,core35/35,LittleCMS36/36 and installed C++/Python consumers pass. Geometry kernels/frozen normative arithmetic,legacy Resize/defaults/policies/UI unchanged. New final audit geometry-manifest-precision-final-verification-v1.json binds archive/current native/logs/consumers. Graph3194 nodes/7193 links/157 communities;140 checked/138 open;no commit/push,camera NR deferred.

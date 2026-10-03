# Bounded arbitrary-angle rotation/straightening - contract draft v1

Status: **scoping draft; not frozen or implemented**.

This is the next Phase4 geometry gate in the
[authoritative build plan](plan/LIBRAWOPS_PLAN.md). Existing quarter-turn
OrientationNode and bilinear/area ResizeNode remain unchanged. Camera NR stays
explicitly deferred. Perspective,expanded canvases,transparent/background fill,
automatic crop and higher-order reconstruction are separate contracts.

## Proposed bounded policy

Choose a fixed-size canvas W by H with zero-based output origin, centered at
`cx=(W-1)/2`, `cy=(H-1)/2` in local pixel-center coordinates. Input native
origin may be nonzero; retain it when requesting actual source coordinates.
One finite binary64 `angle_degrees` control in[-180,180],default0; positive
angles rotate clockwise in the image's y-down coordinate system. This serves
manual straightening and arbitrary-angle fixed-canvas rotation. Corners sample
replicated true source borders; streaks/cropping are explicit limitations.

Input/output RGBFloat32 scene-linear ProPhoto/D50 or Rec.2020/D65; no transfer,
clipping,gamut conversion,profile inference or alpha. Signed values/headroom
are sampled in their actual units. Native output extent remains W,H even at90
degrees. Non-square90-degree output therefore differs from dimension-swapping
OrientationNode; do not route old operations through this control.

Proposed ordered inverse coordinate map for native output integer x,y:

```text
dx = double(x)-cx; dy = double(y)-cy
sx = (cx + cos_theta*dx) + sin_theta*dy
sy = (cy - sin_theta*dx) + cos_theta*dy
sx = clamp(sx,0,W-1); sy = clamp(sy,0,H-1)
```

All operations binary64 without contraction/reassociation. Exact angle0/±90/
±180 use exact algebraic coefficients. Other angles evaluate radians as
`(angle_degrees/180)*pi`,with pi pinned to binary64`0x1.921fb54442d18p+1`.
The supported sin/cos implementation and accuracy/provenance limits need
explicit acceptance; no portable cross-libm exact-pixel claim is assumed.

Use floor(sx/sy),next real pixel capped to W-1/H-1,and fractional weights.
Proposed bilinear order: horizontal top, horizontal bottom, then vertical.
Each binary64 blend uses `a+t*(b-a)` with endpoint/copy rules: t0 returna,
t1 returnb,a==b returna. Cast once after vertical interpolation; exact integer
sampling copies original float32 bits. Constant numeric channels stay constant;
signed-zero behavior at mixed-zero taps requires a precise branch contract.
Validate every returned input region's bounds/storage/descriptor/finite samples
before identity or unused-tap bypass. Finite extreme interpolation/cast handling
must be proven before freezing; do not clamp valid source samples to hide errors.

## ROI and preview architecture

Reuse the existing geometry rule: native rotation first,then direct2x2/4x4
averaging of the once-rounded native output for mip1/2 Preview. Reduced output
is ceil(W/scale) by ceil(H/scale),zero-based;last partial cells average actual
native samples. This avoids interpreting angle/pivot in a reduced source grid.
Native Final and supported reduced Preview must compose through the existing
native geometry anchor; native Preview availability needs an explicit choice.

Proposed exact conservative support planner scans requested native output
coordinates using the same pinned inverse map,unions the actual bilinear taps,
then adds the native input origin. This avoids relying on rounded corner extrema
to contain every floating interpolation tap. A rotated footprint is returned as
one conservative axis-aligned rectangle;holes inside it are not true borders.
Integer products/endpoints widen before arithmetic,including exclusive end2^32.

The reference kernel should work in aligned native output blocks no larger than
128-square,with source bounding rectangles and output/reduction scratch bounded
per block. Alignment must preserve whole2x2/4x4 reduction cells and global
row-major summation,including nonzero viewport origins and final partial cells.
Required_source_regions composes the union footprint with upstream support.
No full-frame interpolation plane or coordinate mesh is required. Caller output
storage remains distinct from capped internal scratch/cache/global budgets.

Exact footprint scanning duplicates coordinate work if render also maps samples;
PERF-030 tags that unmeasured cost,block/source bounding-box amplification,
libm setup,validation,interpolation,reduction,copies,graph/cache and allocator
gaps. Cached coordinates or corner bounds are hypotheses requiring numerical
support proofs and measurements;not incidental optimizations.

## Gates still open before freezing

1. Pin complete sampling/bit/overflow rules with independent high-precision
   coordinates and rational bilinear truth;identity,constant,signed/headroom,
   singleton/thin/non-square/quarter/near-quarter/extreme/uint32 origins.
2. Prove exact ROI/tile/source footprint,internal128-square blocking and mip
   reduction parity;include rotate-before/after-reduction and tile-edge mistakes.
3. Inspect impulse/grid/step/checker/edge and fixed-camera artifacts with declared
   replicate-border/cropped-canvas behavior;freeze controls/display beforehand.
4. Freeze settings/exported node/current-stage output_bounds/input_level/graph
   geometry propagation,type/schema/processing/disabled behavior/cache/history/
   jobs/analysis/multiple-source ownership and existing default replay.
5. Set objective tolerances,libm/platform limits,resource arithmetic and measured
   continuation evidence before native registration. Complete full builds/tests,
   installed public consumers and unchanged UI/dependency/policy/archive gates
   after implementation. No UI panel is needed.

Initial design probes are tooling evidence only. This draft closes no build
checkbox and does not authorize replacing existing immutable geometry methods.

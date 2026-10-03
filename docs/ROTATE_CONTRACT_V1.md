# Bounded fixed-canvas rotation and straightening v1

Status: **bounded native foundation verified, 2026-10-02**. Broader production
geometry and cross-runtime qualification remain open. This implements the arbitrary-angle foundation
under Phase 4 of the [build plan](plan/LIBRAWOPS_PLAN.md). Perspective, expanded
canvases, background/alpha fill, automatic crop, sharper reconstruction and
production geometric qualification remain separate open gates.

## Controls, domain and coordinates

Export `RotateSettings { double angle_degrees = 0; }`,
`validate_rotate_settings(const RotateSettings&)` and immutable `RotateNode`
from `GeometryOps.hpp`. Constructor takes an owned `shared_ptr<const Node>`,
the complete current native input `Rect`, and copied settings. Reject null,
empty/overflowing input bounds, nonfinite or out-of-range angle, and descriptors
other than RGBFloat32 scene-linear ProPhoto/D50 or Rec.2020/D65. Angle is in
[-180,180], positive clockwise in y-down coordinates. Signed samples/headroom
remain in their actual units; no transfer, color conversion, clipping or alpha.

Output extent is W by H, origin zero, even at 90 degrees. Existing dimension-
swapping OrientationNode and old resize/crop behavior remain unchanged.
Pivot uses local pixel centers: `cx=(double(W)-1)/2`, `cy=(double(H)-1)/2`.
Coordinates outside the source clamp to the actual image border before sampling.
Fixed canvas crops rotated content and replicated corners can produce streaks.

Cache immutable coefficients at construction. For angle 0, 90, -90, ±180 use
(c,s)=(1,0),(0,1),(0,-1),(-1,0) exactly. Otherwise compute binary64
`theta=(angle_degrees/180)*0x1.921fb54442d18p+1`, then `std::cos(theta)` and
`std::sin(theta)`. Require finite coefficients with absolute value at most one.
This reference uses the supported platform C++ math runtime. It does not promise
portable bit equality across math libraries/toolchains or future runtime updates.
Changes to coefficient or sampling semantics need an explicit processing/schema
revision; they must not silently replace this contract.

## Ordered sampling and bits

Use strict binary64 operations, no contraction or reassociation:

```text
dx=double(x)-cx; dy=double(y)-cy
sx=(cx+c*dx)+s*dy; sy=(cy-s*dx)+c*dy
sx=clamp(sx,0,W-1); sy=clamp(sy,0,H-1)
ix=floor(sx); iy=floor(sy); fx=sx-ix; fy=sy-iy
jx=ix if fx==0 else min(ix+1,W-1)
jy=iy if fy==0 else min(iy+1,H-1)
```

Add input origin only after local tap selection, using widened integer arithmetic.
Load the four active taps. Horizontally blend top, then bottom, then blend the
two results vertically. `blend(a,b,t)` returns a at t=0, b at t=1, a when
a==b, otherwise `a+t*(b-a)`. Numeric equality of mixed signed zeros returns
the first operand, including its sign. Exact integer selection copies source
float32 bits. Constant channel bits remain exact when all taps share those bits.
Cast once to float32 after vertical interpolation. Validate result finiteness
and magnitude against float32 max before casting; reject overflow rather than
clipping. Real representational underflow is allowed. All finite float32 source
extremes fit binary64 interpolation differences.

Validate the entire returned source bounding rectangle (bounds, descriptor,
exact RGB storage and every sample finite), including holes/unused samples,
before sampling or identity return. Validation is per block; an exception
returns no partially rendered result. Empty output queries/renders produce
empty support/storage without fetching input. Output rectangles must lie inside
the current level extent; construction bounds must match footprint arguments.

## Footprints, preview and bounded work

The support planner scans every requested native output coordinate with the
same coefficient/map/tap routine as rendering. Union active taps into one
conservative source rectangle. Zero-weight neighbors are excluded, including
identity's exact translated support. Holes in returned bounding rectangles are
not true image edges. Integer products/ends use uint64, permitting input
exclusive ends of 2^32 and the final uint32 pixel, rejecting real overflow.

Support native mip0 Final and mip1/2 Preview, following existing geometry level
admission: upstream supports both native Final and the requested reduced level.
Native Preview, reduced Final and mip>2 are rejected. `input_level` is native
Final. Rotate native pixels first, then directly average the once-rounded
native RGB over 2x2/4x4 output cells, anchored at zero. Sum each cell per channel
in global y-major/x-major binary64 order and cast once; partial last cells use
actual count. Preview is not rotation of a pre-reduced input. Reduction may
change signed-zero bits, following existing geometry averaging; native exact
identity remains a bit requirement.

Render requests in output blocks whose corresponding native rectangle is at
most 128x128. Mip blocks are at most (128/scale)-square and start on whole mip
cell boundaries. Each block fetches only its own exact scanned source bbox,
rotates to a local native RGB float32 scratch, reduces/copies into caller output,
then releases source and scratch before the next block. No full-frame temporary
or coordinate mesh. No output cell is split across blocks.

For coefficients bounded by one, each local axis spans at most 254 pixels over
a 128-square block; binary64 uint32-coordinate error is much less than one pixel.
Floor/next-tap expansion fits 257x257 conservatively. Enforce this source-block
extent bound before allocation/fetch. Maximum logical source payload 792,588
bytes plus native scratch 196,608 bytes = 989,196 bytes per block. This excludes
caller output, vector/allocator metadata, upstream stage scratch/cache/source
ownership, concurrency and global budgets. No global process-cap claim.

`required_source_regions()` composes the conservative viewport bbox with
upstream geometry/RAW halos and per-source ownership. Planning scans coordinates
without image allocation; rendering fetches separate bounded block footprints.
PERF-030 tracks duplicated map work, bounding amplification, upstream allocations,
validation, interpolation, reduction, copies and graph/cache overhead. Optimized
corner bounds or cached coordinate maps require support proof and measurements.

## Graph and public integration

`rawengine.rotate`, schema1, processing2. Exact single parameter
`angle_degrees`, finite numeric [-180,180]; bool, missing/unknown fields and
unsupported versions reject even when disabled. Preserve input/output working
domain, one image port, normal blend, opacity1, no masks or extra metadata.
Enabled geometry rebases bounds to zero; disabled geometry retains the upstream
node, coordinates and bounds after normal strict validation. Cache signature
uses canonical immutable operation settings/upstream identity; levels/tile/quality
remain existing cache-key components. No current recipe/default changes.

Saved-manifest APIs exercise C++ and Python RAW/raster/multi-source sessions,
sync/async/latest jobs, analysis, cache, history, source replacement and lifetimes.
No Python recipe shortcut or UI panel is required. Install the exported header
and verify independent C++ and Python consumers; no UI/runtime dependencies ship.

## Objective acceptance and pre-native evidence

Freeze identity/quarter coefficients, sampling branches/order, borders, support,
level admission and 128-square layout before native registration. Native output
must match the frozen prototype within `4e-7*max(1,largest absolute input tap)`
plus one minimum float32 subnormal; same-build tiled/ROI/block/preview/identity
must be exact. Coordinate sampling is defined by the actual ordered platform
map; high-precision coordinate comparisons characterize accuracy, not tap-floor
identity at integer boundaries. Selected uint32-extents error must stay below
2e-6 pixels versus high-precision exact binary64-radian truth. Ideal-degree
conversion error and cross-runtime equality remain outside that diagnostic.

Pre-native ignored tools/reports:

- `rotate-scope-probe-v1.json`: 165 identity coordinates,114 square quarter turns,
  369 conservative scanned support rectangles,738 translated endpoint checks.
- `rotate-bilinear-probe-v1.json`:11,907 exact-rational bilinear cases and603
  constant/endpoint bit checks with max/extreme/signed/headroom/subnormal taps.
  Relative error at minimum subnormal can be0.5; use the absolute allowance.
- `rotate-coordinate-precision-probe-v1.json`:660 comparisons against80-digit,
  110-term Taylor sums at exact binary64 radians;max coordinate error
  3.4534171e-7 pixels,22 floor disagreements,so corner/support shortcuts are
  not justified by small average coordinate errors.
- `rotate-contract-check-v1.json`:3,294 rational samples,max1.1920929e-7;
  306 exact native/mip ROI partitions,387 blocks,7 identity images. Deliberately
  wrong reduction order/tile-local pivot differ by1.765076/2.232178. Full-size
  step/grid/checker/impulse board inspected:bounded bilinear transitions and
  expected crop/border streaks;checker aliasing remains a declared limitation.
- `rotate-camera-prototype-v1.json`:48 fixed controls on ISO20000 crop prototypes,
  both working spaces/demosaicers,background/skin/active-edge;12 previous input
  hashes match,12 boards inspected. Bilinear smooths existing grain/detail;
  replicated corners streak at±7.5/45 degrees. This is a crop-pivot diagnostic,
  not native implementation,full-image pivot or production quality admission.

Helpers/reports reside in ignored `build-msvc-release/research`;camera report is
under ignored `tests/rawfiles/_librawops_local/high-iso/rotate-prototype-v1`.
Before-native archive must bind this normative contract,helpers/reports/artifacts,
current source/native/plan and previous sharpening full logs/consumer evidence.
Native integration,malformed input/level/schema/ownership,RAW geometry chains,
camera references,performance scope,all Release configurations and installed
consumers remain required implementation gates. Broader production,profile,
corpus,Adobe,perspective and resource/concurrency qualification remain open.

## Native checkpoint - 2026-10-02

Implemented [bounded fixed-canvas rotation/straightening](../ROTATE_CONTRACT_V1.md): exported RotateSettings/validator/RotateNode, strict rawengine.rotate schema1/process2 angle, shared exact floating-tap support, ordered bilinear and native-before-mip1/2 averaging in 128-square blocks. Full Release 64/64 default,34/34 core,35/35 LittleCMS pass; seven Python integration cases,viewer9/9,adapter10/10 and installed C++/Python consumers pass. Frozen synthetic audit:1,134 channel maps,1,044 exact,max scaled error7.9474161e-8;756 exact tile/ROI requests,14 native identities,126 constant channels,99 mixed-extreme/near-quarter native/mip checks. All144 full-image-pivot camera references and partitions exact;twelve historical inputs unchanged,twelve boards inspected. Bilinear smoothing/checker aliasing and replicated corner streaks/cropping remain declared limits. PERF-030 uncached/cached API3.411750/0.315950ms aggregate;upstream/stage/full-frame/concurrency/allocator gaps remain open. Graph3111/6982/162;checklist138 checked/138 open;HEAD85c989d,local/no commit/push. UI unchanged/unpackaged;camera NR remains deferred.

Camera report SHA256 `33ab9373fa3d73b1310c56a1596a94de0dbc5ba0bcef8dcd83205daf5cb7acc0`;native report SHA256 `2677e7bc09c78b1f15f4d0707356a3772f3cb73244b961c4b45091100c21d717`. Installed public consumers pass;eight headers/native/CMake exports/two notices,UI excluded. Current source/native/log bindings and prior archive are verified in `build-msvc-release/research/rotate-native-final-verification-v1.json`. Synthetic mismatch cause remains unisolated;all observed errors satisfy the frozen tolerance and exact same-build partition/support requirements. No production/photo/profile/Adobe/cross-runtime equivalence claim.

## Saved-angle precision correction - 2026-10-02

Corrected saved rotation angle parsing to retain binary64 precision: the shared float scalar helper rounded near-quarter-turn angles and caused the prior90 nonexact frozen channel maps. New focused regression fails against the archived previous DLL and passes current native. All1134 rotation channel maps now EXACT;756 tile/ROI,14 identities,126 constants unchanged. Projective1728 maps/1152 partitions remain exact. Both144-case camera reruns are exact;all24 existing inspected board hashes and24 historical input records unchanged. Fresh default66/66,core35/35,LittleCMS36/36 and installed C++/Python consumers pass. Geometry kernels/frozen normative arithmetic,legacy Resize/defaults/policies/UI unchanged. New final audit geometry-manifest-precision-final-verification-v1.json binds archive/current native/logs/consumers. Graph3194 nodes/7193 links/157 communities;140 checked/138 open;no commit/push,camera NR deferred.

# Linear, radial and polygon mask contract v1

Frozen engineering contract, 2026-10-04. This advances the original Phase 5
linear/radial/parametric mask implementation row after
[brush acceptance](BRUSH_MASK_EVIDENCE_V1.md). Independent pre-native numeric fixtures and the frozen checkpoint are
accepted in [contract evidence](PARAMETRIC_MASK_CONTRACT_EVIDENCE_V1.md). Native
implementation and its full acceptance gates remain required. This file
defines original geometric behavior; there is no Adobe equivalence claim.

## Primitives, ownership and input semantics

`ParametricMask.hpp/.cpp` will expose:

- `MaskPoint { double x, y; }`.
- `LinearGradientMaskSettings { MaskPoint start, end; bool invert=false; }`.
- `RadialGradientMaskSettings { MaskPoint center; array<double,4> axes;
  double inner=0; bool invert=false; }`; axes default to `[1,0,0,1]`.
- `PolygonMaskSettings { vector<vector<MaskPoint>> rings; bool invert=false; }`.
- Settings validators and `evaluate_linear_gradient_mask`,
  `evaluate_radial_gradient_mask`, `evaluate_polygon_mask`, each returning
  binary32 coverage at an explicit binary64 native-coordinate sample center.
- `CoverageLinearGradientNode`, `CoverageRadialGradientNode`,
  `CoveragePolygonNode`, deriving from CoverageNode and retaining a nonnull
  shared coverage input plus copied immutable settings.

Geometric primitives generate coverage. Nodes multiply their generated coverage
with the complete validated native input mask, permitting intersection/refinement
of an existing mask. An all-one raster input generates the geometric shape directly.
Mask-add/subtract/intersect/invert operations retain their existing independent
meaning. No implicit replacement, mutable polygon editor, device gestures,
RGB-as-coverage storage or color descriptor is introduced.

Every actual upstream ROI/storage/value is validated, even when generated shape
is zero or one. Shape zero stores positive zero; shape one copies upstream bits;
otherwise promote binary32 shape/input to binary64, multiply once, then store
binary32, canonicalizing a generated numerical zero. Settings are validated in
full before node publication, including shapes entirely outside the canvas.
The primitive APIs validate the sample center and full settings before evaluation.

## Coordinate and resource admission

Coordinates are absolute native mask coordinates. Integer pixel `(x,y)` is sampled
at `(double(x)+0.5,double(y)+0.5)`. Point/sample coordinates are finite in
`[-2^33,2^33]`, covering every admitted uint32 native origin. Off-canvas geometry
is valid and is clipped only by sampled canvas pixels. Native extent admission
matches existing masks: nonzero width/height, origin+dimension <=UINT32_MAX.

Linear start/end must have staged squared distance at least `2^-16` (minimum
segment length 2^-8). Nonfinite values, degenerate/short segments are rejected.

Radial axes are four finite binary64 values in `[-2^33,2^33]`. They represent
two forward vectors U=(ux,uy), V=(vx,vy): normalized ellipse coordinates map to
native points as `center + U*u + V*v`. This supports rotation, nonuniform size,
reflection and oblique ellipses without platform trigonometric functions.
The separately rounded determinant `ux*vy - vx*uy` must be nonzero. Each inverse
matrix coefficient, computed by separate division below, must be finite with
absolute value <=256. The limit admits circular radius 2^-8 and avoids unbounded
inverse amplification; no silent repair or normalization occurs. Inner is finite
in `[0,1]`. Geometry remains unchanged by an outside/inside selection.

Polygon admits 1..4096 rings, 3..65536 points per ring and at most 1048576 total
points, checked with overflow-safe totals before copying. Rings are implicitly
closed. Repeated points, either orientation, self-intersections, overlapping rings
and degenerate/collinear rings have the explicit even-odd/boundary semantics below.
No implicit topology repair, vertex sorting or hidden triangulation occurs.
Inversion is an actual bool in saved state; numeric/string substitutes are invalid.

## Floating-point stages

Use IEEE binary64 nearest-even with gradual underflow, separate arithmetic stages,
no FMA/reassociation and no change to caller rounding mode. Store shape to binary32
nearest-even before optional inversion. Inversion then applies the accepted
`invert_coverage` binary32-to-binary64 `1-a` and binary32 store, so inverted shape
is the exact existing mask-invert result. Canonicalize generated numerical zeros.
No epsilon, gamma, transcendental map or undocumented subpixel antialiasing exists.

### Linear ramp

Prepare `vx=end.x-start.x`, `vy=end.y-start.y`; then `sx=vx*vx`, `sy=vy*vy`,
`den=sx+sy`, each separately rounded. At sample P, `dx=P.x-start.x`,
`dy=P.y-start.y`; `nx=dx*vx`, `ny=dy*vy`, `num=nx+ny`.
If num<=0, shape is zero; if num>=den, shape is one; otherwise `t=num/den`
and store t to binary32. The gradient extends infinitely perpendicular to its
start/end direction with constant endpoint plateaus. Reversing endpoints defines
a different saved ramp; optional invert acts after its binary32 storage.

### Radial elliptical gradient

Prepare `p=ux*vy`, `q=vx*uy`, `det=p-q` and inverse coefficients in this order:
`a=vy/det`, `b=(-vx)/det`, `c=(-uy)/det`, `d=ux/det`.
At sample P, `dx=P.x-center.x`, `dy=P.y-center.y`;
`au=a*dx`, `bv=b*dy`, `cu=c*dx`, `dv=d*dy`;
`u=au+bv`, `v=cu+dv`; `uu=u*u`, `vv=v*v`, `r2=uu+vv`.
Prepare `inner2=inner*inner`.

| Condition | Generated shape before binary32 storage |
|---|---|
| r2>=1 | positive zero |
| r2<=inner2 | one |
| otherwise | clamp((1-r2)/(1-inner2),0,1), separate subtracts then divide |

Softness is linear in squared normalized distance. Inner one yields an open hard
ellipse: its circumference is zero, its interior one. Inner zero still gives one
at the center. This matches the declared brush softness convention, not a claim
of linear Euclidean-distance falloff.

### Polygon parametric mask

Traverse saved rings and their edges in order, including last point -> first.
Start parity false. For edge A/B, first test inclusive staged collinearity:
`dx=P.x-A.x`, `dy=P.y-A.y`, `vx=B.x-A.x`, `vy=B.y-A.y`;
`left=dx*vy`, `right=dy*vx`, `cross=left-right`.
If cross==0 and both P coordinates lie in the inclusive endpoint min/max box,
shape is one immediately. A zero-length edge therefore includes only its saved
point. This is explicit staged numerical boundary behavior, not arbitrary-precision
real-geometry classification. No threshold is inferred from distance.

Otherwise an edge crosses the horizontal ray when `(A.y>P.y)!=(B.y>P.y)`.
If P.y==A.y, use A.x exactly; if P.y==B.y, use B.x exactly. Otherwise
`num=P.y-A.y`, `den=B.y-A.y`, `t=num/den`, `offset=t*(B.x-A.x)`,
`intersection=A.x+offset`. Toggle parity only when P.x<intersection.
After all edges, shape is one for odd parity and zero for even. All boundaries,
including hole boundaries, are inside. Orientation has no intended topological
meaning, but order/direction remain saved because staged rounding can differ.
No bit-level resampling/reversal equivalence is promised for arbitrary coordinates.

All admitted intermediates remain finite: bounded differences/products are far
below binary64 overflow; linear divisions occur inside 0<num<den; admitted radial
inverse coefficients bound normalized coordinate magnitudes; polygon crossing
divisions occur only across a genuine finite y interval containing the sample.
Radial kernel clamp is an explicit rounding guard, not invalid-input repair.

## Levels, source-ID footprints and buffers

Enabled geometric nodes support native Final/Preview and mip-1/2 Preview whenever
their input supports native Final. They always evaluate native Final input and
native-coordinate shape, multiply/store each native sample, then directly reduce
for preview. Native Preview matches native Final. Mip-2 is not cascaded.
Reduced extents are rebased zero ceil(native width/scale), ceil(height/scale).
Each reduced ROI requests exactly clipped native 2x2/4x4 blocks translated by
native origin. Average stored painted/intersected binary32 values in global
row-major order, separately rounded binary64 sum/division with actual edge counts.

`input_level` returns native Final. `input_region_level` validates the requested
level/ROI and matching native input extent, then returns that native block.
Planner composes this through every upstream source ID and downstream RGB
transforms/halos. Shape geometry requires no sampled halo or additional source ID.
Do not skip upstream validation or dependency identity for constant shape output.

Validate materialized capacity before allocations. Native output can modify its
owned validated input tile. Reduced rendering retains native block N and output
P, analytical sample storage 4N+4P bytes, excluding upstream/settings/cache/job/
Python/allocator costs. No full-canvas geometric mask buffer is necessary for a
small ROI. Polygon scanning is initially per-pixel/per-edge; record observations,
defer dedicated optimization/profiling. Cancellation stays at output tile boundaries.

## Saved graph and integration

All types require format 4, schema 1, processing 2, coverage input/output domains,
exactly one input `mask`, no named mask ports, normal blend mode, operation opacity
one and no extension fields. Parameters contain exactly these required keys:

| Type | Parameters |
|---|---|
| rawengine.mask_linear_gradient | start:[x,y], end:[x,y], invert:bool |
| rawengine.mask_radial_gradient | center:[x,y], axes:[ux,uy,vx,vy], inner:number, invert:bool |
| rawengine.mask_polygon | rings:[[[x,y],...],...], invert:bool |

Unknown/missing keys, malformed nested arrays, bool/string/null numeric values,
invalid coordinates/geometry/resource limits and versions/domains/ports are
rejected even when disabled. Disabled nodes validate everything and alias upstream
output/signature/level/footprint behavior. Enabled signatures include the complete
canonical saved operation, all controls, every ordered ring/point and upstream
signature. No geometry optimization may drop saved settings or source identity.

Use existing shared scalar cache budget/generation/immutable-return semantics.
Upstream native Final reuse has native Final keys; geometric outputs retain
requested mip/quality and scalar kind. Generic Python graph/history scalar and
masked-RGB render/submit/compare paths execute the saved types. Undo/redo/restore,
pinned old sources/jobs and atomic replacement must work. No new UI or mutable
session editing API is required. Earlier RGB/RAW/ICC and mask contracts are unchanged.

## Acceptance gates

Freeze an independent Fraction/IEEE-stage oracle before native implementation.
Include linear plateaus/projection/reversed ramps, oblique/rotated/reflected radial
axes, hard/soft circumference/center/transition, inverse/degenerate guards,
polygon convex/concave/hole/self-crossing/boundary/vertex/duplicate/degenerate
cases, inversion after binary32 storage, input endpoints/subnormals, off-canvas
and shifted/extreme origins, native-before-reduction and staged multiplication.
Pin canonical serialized bytes for all three types. Hand-derived geometric anchors,
coverage bounds, selected ring orientation/hole parity and independently replayed
direct mip/ROI cells must pass before freeze.

Native and Python must match frozen full/ROI/single-pixel/multiple-tile bits at
every level; verify strict actual/settings/nested-saved/typed-edge guards, caller
ownership, source-ID footprints through upstream algebra and downstream RGB
transforms, cache change/reuse/clear/budget/generation, disabled behavior,
history/restore/pinned lifetime and cancellation/supersession/source-fault recovery.
Full configured Release suites plus fresh installed C++/Python consumers are
required. Preserve preceding accepted receipts/artifacts and unchanged numeric
sources/fixtures. Only then audit completion of the existing original implementation
row; no new headline checkboxes. Ranges, feather/density/refinement, RGBA/layers,
assets and broader quality/performance/signoff remain separate required work.

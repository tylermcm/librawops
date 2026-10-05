# Mask refinement — contract v1

Status: frozen specification, 2026-10-04. This specifies the next original Phase 5 row,
“Implement feather, density and edge-aware refinement.” It is not implementation
acceptance and does not close that row. The fixed plan remains 149/131/280.
Independent specification evidence is recorded in [numeric evidence](MASK_REFINEMENT_NUMERIC_EVIDENCE_V1.md). Native implementation and execution acceptance remain required.

## Domain and selected operations

Input/output is float32 scalar Coverage, finite and within [0,1]. No RGB channel
stands in for a mask. Every needed input sample, exact rectangle, descriptor and
storage is validated before endpoint or identity shortcuts. Sources are immutable;
settings are copied. Preserve signed-zero input bits only at specified identity
branches; newly computed zero is positive zero.

Three independent nodes allow explicit ordering. Density is mask density, not
layer opacity. Feather smooths coverage using a symmetric nonnegative finite
kernel. Edge-aware refinement filters coverage using a caller-supplied RGB guide;
it neither discovers semantic objects nor estimates depth or sensor noise.

## Density

Finite binary64 `density` in [0,1], default 1. For input a, density 1 copies a's
bits; density 0 returns 1 after validation. Otherwise perform separate binary64
stages `u=1-double(a)`, `v=density*u`, `q=1-v`, clamp q to [0,1], then convert
once to float32. This lifts unselected coverage toward full selection as density
decreases. Full selection remains full selection. Never multiply a by density:
that would instead reduce selected coverage. No implicit coupling to opacity.

## Feather

Integer `radius` in [0,32], default 0, in native pixels. Radius r uses the
binomial row `w[t]=C(2r,t+r)`, t=-r..r, in each axis. This is a separable
Gaussian approximation with exact finite support, variance r/2 per axis before
clipping; radius denotes support, not sigma. Its explicit identity is retained
in saved records; this does not implement the separate Gaussian/lens blur rows.

Generate integer weights with Pascal addition in uint64 (largest coefficient
C(64,32) fits); convert each coefficient once to binary64. Sum converted
included weights in increasing axis-coordinate order in binary64. True image
edges truncate and renormalize each axis. No reflection, replication, zero pad
or tile-edge normalization. Radius zero copies validated input bits.

For each needed horizontal center, if all included input values compare equal,
copy the center's promoted value. Otherwise accumulate separate binary64
`weight*double(sample)` products and sums, starting at positive zero, left to
right; divide once by the included weight sum. Retain binary64 horizontal
values. For each output, apply the same equality shortcut or ordered weighted
sum/division vertically, top to bottom. Clamp to [0,1] and convert once to
float32. No intermediate float32 horizontal buffer, FMA or reassociation.
Equality uses numeric equality, so signed zeros count as equal; active feather
output zero is canonical positive zero. Flat coverage is retained exactly.

Output rectangle O needs horizontal centers H with O's x interval and y expanded
by r; input I expands H in x by r. All expansions clip at true native bounds.
This equals an r halo on both axes, including for ROI and tiled execution.
Numerical sample payload is at most `4*|I| + 8*|H| + 4*|O|` bytes, excluding
upstream/cache/allocator/concurrent storage. Guards precede allocation/render.

## Edge-aware refinement

Integer native-pixel `radius` in [0,8], default 3; finite binary64 `epsilon` in
[2^-24,65536], default 2^-12. Epsilon is squared working-Y regularization,
not sensor variance. Validate both controls even at radius zero or when disabled.
Guide must be finite scene-linear RGBFloat32 in declared ProPhoto/D50 or
Rec.2020/D65 with exactly the mask's native extent. Signed/headroom RGB is valid.
Encoded/display RGB, implicit conversion and inferred profiles are rejected.

The centered regression follows the local-regression idea documented in
[the working-Y guided-filter contract](GUIDED_FILTER_CONTRACT_V1.md), but operates
on scalar coverage with an external guide and an explicit final coverage clamp.
It is a distinct operation and does not change that RGB filter's contract.
Working-Y constants are wr/wb respectively:
ProPhoto 0.28807112822929337/0.00008565396060525903;
Rec.2020 0.26270021201126703/0.059301716469861945.

Define ordered guide difference D(j,k): promote RGB to binary64; compute
dr=Rj-Rk, dg=Gj-Gk, db=Bj-Bk, ur=dr-dg, ub=db-dg, pr=wr*ur,
pb=wb*ub, s=dg+pr, D=s+pb, each a separate binary64 stage.
Do not subtract separately rounded absolute Y values.

For each coefficient center k, Wk is its square r window clipped at true image
bounds. Sum D(j,k) and `double(mask[j])-double(mask[k])` in absolute row-major
order from positive zero; divide by actual count to obtain mg and mp. A second
ordered pass computes dy=D(j,k)-mg and dp=(mask[j]-mask[k])-mp, then separate
dy*dy and dy*dp sums, divided by actual count to obtain variance and covariance.
Set denominator=variance+epsilon and a=covariance/denominator. Store mg,mp,a in
binary64. No raw-moment subtraction, hidden variance floor or guide quantization.

At output i, visit coefficient centers Ci within r, clipped at the true image
edge, in row-major order. Sum corrections
`((mask[k]-mask[i])+mp[k]) + a[k]*(D(i,k)-mg[k])` with separate binary64
stages; skip the product when a is zero. Divide the sum by actual |Ci|. Exact
zero correction copies mask[i]'s bits. Otherwise add correction to mask[i],
clamp to [0,1], convert once to float32 and canonicalize generated zero.
Radius zero copies input bits after validating both complete needed inputs.
The unclamped guided target can overshoot because weights may be negative;
the clamp is deliberate scalar-coverage behavior. Equal-Y chromatic edges are
not guaranteed preservation. No universal ULP or photographic quality claim.

Coefficient region A=expand(O,r); needed inputs N=expand(A,r), both clipped to
true native extent. Required support is 2r for BOTH mask and guide. Payload:
`16*|N| + 24*|A| + 4*|O|` bytes (mask+RGB,three double coefficients,output).
No full-image statistics/model/integral-image allocation. Work is bounded but
radius-dependent. Existing large-DAG capability-query PERF-045 remains deferred.

## Levels, footprints and saved records

Native Final/Preview; mip1/2 Preview only when all used inputs support native
Final. Process at native resolution FIRST, then reduce resulting float32 coverage
with existing direct clipped 2x2/4x4 row-major binary64 means. Radius stays native.
Preview does not first reduce the guide or mask. Native origin is retained;
reduced extent is zero-based ceil(native width/scale) by ceil(height/scale).
Map reduced output cells to their true clipped native block before adding halo.
Full source-ID footprints include every used mask/guide branch and composed
upstream geometry/halo. Return only requested output; never treat ROI edges as
canvas edges. Check nonempty uint32-addressable extents/endpoints and all size
products/vector capacities before upstream evaluation/allocation. Preview adds
4 bytes per reduced output cell while native mapped coverage is retained.

Format4/schema1/process2 types:

| Type | Exact parameters | Exact input ports |
| --- | --- | --- |
| rawengine.mask_density | density | mask (Coverage) |
| rawengine.mask_feather_binomial | radius | mask (Coverage) |
| rawengine.mask_refine_working_y | radius, epsilon | mask (Coverage), image (RGB) |

All declare Coverage input/output domains, normal blend, opacity1, empty masks,
no extra parameters/ports/extensions. Saved numbers reject booleans, nonfinite,
out-of-range values; radii must be numeric integral. No saved-key defaulting.
Validate disabled records, guide types/descriptors/extents and settings, then
alias mask execution/signature/footprint; disabled guide is not sampled.
Enabled canonical full-operation and every used upstream signature distinguish
settings even at identity endpoints. Existing cache generations/budgets, scalar
delivery, histories, pinned jobs, ownership and source replacement apply.
No default graph insertion, UI expansion or processing-version change.

## Required evidence before acceptance

Independent IEEE-stage Fraction fixtures AND hand-derived mathematical anchors:
density endpoints/monotonic direction/subnormal ties; binomial impulse/kernel
normalization/radius32/constants/borders/odd extents; stable guided centered
moments/constant guide/two-box support/isoluminant boundaries/signed extremes/
clamp and cancellation. Demonstrate native-before-mip noncommutativity and full
halo necessity. Native strict-FP replay, invalid complete-input rejection,
ROI/tile/thread identity, strict saved records, both guide spaces, source-ID
geometry, cache/history/jobs/cancellation/fault recovery, Python and installed
C++/Python consumers, all configured full suites. Broader quality, portable
numeric, patent/release and dedicated performance gates remain open.

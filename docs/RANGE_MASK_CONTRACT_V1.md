# Luminance, color and depth range mask contract v1

Frozen engineering contract, 2026-10-04. This advances the original Phase 5
`Implement luminance/color/depth ranges.` requirement after
[geometric mask acceptance](PARAMETRIC_MASK_EVIDENCE_V1.md). Independent pre-native fixtures and admission evidence are accepted in
[contract evidence](RANGE_MASK_CONTRACT_EVIDENCE_V1.md). Native implementation,
complete graph integration and full acceptance gates remain required.

## Inputs and public primitives

`RangeMask.hpp/.cpp` will expose copied settings, validators and pure scalar
evaluators, plus three immutable CoverageNode implementations:

- `ScalarRangeSettings { array<double,4> edges={0,0,1,1}; bool invert=false; }`.
- `ColorRangeSettings { array<double,3> center={0,0,0};
  array<double,3> scales={1,1,1}; double inner=0, outer=1;
  bool invert=false; }`.
- `evaluate_luminance_range_mask(array<float,3> rgb, ImageDescriptor,
  const ScalarRangeSettings&)`.
- `evaluate_color_range_mask(array<float,3> rgb, ImageDescriptor,
  const ColorRangeSettings&)`.
- `evaluate_depth_range_mask(float depth, const ScalarRangeSettings&)`.
- `CoverageLuminanceRangeNode` and `CoverageColorRangeNode` retain a shared
  coverage mask and a shared scene-linear RGB guide; `CoverageDepthRangeNode`
  retains a shared coverage mask and shared scalar depth guide. Each retains
  copied immutable settings and the exact matching native extent.

All guides are ordinary graph inputs, not hidden buffers. Luminance/color guides
must declare scene-linear ProPhoto/D50 or Rec.2020/D65. Color centers/scales are
expressed in that guide's working RGB coordinates. The engine does not infer a
different space or transform encoded RGB. Guide RGB is finite binary32 and may
be negative or exceed one. Depth uses an explicitly supplied finite scalar field
in [0,1]; zero and one are the host's normalized depth endpoints. Hosts must
normalize metric/inverse/disparity depth explicitly and handle missing values
before source admission. No geometry reconstruction, depth estimation or unknown
sample sentinel is implicit. The operation declares the scalar guide's depth
meaning; it is carried through the existing immutable CoverageImage source and
source fingerprint, rather than disguised as RGB. External asset persistence
remains required by the separate plan row.

Each node generates selection coverage and intersects it with the actual native
mask. An all-one mask generates a selection; callers can combine multiple
selections using the existing add/subtract/intersect operations. Evaluate every
actual guide and mask ROI/storage/descriptor/value before any output shortcut,
including a mask that is all zero or a selection that is everywhere zero/one.
Shared inputs must be nonnull and have identical nonempty native x/y/width/height,
with origin+dimension <=UINT32_MAX. There is no implicit alignment/resampling.

## Numerical stages and scalar range

Use IEEE binary32/binary64 nearest-even with gradual underflow and separate
arithmetic stages; no FMA/reassociation or caller rounding-mode changes. Every
generated numeric zero is stored as positive zero. Optional invert is applied
after binary32 selection storage using the accepted `invert_coverage` semantics.
Then selection zero stores +0, selection one copies input mask bits, otherwise
multiply promoted binary32 mask/selection in binary64 and store binary32 once.

Edges `[a,b,c,d]` must be finite and a<=b<=c<=d. Luminance edges are in
[-65536,65536]; depth edges are in [0,1]. Equality is valid, including all equal.
No epsilon, implicit minimum soft width or repair is introduced. Given finite
binary64 sample t, test the plateau first:

| Condition, in this order | Selection before binary32 storage |
|---|---|
| b<=t<=c | one |
| t<=a or t>=d | zero |
| t<b | separately rounded (t-a)/(b-a), then clamp [0,1] |
| otherwise | separately rounded (d-t)/(d-c), then clamp [0,1] |

Plateau-first gives inclusive hard boundaries when a=b or c=d. Four equal edges
select exactly that staged scalar value. Soft outer endpoints are zero. Division
is only evaluated inside a genuine positive transition width. Clamps account for
staged rounding; invalid settings/values are rejected before mapping.

## Luminance guide

Use the existing processing-v2 green-anchored working-Y policy. Constants are
once-rounded binary64 decimal values, preserved in fixtures as exact bit words:

| Guide descriptor | Red weight | Blue weight |
|---|---|---|
| scene-linear ProPhoto/D50 | 0.28807112822929337 | 0.00008565396060525903 |
| scene-linear Rec.2020/D65 | 0.26270021201126703 | 0.059301716469861945 |

Promote finite binary32 r/g/b. Separately compute dr=r-g, db=b-g,
pr=red_weight*dr, pb=blue_weight*db, partial=g+pr, Y=partial+pb. Do not store Y to
binary32 or clamp it before scalar selection. Neutral RGB has exact Y=g;
negative/headroom luminance participates in explicit signed range selection.
Descriptor admission applies even to otherwise constant selection settings.
These original luminance ranges do not claim perceptual lightness or Adobe
equivalence.

## Color guide

Select an axis-aligned ellipsoid in declared scene-linear RGB. Center channels
are finite in [-65536,65536]; scales are finite in [2^-8,65536]. Inner is finite
in [0,outer]; outer is finite in [2^-8,65536]. Reflection, hue wheels, perceptual
color differences and hue-wrap interpolation are not implied by this ellipsoid.
This is a complete original RGB-distance selection with per-channel width and
inner/outer softness, independent of the future compatibility-quality row.

For channels r,g,b in order, separately compute diff=channel-center[channel],
q=diff/scales[channel], sq=q*q. Compute s01=sq_r+sq_g, distance2=s01+sq_b;
inner2=inner*inner, outer2=outer*outer. Test:

| Condition, in this order | Selection before binary32 storage |
|---|---|
| distance2<=inner2 | one |
| distance2>=outer2 | zero |
| otherwise | clamp((outer2-distance2)/(outer2-inner2),0,1), staged subtraction/division |

Inner=outer creates a closed hard ellipsoid (boundary selected). Inner zero
selects its exact staged center at one. Softness is linear in squared normalized
RGB distance. Every admitted finite binary32 RGB sample and control yields finite
binary64 intermediates: the largest normalized component is below 2^137 and
three squared components remain below 2^277. Luminance intermediates are also
far below binary64 overflow. This bound does not authorize removing ordinary
input/stage/output validation in this feature implementation.

## Levels, dependency identity and storage

Enabled nodes support native Final/Preview and mip-1/2 Preview if both inputs
support native Final. Always request both inputs at native Final, map/store the
native selection/product first, then average stored native products over each
clipped 2x2/4x4 block in row-major binary64 stages with actual edge counts and
one binary32 store. Mip-2 is direct, not cascaded. Native output retains origin;
reduced output is rebased to zero with ceil-divided dimensions. ROI planning
requests exactly the same translated native block from each branch. There is
no sampled halo at the range operation; upstream RGB transforms/filters may
have their own footprints.

Extend the scalar dependency planner to represent RGB guide branches explicitly.
Walk those branches through the existing RGB planner, union per-source-ID regions
with scalar mask/depth branches, and preserve correct levels and rebased geometry.
Do not erase a guide branch because the selection or mask is constant. Recursive
scalar-to-RGB-to-scalar paths must remain acyclic under existing manifest validation.
The public singular source helper still rejects genuinely multiple-source outputs.

Validate materialized capacity before requesting/allocating buffers. Native output
may reuse the owned mask tile. With N native input pixels and P reduced outputs,
analytical simultaneously retained sample storage is 16N+4P bytes for RGB-guided
ranges (mask+RGB+output), and 8N+4P for depth-guided ranges. Native output has P=0
in this additional-output formula. This excludes upstream scratch, graph/settings,
cache, scheduler, source storage, Python and allocator overhead. There is no
full-canvas selection buffer for a small ROI. Cancellation remains at output tile
boundaries. Log cheap observations; defer profiling and dedicated optimization.

## Saved state and disabled behavior

Use format 4, schema 1, processing 2. Primary mask input and output domains are
Coverage; guide typing is checked individually at its named input port. Require
exact named inputs below, no `masks` ports, normal blend mode, opacity one and
no extension fields. Numeric fields reject bool/string/null substitutes.

| Type | Exact inputs | Exact required parameters |
|---|---|---|
| rawengine.mask_luminance_range | mask:Coverage, image:scene-linear RGB | edges:[a,b,c,d], invert:bool |
| rawengine.mask_color_range | mask:Coverage, image:scene-linear RGB | center:[r,g,b], scales:[r,g,b], inner:number, outer:number, invert:bool |
| rawengine.mask_depth_range | mask:Coverage, depth:Coverage normalized depth | edges:[a,b,c,d], invert:bool |

Validate settings, guide type/descriptor/extent, ports, versions and domains even
when disabled. Disabled output aliases the mask's pixels, signature, supported
levels and dependency footprint; it does not sample the guide. Both edges still
must exist and be well typed at publication. Enabled output signatures combine
the full canonical operation, mask signature, guide signature and typed guide
descriptor. Changing guide content/source identity, controls or working-space
meaning invalidates selection output; unchanged upstream tiles remain reusable.
Shared scalar cache budget/generation and immutable copied returns apply.

Generic Python mixed-source scalar/RGB rendering, histories and jobs must execute
these saved types without a new UI. Old-source pinning, independent source hashes,
atomic replacement, comparison/undo/redo/restore, clear generation, budget misses,
queue cancellation/supersession and source-fault recovery remain required. No
existing RGB/RAW/ICC/brush/geometry numerical contract or default changes.

## Acceptance before closure

Freeze independent Fraction/IEEE-stage fixtures before native implementation:
both spaces, neutral/signed/headroom RGB, interval endpoints/hard/singleton/soft
selection, ellipsoid scales/center/soft/hard boundary, tiny strengths, inversion
after binary32 storage, input-mask +/-zero/endpoints/subnormals, independent source
identity/canonical operation bytes and native-before-mip counterexamples. Verify
hand-derived anchors, bounds, plateau behavior, color center/axis symmetry where
staged arithmetic supports it, direct odd-edge reductions and shifted/extreme origins.

Native/Python must replay all frozen cases/frames/ROI/tile partitions exactly.
Exercise all invalid controls/descriptors/nested keys/typed edges/actual buffers,
full guide validation with zero masks, copied ownership, multi-source-ID footprints
through RGB geometry/halos and scalar algebra/ranges, disabled guide exclusion,
cache revisions/clear/budget, histories/jobs/pinned lifetime and promise-gated
clear/recovery/supersession. All configured Release suites and fresh installed
C++/Python consumers are required. Preserve earlier accepted artifacts and fixtures.
Only then close the existing original luminance/color/depth implementation row;
147 checked/133 open/280 total remains unchanged until that audit. Feather/density/
refinement, RGBA/layers, assets and broader quality/performance/release remain open.

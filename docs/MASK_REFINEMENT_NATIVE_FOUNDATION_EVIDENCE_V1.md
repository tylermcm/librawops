# Mask refinement native foundation evidence v1

2026-10-04. Implemented the native portion of the original Phase 5 feather,
density and edge-aware refinement requirement. That original row remains open:
saved registration and complete integration/installed acceptance remain required.
Fixed checklist: 149 checked / 131 open / 280 total.

## Implemented behavior

[MaskRefinement.hpp](../MaskRefinement.hpp) exports copied settings/validators,
the scalar density evaluator, and immutable CoverageDensityNode,
CoverageFeatherNode and CoverageRefineNode. [MaskRefinement.cpp](../MaskRefinement.cpp)
implements the [frozen contract](MASK_REFINEMENT_CONTRACT_V1.md): staged density
lift, separable clipped/renormalized binomial feather with double horizontal
storage, and centered two-pass external working-Y coverage regression with final
clamp. Supported guide descriptors are exactly scene-linear ProPhoto/D50 and
Rec.2020/D65. Construction validates guide extent and stores its descriptor;
actual tiles must match it. No implicit conversion or RGB-as-mask path.

All three nodes render native before direct clipped preview reduction, retain
native origins, and map reduced requests back to their native cell blocks.
Feather requests r support and refinement 2r support from both mask and guide;
true source edges alone clip neighborhoods. Settings are copied, guide/mask nodes
are retained through shared immutable ownership, and numeric buffers are local
to each render. Radius-zero operations validate actual required inputs before
copying bits, including the refinement RGB guide. Capacity/product/combined
payload guards precede upstream evaluation. Identity settings do not check
capacities for intermediate buffers that they do not allocate.

CMake adds the source, strict FP options, installed header, native replay test
and independent oracle check. EditGraph, Python entry points, scheduler and prior
mask/brush/range/RGB arithmetic sources remain unchanged. The new saved type
names still require explicit registration before graphs can execute them.

## Compiled proof

Immutable report:
`build-msvc-release/research/mask-refinement-native-foundation-v1/verification.json`,
SHA256 `04ad6cb4593678d7cc431eccf510c8ca3e64b858a74a130c01de94dbe7492f11`.
It binds current sources, frozen oracle/header/contract, build and test logs,
four DLLs and two current cp39 Python modules. All four Ninja manifests confirm
MaskRefinement.cpp is compiled with `/fp:strict`.

Default, core, LittleCMS and LittleCMS+Python builds each pass:

- 238 independent density scalar cases and all 138 native/preview frames.
- Tile sizes 1/2/8 and exhaustive contained rectangular ROIs at native Final,
  native Preview and mip1/2 Preview; bit-identical outputs.
- 2,001 independently frozen cell outputs and required-input regions, matched
  by both planning API and actual mask/RGB requests at native Final.
- Invalid controls/null/mismatched/empty/overflowing extents; malformed actual
  ROI/storage/nonfinite/out-of-domain samples, even with identity/zero settings.
- Storage overflow rejection before evaluating either input; constructor-copied
  controls and retained guide descriptor; unsupported level/upstream rejection.
- Selected four-mode ROI equality/caller rounding-control preservation and
  eight concurrent renders on selected immutable frames.

Four-mode checks do not compare every stage against an independent four-mode
oracle. They establish selected spatial equality and control preservation.
Nearest-even fixture equality is the separately proven numeric scope. FTZ/DAZ,
other compilers, universal error bounds and production photographic qualification
remain unverified. Ordinary arithmetic status flags may accrue.

Each configuration also passes the prior range, parametric, brush, mask graph,
scalar delivery, scalar source and alpha native tests: 32 focused executable
runs total. Independent oracle `--check` reproduces the complete frozen header.
All 82 pre-native archived artifacts remain exact; 52 bound prior source/numeric
artifacts remain unchanged in the current tree. This focused evidence does not
stand in for the next required full CTest and fresh installed-consumer gates.

## Deferred observations and next work

[PERF-046](PERFORMANCE_ISSUES.md) records radius-dependent neighborhood loops,
repeated guide-difference calculations and overlapping tile halos as code-level
performance candidates. Logical payload at an unclipped 256-square request is
1,327,104 bytes for feather radius32 and 3,364,864 for refinement radius8;
these are analytical sample buffers, not measured process peaks. Focused test
workflows are correctness evidence, not kernel/45MP or regression benchmarks.
No profiling/optimization pass began. Existing PERF-045 shared-DAG support
queries remain deferred.

Next: strict saved type/parameter/port/domain registration including disabled
validation/bypass and complete upstream/cache identities; mixed source-ID halo
planning; cache clear/budget/source replacement, histories, scalar jobs/fault
recovery/cancellation/ownership, Python replay, all full suites and fresh installed
C++/Python consumers. Broader layer/RGBA/asset/quality/release requirements stay
open. No UI expansion, camera NR, commit or push.

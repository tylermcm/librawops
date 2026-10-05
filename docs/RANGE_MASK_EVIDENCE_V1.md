# Luminance, color and depth range mask acceptance v1

2026-10-04. This completes the original `Implement luminance/color/depth ranges.`
implementation requirement using [RangeMask.hpp](../RangeMask.hpp),
[RangeMask.cpp](../RangeMask.cpp), strict saved types and typed mixed scalar/RGB
dependency planning. The [frozen numeric/input contract](RANGE_MASK_CONTRACT_V1.md)
and [canonical metadata correction](RANGE_MASK_CANONICAL_CORRECTION_V1.md) apply.

| Requirement | Current evidence |
|---|---|
| Luminance range | Both working-space green-anchored Y policies, signed/headroom guides, four ordered hard/soft/singleton edges, stored inversion and existing-mask product |
| Color range | Declared scene-linear RGB ellipsoid, per-channel center/scales, inner/outer squared-distance softness and closed hard boundary |
| Depth range | Explicit supplied normalized [0,1] scalar guide, ordered hard/soft/singleton edges; no implicit depth estimator or unknown-value sentinel |
| Native arithmetic/actual guards | 12,236 frozen scalar selections and actual native products; complete guide and mask validation even for zero masks, copied settings, extent/capacity guards |
| Levels/ROI | 30 native/direct mip frames, exhaustive rectangular direct-node ROI and saved full/tiled/pixel ROI/source-ID footprints at native Final/Preview and mip-1/2 Preview |
| Saved admission | Format4/schema1/process2, exact nested parameters/ports/domains, typed matching guide/mask edges, disabled mask alias with guide excluded from pixels/signature/footprint |
| Shared graph support | Iterative mixed RGB/scalar source traversal, source-ID unions through RGB-to-mask-to-RGB paths/crop/blur; bounded8-stage shared DAG footprint probe |
| State/jobs/cache | Revisions/undo/redo/restore/compare, pinned sources/jobs, source replacement and current-identity admission, warm output/revision/clear/budget behavior; promise-gated guide clear, bad_alloc recovery and group supersession for all three types |
| Python | Four range test methods cover all30 frozen frames/levels/ROI/partitions/jobs/ownership, strict state, cache/history/source replacement and both-space independent masked RGB chains. Additional20 actual zero-mask guide guards include a successfully rendered sRGB predecessor, RGB-as-depth and coverage-as-RGB, enabled/disabled |
| Package | Seven fresh exported-target C++ consumers and4 range/4 parametric/4 brush/6 delivery Python methods pass in both default and LCMS+Python installed prefixes; engine/module path/hash proofs retained |

Full Release verification is93/53/54/94 tests (default/core/LCMS/LCMS+Python).
`build-msvc-release/research/range-mask-full-v1/verification.json` SHA256
`50ee090b1ef2bbd3c1cfe2fff5d3d1241b9ab905ef7abe2e46043bf5483c7f82`. Fresh installation receipt
`research/range-mask-installed-v1/verification.json` SHA256 `f19966284efa1fdef0a73d8650f12296d8b7e5efdf6ca129984ae3aa218c4f2d`.
Additional strict-guide proof is `research/range-mask-guide-admission-v1.json`.
Native compile flags use /fp:strict in all four builds. Successful sources,
binaries and logs are bound by immutable receipts; prior56 geometric artifacts
and35 native-foundation artifacts remain byte-exact in their separate archives.

Limits are explicit. Built-in RGB RasterImage sources use zero-origin extents;
saved RGB-guided frozen frame inputs are rebased to zero without changing sampled
guide/mask values. Direct nodes test original shifted/extreme origins; depth
graphs/Python retain shifted/extreme scalar origins. CoverageImage canonicalizes
source -0 to +0, so saved graph expectations canonicalize that endpoint while
direct native source tests deliberately retain -0 to prove exact endpoint copying.
The canonical-example correction changes only opacity1 to1.0 and three hashes;
every scalar/frame bit and numeric contract is unchanged and the old bytes remain.

PERF-045 records the interrupted160-stage shared-DAG probe: existing recursive
supports_level checks revisit layer/mask-guide subgraphs despite the iterative
footprint walker. The8-stage functional test does not establish large-DAG
responsiveness. Dedicated support/preparation/compiler/allocator/performance
remediation is deferred, per the user. No production latency, arbitrary-DAG scale,
clean-machine runtime, cross-platform, photographic/Adobe quality or release
certification is implied by configured-host correctness/installation evidence.

Fixed original checklist148 checked/132 open/280 total, only the existing range
implementation row closed. Next audit the already implemented original mask
combination row against its accumulated native/Python/installed evidence, then
freeze/implement feather, density and edge-aware refinement. RGBA/layers, assets,
quality/performance/signoff remain required. No UI expansion or agent commit/push.

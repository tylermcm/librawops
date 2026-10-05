# Linear/radial/polygon mask implementation acceptance v1

2026-10-04, worktree HEAD `5792d22`. Implements the original Phase 5 row
**Implement linear/radial/parametric masks** under the
[frozen contract](PARAMETRIC_MASK_CONTRACT_V1.md) and
[independent pre-native evidence](PARAMETRIC_MASK_CONTRACT_EVIDENCE_V1.md).

## Original implementation requirement audit

| Requirement | Authoritative implementation and tests |
|---|---|
| Linear masks | Exported LinearGradientMaskSettings, validator, evaluate_linear_gradient_mask and CoverageLinearGradientNode implement staged projection, plateau endpoints, copied settings and explicit inversion. Independent reversed/oblique/minimum/large ramp fixtures match bits. |
| Radial masks | Exported RadialGradientMaskSettings, validator, evaluate_radial_gradient_mask and CoverageRadialGradientNode implement forward-axis elliptical coordinates, guarded inverse, inner plateau/squared-distance softness and optional inversion. Rotated/oblique/reflected/minimum/large/hard/soft cases match frozen bits. |
| Parametric geometry | Exported PolygonMaskSettings, validator, evaluate_polygon_mask and CoveragePolygonNode implement ordered even-odd rings with explicit staged boundary and hole semantics. Convex/concave/hole/self-crossing/duplicate/collinear fixtures pass. Full copied caller-owned rings are isolated. |
| Native/preview/ROI product | 988 independent shape fixtures match native primitives, and 432 matching-center scalar input/product cases execute actual nodes, including subnormals and signed-zero endpoints. All 36 native/mip product frames, native Final/Preview, direct mip-1/2, 1/2/8 tile partitions and exhaustive rectangular/single-pixel ROIs match frozen bytes. Native block source support is checked. |
| Saved typed integration | Strict format-4 schema-1/process-2 types validate nested controls, domains/ports/versions/extensions even when disabled. All three independently serialized canonical operation strings round trip. Native/Python malformed trees, disabled bypass, source fingerprint/extent and RGB-as-mask guards pass. |
| Cache, history and jobs | Changed inversion misses output/reuses native upstream; immutable cache returns, clear/budget and promise-gated old-generation clear checks pass. Commit/undo/redo/restore and invalid-commit isolation pass. Pinned source/settings and cancellation survive caller ownership changes. Gated active supersession and queued/later same-node source-boundary fault recovery pass for all types. |
| Python and installed use | Four Python tests compare all 36 frozen frames/levels/ROIs/tiles, typed jobs/progress, strict disabled/nested state, cache/history/restore/source replacement and pinned lifetime. All three mask types feed independently checked masked RGB in both spaces through upstream mask combination and downstream crop/blur source-ID footprints. Fresh exported-target native and installed Python consumers pass. |

This completes the declared original geometric mask implementation row. Luminance/
color/depth ranges, feather/density/edge-aware refinement, semantic mask licensing,
RGBA graph/layers/assets and broader quality/portable/Adobe/performance/signoff
requirements remain open. Polygon boundary behavior is the frozen staged numerical
rule, not exact real-geometry or bit-level arbitrary ring reversal equivalence.
No UI, automatic antialiasing, gamma, topology repair or platform trig was added.

## Ownership, guards and arithmetic

[ParametricMask.hpp](../ParametricMask.hpp) exposes immutable node-owned settings
and scalar primitives. [ParametricMask.cpp](../ParametricMask.cpp) evaluates shape
at native pixel centers, stores shape to binary32 before existing inversion,
then multiplies complete validated native input coverage with explicit zero/one
endpoints. Preview always requests native Final input and directly averages
stored native product samples using actual clipped counts. No full-canvas shape
buffer is retained for a small ROI. Earlier numeric kernels remain byte unchanged.

Native tests reject short/degenerate linear segments, singular/over-amplified/
subnormal inverse axes, nonfinite/out-of-range controls/coordinates, invalid
polygon ring/point/total counts, null/invalid extents, unsupported levels/ROIs,
wrong actual ROI/storage/NaN/out-of-range input and impossible materialized capacity
before input rendering. The admitted million-point limit is validated directly.
Copied linear/radial/ring ownership is verified. All four configured builds use
ParametricMask.cpp /fp:strict; non-MSVC builds are configured -ffp-contract=off.
Configured Windows nearest-even bits are proved; cross-platform bit replay and
alternative caller rounding qualification remain separate work.

Gated recovery injects one-shot bad_alloc at the source boundary before a healthy
request queued behind it; both queued and later same-node renders match frozen
outputs. This is not actual allocator-internal fault tracing. Supersession and
cancellation remain cooperative at output tile boundaries, without latency or
in-node preemption claims. No dedicated performance investigation was run.

## Verified build and installed receipts

Full Release suites pass **90 default / 51 core / 52 LittleCMS /
91 LittleCMS+Python**. Exact source/native/Python/log bindings:
`build-msvc-release/research/parametric-mask-full-v1/verification.json`, SHA256
`784a53e188bc13dcc656251bf3af4b7b127c0c0ea54712913839b6a7258a3382`. Frozen contract SHA256 `2f2803f5ecde309a1581726683a6876306f19a962eebe1273aa4946e31a6a438`.

Fresh default/LCMS+Python installs compile/run six exported-target consumers:
parametric, brush, scalar delivery, mask graph, coverage source and coverage alpha.
Each also passes four parametric Python, four brush Python and six scalar delivery
Python tests. Installed/copied/build DLL and cp39 module hashes match; loader proves
installed extension/engine paths using the accepted explicit engine preload.
Receipt: `build-msvc-release/research/parametric-mask-installed-v1/verification.json`,
SHA256 `7533b369b6a7247776120ea657ace84795686ac306a1073d9b32b7cad5f0db02`. These are configured-host SDK results, not clean-machine
runtime redistribution or release certification.

Preceding accepted artifacts remain byte exact under
`build-msvc-release/research/before-parametric-mask-v1/bindings.json` (45 artifacts),
including prior four DLLs/two current cp39 modules. Brush/mask/coverage source/
alpha/delivery, RGB/RAW kernels, scheduler, Python binding implementation and prior
frozen numeric fixtures are unchanged by this step. EditGraph.cpp adds strict
geometry decoders and the three scalar executable types. Generic Python methods
require no binding implementation change. Prior receipts describe archived builds.

```bat
build-msvc-release\build-pinned-camera.cmd
build-msvc-release\research\run-with-windows-env.cmd build-msvc-release\research\run-ctest-tool.cmd --test-dir build-msvc-release -R ParametricMask --output-on-failure
```

## Current continuation

One original implementation row is newly completed; denominator stays 280.
**147 checked / 133 open / 280 total**. Next freeze/implement luminance/color/depth
range mask contracts and their typed scalar integration, then feather/density/
refinement. RGBA/layers/assets/qualification remain required. Camera NR remains
deferred; no UI or agent commit/push.

PERF-043 records unmeasured polygon edge scanning, repeated radial inverse/linear
preparation and metadata/buffer ownership. PERF-028 records uncontrolled full-suite
workflow timings. Keep feature completion first; no dedicated profiling,
benchmark expansion or performance fix was performed.

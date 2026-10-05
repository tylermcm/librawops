# Raster/brush mask implementation acceptance v1

2026-10-04, worktree HEAD `5792d22`. Implements the original Phase 5 row
**Implement reusable raster/brush mask graph and painting primitives**.
[Frozen stroke contract](BRUSH_MASK_CONTRACT_V1.md),
[additive scalar API](BRUSH_MASK_PRIMITIVE_API_V1.md),
[independent pre-native evidence](BRUSH_MASK_CONTRACT_EVIDENCE_V1.md).

## Original requirement completion audit

| Requirement | Current implementation and evidence |
|---|---|
| Reusable raster mask graph | Accepted immutable CoverageImage/CoverageRasterNode, canonical source identity, scalar graph edges, shared cache and copied Python sources remain intact. CoverageBrushNode retains the shared input and copies complete stroke settings. Caller mutation checks pass. |
| Painting primitives | Exported apply_brush_mask and CoverageBrushNode implement ordered paint/erase polylines, endpoint dabs/round caps, original squared-distance softness, pressure strength, flow/opacity and per-stroke maximum envelope. All 426 independent staged scalar fixtures match bits. Separate strokes accumulate in array order. |
| Native/preview/ROI consistency | All 12 independent native/mip frames match, including odd/singleton/shifted/extreme origins. Native Final/Preview, direct mip-1/2, 1/2/8 tile partitions, all single-pixel and exhaustive rectangular native/reduced ROIs pass. Enabled empty brush proves native replay before reduction differs from upstream reduce-first algebra. |
| Saved operation and typed integration | Format-4 rawengine.mask_brush schema 1/process 2 validates exact nested fields, controls, ports, versions/domains/compositing metadata even when disabled. Independent canonical operation bytes round trip. Native and Python guards, disabled bypass and source identity/extent checks pass. |
| Reuse, source-ID planning and history | Upstream mask algebra native footprints compose through each source ID; masked RGB in both spaces and downstream crop/blur halos pass. Changed strokes reuse upstream cache and change output identity; copied cache returns, clear/budget and promise-gated in-flight generation checks pass. Ordered-stroke history/undo/redo/save/restore and old source/job lifetimes pass. |
| Job and installed use | Promise-gated active supersession, queued/later source-boundary bad_alloc recovery and cancellation pass. Four Python tests cover frozen bits, scalar/RGB jobs/progress, strict trees, ownership, reorder/history/source pinning, both working spaces and transform footprints. Fresh default/LCMS+Python installed exported-target C++ and installed Python consumers pass. |

This proves the declared original engine implementation row. It does not close
the separate rows for RGBA graph/layers, parametric/range/feather/refinement,
binary asset persistence, photographic/Adobe/portable qualification, controlled
large-brush latency/cache measurements or Phase 5 signoff. Optional variable
radius/tilt/scatter/device sampling are outside v1's declared original brush
behavior. No brush UI or product gesture state was added.

## Native implementation and numeric limits

[BrushMask.hpp](../BrushMask.hpp) exports the copied settings, validator, pure
scalar map and CoverageBrushNode. [BrushMask.cpp](../BrushMask.cpp) validates
complete actual input tiles even for empty/zero-strength settings, checks storage
capacity before rendering, replays native Final input pixels and directly reduces
painted binary32 pixels with true edge counts. There is no full-canvas painted
buffer for a small ROI. Mip-2 is not cascaded. Generated zero/endpoint rules and
every explicit binary64/binary32 stage follow the frozen contract.

Settings cap 4096 strokes, 65536 points per stroke and 1048576 total points.
Radius is 2^-8..2^20 native pixels; finite point coordinates span +/-2^33;
strength controls are [0,1]. Native tests exercise admitted total-point limit and
rejected per-stroke/total/stroke-count overflow, unsupported modes/levels/extents,
nonfinite/invalid strengths/coordinates/coverage, wrong actual ROI/storage and
impossible materialized capacity. One-shot bad_alloc is injected at the source
boundary; no allocator-internal failure attribution or cancellation-latency
guarantee is asserted. Cancellation remains at output tile boundaries.

The first full v1 pass was diagnostic: its CMake source list had not yet added
BrushMask.cpp to staged-arithmetic compiler flags. The exact pre-strict CMake
snapshot and v1 receipt/logs are retained. Before acceptance, BrushMask.cpp was
added to MSVC /fp:strict and non-MSVC -ffp-contract=off; all four builds/tests were
rerun into fresh v2 storage. V1 binaries are historical and not current acceptance.
No kernel optimization or floating-environment changes were introduced.

## Verified Release and installed receipts

Full suites: **87 default / 49 core / 50 LittleCMS / 88 LittleCMS+Python**.
Source/native/Python/log bindings:
`build-msvc-release/research/brush-mask-full-v2/verification.json`, SHA256
`db6dbee0e827794afb6d4091731e3de91683ce770bb38d44d1ef22a141d1e460`. Contract SHA256 `2e36eb769ab5a63d11b21f4f3a8008e94a002ae804a491217724444af4b887a0`; additive API SHA256 `e37f2f782525ca1f068c4851947f09582590f933026712b72356014ae7264b80`.

Fresh default/LCMS+Python installs each compile/run five exported-target consumers:
brush, delivery, mask graph, coverage source and coverage alpha. Each also passes
four Python brush tests and six scalar delivery tests. Installed/copy/build DLL
and cp39 module hashes match; loader proves installed module/engine paths using
the previously accepted explicit installed-engine preload. Receipt:
`build-msvc-release/research/brush-mask-installed-v1/verification.json`, SHA256
`c25db1c328f74308b16f05552c1d2f853f6257b449bd69bd05aac16a974ec2df`. This is configured-host SDK evidence, not clean-machine runtime
redistribution, cross-platform bit replay or release certification.

Exact preceding accepted artifacts remain under
`build-msvc-release/research/before-brush-mask-v1/bindings.json` (41 artifacts).
MaskOps, coverage source/alpha/delivery, RGB/RAW kernels, shared scheduler, Python
binding implementation and frozen prior mask arithmetic fixtures are byte unchanged
by this step. EditGraph.cpp adds the strict brush parameter decoder and executable
scalar branch; generic Python methods require no binding implementation changes.
Previous accepted receipts continue describing their archived historical builds.

```bat
build-msvc-release\build-pinned-camera.cmd
build-msvc-release\research\run-with-windows-env.cmd build-msvc-release\research\run-ctest-tool.cmd --test-dir build-msvc-release -R BrushMask --output-on-failure
```

## Current continuation

Exactly one original checkbox is newly completed. Fixed denominator stays 280:
**146 checked / 134 open / 280 total**. Continue linear/radial/parametric mask
contracts and implementation, then luminance/color/depth ranges and
feather/density/refinement. Broader alpha/RGBA/layers/assets/quality requirements
remain open. Camera NR remains deferred. No agent commit/push or UI changes.

Feature completion stays first. PERF-041 records unmeasured all-primitive brush
scanning/buffer costs; PERF-042 records observed correctness-test workflow time
and a fixture-regeneration hypothesis. PERF-028 retains full-suite timings with
uncontrolled-host/concurrent-suite caveats. No dedicated profiling/benchmark
expansion/performance fix was performed.

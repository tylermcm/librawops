# Mask refinement implementation acceptance v1

2026-10-04. Completes the original Phase 5 implementation requirement
“Implement feather, density and edge-aware refinement.” Fixed original checklist:
150 checked / 130 open / 280 total. Broader quality, layers/RGBA, assets,
performance and release requirements remain separate open rows.

## Requirement and evidence

| Requirement | Implemented behavior and direct proof |
| --- | --- |
| Density | CoverageDensityNode; copied finite density, staged `1-density*(1-a)`; 238 independent scalar cases and saved native/mip replay. Density is mask lift, not layer opacity. |
| Feather | CoverageFeatherNode; explicit native-pixel finite binomial support, clipped axis renormalization, double horizontal storage; radii0..32, singleton/odd/extreme-origin frames, exhaustive ROI/tile replay. No claim to complete the separate Gaussian/lens blur rows. |
| Edge-aware refinement | CoverageRefineNode; external scene-linear working-Y RGB guide, centered two-pass scalar regression, true two-window support and deliberate coverage clamp. Both working spaces, signed/headroom/extreme RGB, epsilon endpoints and copied descriptor/controls. |
| Borders and preview | Full r/2r native halo; native operation before direct clipped preview mean. All138 frames and2001 independent cell outputs/footprints match. NativeFinal/Preview and mip1/2Preview; tiled1/2/8 and exhaustive rectangular native API ROI comparisons are exact. |
| Strict saved execution | Format4/schema1/process2 `rawengine.mask_density`, `rawengine.mask_feather_binomial`, `rawengine.mask_refine_working_y`. Exact controls/ports/Coverage domains/normal/opacity1/no masks or extensions; numeric integral radii, bool/extra/missing/out-of-range rejection even disabled. |
| Guide admission and disabled bypass | Actual native tile/descriptor/storage/finite checks before identity return. Valid executable scene-linear-sRGB predecessor then rejects at the refinement guide boundary; RGB/scalar swaps and mismatched guide extents reject enabled/disabled in both spaces. Disabled execution aliases only mask and excludes guide footprints. |
| Source-ID composition | Unary scalar and mixed RGB/scalar dependencies use the existing iterative source planner. All cell halos, four-source reuse/merge, guide blur plus masked-RGB/crop/post-blur composition pass native/mip/partition checks. |
| Cache/history/source identity | Canonical complete operation and every used upstream signature; changed controls distinguish enabled identities even at pixel identities. Warm requests add no misses; source/guide replacement rejects stale manifests and fresh graphs match independent truth. Undo/redo/restore and failed-commit atomicity pass. |
| Jobs, ownership and recovery | Pinned scalar jobs, latest-job supersession, caller-buffer copies, after-session/history lifetime and native eight-worker immutable renders pass. Promise-gated unary scalar/RGB source faults, in-flight cache clear, queued/later recovery, cancellation and tiny-cache budget tests pass. |
| Build and installed interface | StrictFP allfour configurations; all configured full suites pass. Exported installed header/target, eight native consumers and five Python suites pass under each fresh default/LCMS+Python prefix with explicit installed module/DLL loading proof. |

The unchanged [frozen contract](MASK_REFINEMENT_CONTRACT_V1.md) and
[numeric evidence](MASK_REFINEMENT_NUMERIC_EVIDENCE_V1.md) remain authoritative.
[Native foundation evidence](MASK_REFINEMENT_NATIVE_FOUNDATION_EVIDENCE_V1.md)
records the earlier direct-API checkpoint and its limited acceptance scope.
EditGraph registration/dependency tracking and tests are new; prior RGB/RAW,
alpha/source/brush/parametric/range arithmetic, scheduler and Python production
entry points remain unchanged by this integration.

## Verified reports

- Full `build-msvc-release/research/mask-refinement-full-v1/verification.json`,
  SHA256 `a4b71a1a7cc8664f279764858c36d199c873accaae8b5d7d4ea58b14d92cfec0`:
  **96/55/56/97** passing default/core/LCMS/LCMS+Python tests.
- Installed `build-msvc-release/research/mask-refinement-installed-v1/verification.json`,
  SHA256 `03c9db2dac7161597bfde9159f84d35d547e327050de7e6f6995ede88d8a2b75`:
  eight C++ consumers and refinement/range/parametric/brush/delivery Python
  suites under each fresh prefix. Installed DLL/header/module bytes match builds.
- Supplemental `build-msvc-release/research/mask-refinement-composition-v1/verification.json`,
  SHA256 `f64693f1b0490b99abd4dea088d4a1ebe800d28a2f0434ef9d43c2df3e68f755`:
  16 guide guards and18 crop/halo/native/mip/tile compositions per current-built
  and installed default/LCMS+Python run. This separate proof does not alter CTest
  counts or replace the complete frozen numeric replay.

Before integration,42 accepted native foundation/current-handoff artifacts were
archived exactly under `research/before-mask-refinement-integration-v1`.
Before native work,82 accepted range/candidate artifacts were archived exactly
under `research/before-mask-refinement-v1`. Successful historical receipts retain
their historical bytes; the earlier documentation-only41-to52 count correction
and failed audit helpers are preserved. No numeric fixture/contract correction
was needed during implementation.

## Limits and next work

Nearest-even fixtures establish exact selected stage outputs. Selected other
rounding-mode tests prove ROI equality and caller-control preservation, not a
complete independent four-mode oracle or universal accuracy bound. Working-Y
guidance does not guarantee preservation of isoluminant chromatic edges;
unclamped regression weights can be negative, so final [0,1] clamp is intentional.
Feather radius denotes finite support, not sigma. RGB raster built-ins use zero
origin; direct nodes/unary coverage sources also prove shifted/extreme origins.
Normalized depth inputs used in composition remain supplied fields, not estimates.

Installed tests prove the configured host SDK paths, not a clean-machine runtime
or package certification. No production photographic/Adobe match, complete
Phase5 quality/signoff, RGBA layer graph, binary asset persistence, large-mask
performance or portable-compiler acceptance is claimed.

[PERF-046](PERFORMANCE_ISSUES.md) retains neighborhood/halo/storage candidates;
PERF-045 retains shared-DAG capability-query cost. Full concurrent correctness
workflow timings remain PERF-028, not kernel/regression benchmarks. Performance
fixes stay deferred until the functional build is complete.

Next in Phase5: complete float-coverage/premultiplied-RGBA graph semantics and
safe straight-color access, then shared local adjustments/layers and persistence.
Research/license and conditional semantic-model gates remain open. No UI
expansion, camera NR, commit or push.

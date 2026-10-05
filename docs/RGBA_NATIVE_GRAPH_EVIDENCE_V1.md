# Typed native RGBA graph foundation acceptance v1

2026-10-04. Completes the bounded native foundation under the original Phase 5
float-coverage/premultiplied-RGBA requirement. **That original row remains open**;
the fixed original checklist remains **150 checked / 130 open / 280 total**.

## Implemented and verified

| Requirement | Direct evidence |
| --- | --- |
| Immutable premultiplied source | RgbaImage owns canonical visible samples, ignores padding, retains nontransparent signed-zero bits, canonicalizes transparent pixels and hashes logical bounds/space/samples. Independent Python hashlib agrees with native fingerprints; caller mutation, shifted/extreme origins, space/origin changes and eight concurrent lazy fingerprint calls are tested. |
| Typed native graph | RgbaNode, RgbaRasterNode, RgbPremultiplyNode, RgbaApplyCoverageNode and RgbaSourceOverNode retain immutable input ownership and expose typed dependencies. RGB, scalar and RGBA payloads remain distinct. |
| Explicit color/alpha adapters | RgbaAlphaNode and checked-float32 RgbaStraightRgbNode; owned straight_rgb_float64 tile access matches all 120 inherited independent binary64 fixtures and safely handles tiny alpha with full-range color. No epsilon floor or clipping. |
| Frozen point arithmetic | All 496 inherited independent premultiplication/coverage/source-over/straight-float32 cases replay through native graph nodes, including expected overflow rejection. Existing CoverageOps arithmetic is unchanged. |
| Preview and footprints | All 84 independently generated frames in both working spaces at native Final/Preview and mip1/2 Preview match exact Fraction/IEEE-stage truth through 14,124 exhaustive rectangular ROI comparisons. Native operation precedes direct clipped-cell reduction. Actual RGB/scalar point requests and a shifted clipped RGBA/scalar request prove native Final input mapping. Reduced alpha underflow canonicalizes all color to zero. |
| Actual-input and resource guards | Wrong ROI, missing/extra storage, wrong space/descriptor, nonfinite values, invalid alpha and hidden transparent color reject, including zero coverage/opaque source endpoint cases. Invalid level/ROI/extent and oversized logical payload reject before upstream execution. Constructor null/extent/space guards pass. |
| Caller floating controls | Selected transparent composition runs retain all four caller rounding modes. Independent exact numeric fixtures are nearest-even only; no full four-mode or portable-compiler numeric claim. |
| Installed interface | New header is exported/installed. Fresh default and LittleCMS+Python consumers build against installed headers/targets/DLLs; a standalone public-API consumer and the complete frozen native executable pass under each prefix. Existing scalar/refinement Python suites pass with explicit installed module/DLL loading proof. New RGBA Python APIs remain pending. |

The [frozen native contract](RGBA_NATIVE_GRAPH_CONTRACT_V1.md) extends the
unchanged [alpha contract](COVERAGE_ALPHA_CONTRACT_V1.md). No existing production
RGB/scalar graph, cache, scheduler or Python source code changed in this stage.
The only build integration is the new source, strict-FP flags, native/oracle
tests and installed header. No new dependency or third-party implementation.

## Reproducible reports

- `build-msvc-release/research/rgba-native-full-v1/verification.json`, SHA256
  `2454eee38da10c833d86b8a662cfd78752864fe51f3b533cd6aacb4eb20efe9b`:
  full **98 default / 57 core / 58 LittleCMS / 99 LittleCMS+Python** tests pass.
  Sources, frozen fixtures, four DLLs, two Python modules and test/build logs
  are bound by SHA256. Four correctness suites run concurrently, each CTest-j6.
- `build-msvc-release/research/rgba-native-installed-v3/verification.json`, SHA256
  `5a24101db55bc19d345c02b17a5c5020fc9487d43e2e4d193b73bc8ea694c0c2`:
  two native consumers plus existing refinement/delivery Python suites per
  default/LCMS+Python prefix; exact installed header/DLL/module identities.
  Configured Windows SDK evidence, not clean-machine packaging certification.

Commands: `run-with-windows-env.cmd C:\Users\tylle\anaconda3\python.exe
rgba-native-verify-v1.py` and corresponding `rgba-native-installed-verify-v3.py`
from `build-msvc-release/research`, using paths relative to the repository root
as recorded exactly in report commands. Native executable:
`build-msvc-release/RawEngineRgbaNativeGraphTests.exe`. Independent oracle:
`tests/reference/rgba_native_oracle.py --check`.

Before code/build/handoff changes, 22 accepted current files and binaries were
archived exactly in `build-msvc-release/research/before-rgba-native-v1`, with
SHA256 bindings. Successful prior reports remain immutable. Failed installed
v1/v2 logs are retained: Windows backslashes in a CMake source argument, then a
consumer-only missing scene-linear working-space argument. Fresh v3 succeeds;
these failures did not alter production code or frozen numeric truth.

## Limits and continuation

Saved operation registration/schema/domains, native RGBA source binding,
typed cache accounting, complete source-ID footprint traversal, delivery/jobs,
Python sessions and history still need implementation and acceptance. Current
native pointer-based composition does not imply those paths already execute.
No RGBA spatial resampling, blend modes, layers, assets or file codecs added.

Source-over and checked straight-float32 access can reject finite inputs whose
native result exceeds float32, even when later averaging could reduce the
result. Transparent color discarded during premultiplication/underflow is not
recoverable. Straight RGB adapters average native straight colors, which can
differ from dividing already reduced premultiplied pixels. No production,
Adobe-equivalence, large-frame performance or complete Phase 5 signoff claim.

[PERF-047](PERFORMANCE_ISSUES.md) logs native block materialization and repeated
validation at composed boundaries as unmeasured improvement candidates.
Concurrent full-suite workflow durations remain PERF-028. Performance work
stays deferred; no UI expansion, camera NR, commit or push.

Next: integrate the typed RGBA sources/nodes/adapters into strict saved graphs,
source-ID footprints and correctly charged RGBA cache entries, then typed
delivery/jobs/Python/history before closing the original implementation row.

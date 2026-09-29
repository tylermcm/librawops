# LibRawOps implementation plan
## Contents

- [Current handoff — read first](#current-handoff--read-first)
- [Implementation plan and source audit](#intent-and-constraints)
- [Phases and gates](#phases-and-gates)
- [Feature and maturity matrix](#feature-and-maturity-matrix)
- [Dependency and license decisions](#dependency-and-license-decisions)
- [Adobe compatibility](#adobe-compatibility-method-and-evidence-ledger)
- [Architecture decision records](#architecture-decision-records)

## Current handoff — read first

**Purpose and update rule.** This section is the continuation point for Codex on any machine. At the end of **every implementation run**, update the snapshot, verification, open issues, and next action here in this same file; append one concise entry to the run log below. Record actual commands/results and the last code commit. Keep the detailed roadmap, dependency ledger, and ADRs below consistent. Do not create a separate handoff or assume that a prior chat is available. If a run ends before verification, mark it unverified rather than implying completion.

### Snapshot — 2026-09-29

- **Repository:** standalone `librawops` Git repository, branch `main`. Last completed code commit: `f094082` (`Choose linear ProPhoto default and add typed working-space conversion`). This handoff edit follows that commit. On another machine, inspect `git status` and `git log -5 --oneline` first; do not assume these values remain current.
- **Mission:** reusable, application-independent C++20 non-destructive RAW and raster editing library with optional native Python bindings. Do not work on or integrate with ImageTriage. The no-copyleft rule applies to **every shipped component**. The core accepts decoded uint16 Bayer data and must remain decoder-free. A permissive RAW-file decoder path is a release gate, not a prerequisite for current library work.
- **Current code maturity:** Phase 0 evidence and Phase 1 typed/color foundations are **In Progress**. This is a prototype, not a production-ready Camera Raw replacement or a stable C++ ABI. Phase 2 graph/serialization/cache has not started. Adobe reference measurements are unavailable on this work machine; do not claim Adobe compatibility.
- **Source and graph:** `RawEngine.hpp/.cpp` define validated `RawImage`/`RawMetadata`, `RasterImage`/`RasterMetadata`, typed descriptors, nodes, `GraphRecipe`, `ImageGraph`, and tiled `Renderer`. RAW has a bilinear demosaic baseline, signed black/white normalization, per-site levels, CFA phase and active area. Raster input is already scene-linear finite float32 RGB in ProPhoto/D50 or Rec.2020/D65; file codecs and ICC import are absent. The graph is currently a fixed lazy chain. It creates tiles on viewport demand but has no cache, mip pyramid, cancellation, general DAG, or edit serialization.
- **Color/output:** white balance gains and linear exposure precede an optional caller-calibrated camera RGB→XYZ D50 matrix. The graph supports linear ProPhoto/D50 and Rec.2020/D65; **ProPhoto/D50 is now the canonical default**, with explicit typed `WorkingSpaceConvertNode` between the two and no clipping. This is an architectural choice, **not measured Adobe equivalence**. The default `LegacyBounded` output is bounded but unmanaged. `SrgbPreview` converts scene-linear RGB to linear sRGB, applies the prototype tone curve, hard-clips and sRGB-encodes. `IccDisplay` accepts a thread-safe transform, validates output, and tags tiles with the output-profile digest; optional `LittleCmsBackend.hpp/.cpp` supplies an RGB display/output ICC adapter with intent/BPC and hard clipping. Raster ICC import, monitor discovery, soft proof and general export color are absent. Python exposes one-shot RAW/raster rendering and sRGB preview, but not ICC transforms or persistent objects.
- **Dependency boundary:** The default build uses C++ standard library plus optional OpenMP and CPython C API. `RAWENGINE_WITH_LCMS=ON` opts into the audited LittleCMS **2.19.1 core-only** static build; its exact archive SHA-256 is pinned in CMake. GPL fast-float and threaded plugins, tools, tests and optional codecs are disabled. The staged Windows install contains no GPL plugin binary or source; LittleCMS's installed `lcms2_plugin.h` is MIT-licensed, not the GPL plugin implementation. Keep future transitive, platform, binary and notice audits in the [dependency ledger](#dependency-and-license-decisions).
- **Key files:** `tests/core_tests.cpp` covers RAW metadata, signed headroom, camera/working/sRGB matrices, the 347-patch working-space conversion grid, tiled ROI, raster input, descriptors and mock ICC boundary. `tests/smoke.py` covers native Python entry points and the ProPhoto default. `bench/render_benchmark.cpp` measures synthetic ROI/full streaming; `bench/color_space_benchmark.cpp` measures tile-streamed conversion overhead without a 45 MP float input allocation. `README.md` is the consumer API guide. This file is the **single** living architecture/plan/handoff document.

### Verified on the work workstation

Windows work machine: Intel Core i5-14500, 15.7 GiB RAM, Intel UHD 770; VS 2022 Build Tools/MSVC 17.14, Python 3.13 available. Build directories are ignored by Git. At `f094082`, Release `build` (core + Python + two benchmarks) built and CTest passed **2/2**; Release `build-core` (Python OFF, OpenMP OFF) built and passed **1/1**; Release `build-lcms` (LittleCMS ON, Python/OpenMP OFF) built and passed **2/2**. One concurrent MSBuild `build-core` attempt hit transient `LNK1114` on its own import library; a sequential retry built and tested successfully. The previous LittleCMS Windows staged-install/DLL dependency audit remains as recorded below; it was not rerun for this code change. Portable commands:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRAWENGINE_BUILD_BENCHMARK=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake -S . -B build-core -DCMAKE_BUILD_TYPE=Release -DRAWENGINE_BUILD_PYTHON=OFF -DRAWENGINE_USE_OPENMP=OFF
cmake --build build-core --config Release
ctest --test-dir build-core -C Release --output-on-failure
cmake -S . -B build-lcms -DRAWENGINE_WITH_LCMS=ON -DRAWENGINE_BUILD_PYTHON=OFF -DRAWENGINE_USE_OPENMP=OFF
cmake --build build-lcms --config Release
ctest --test-dir build-lcms -C Release --output-on-failure
```

The latest 45 MP synthetic end-to-end benchmark **before** the ICC adapter contract is recorded in [Prototype baseline](#prototype-baseline-2026-09-29): legacy ROI 13.483 ms / full streaming 642.573 ms; sRGB preview ROI 16.075 ms / full streaming 850.809 ms. The new working-space benchmark below measures one matrix pass separately. Neither is fit-to-screen or cached-slider latency. Peak RSS, ICC backend cost, final demosaic, decode and export remain unmeasured.

### Exact next action and open gates

1. **Next bounded implementation:** build reproducible Adobe reference fixtures/capture metadata and a comparison harness that can run without Adobe installed; leave Adobe result cells **Not measured**. Use the DNG-specified ProPhoto domain for future profile maps, and test any later hue-control domains empirically. Before Phase 2 serialization, require explicit working-space and processing-version fields so old implicit Rec.2020 prototype outputs cannot silently change when an edit is loaded.
2. **ICC follow-through:** measure LittleCMS core tile cost and memory; add real-profile round-trip and representative monitor/output ICC corpus, malformed-profile/fuzz coverage, and profile-aware raster import. Include intent/BPC in future graph/cache identity separately from the profile-byte digest. Add platform and release-binary audits before shipment. Adobe comparisons remain **Not measured** until the application is available.
3. **Later gates:** Phase 2 versioned DAG and edit format together, bounded cache/scheduler and cancellation; Phase 3 production RAW calibration/demosaic/preview; permissive decoder decision before RAW-file product shipment. Full control coverage, export and optimization follow the phase criteria below. Revisit the provisional 50 ms/250 ms/10 s goals after Phase 3 on the eventual development/target machine.

### Run log

| Date | Last code commit | Result and verification | Next handoff |
|---|---|---|---|
| 2026-09-29 | `be99c2f` | Decoded RAW metadata, stride, CFA phase, active area and per-site levels; native/Python tests passed. | Explicit camera color. |
| 2026-09-29 | `5a1556a` | Optional camera→XYZ D50→linear ProPhoto/Rec.2020 matrix with numeric and ROI tests; both build configurations passed. | Raster memory source. |
| 2026-09-29 | `4dd78b0` | Typed scene-linear raster memory source and Python entry point; both build configurations passed. | Preview/output color. |
| 2026-09-29 | `34aefc1` | Explicit encoded sRGB preview, domain tests and synthetic benchmark; both build configurations passed. | ICC boundary. |
| 2026-09-29 | `01670cc` | Injectable ICC display-transform boundary with mock backend, profile digest and validation; both build configurations passed. **No real ICC backend.** | Exact-version core-only LittleCMS audit and backend. |
| 2026-09-29 | `01670cc` | Documentation-only handoff snapshot and per-run update rule added to this plan; no code changed or tests rerun. | Exact-version core-only LittleCMS audit and backend. |
| 2026-09-29 | `b9083b9` | Optional pinned LittleCMS 2.19.1 core-only ICC display adapter, exact-byte SHA-256, intent/BPC, profile validation, concurrent and tiled tests; three Release configurations passed (2/2, 1/1, 2/2). Staged Windows install and DLL dependencies inspected; no GPL plugin artifacts. | Working-space decision, ICC measurements and broader profile corpus. |
| 2026-09-29 | `f094082` | Typed ProPhoto↔Rec.2020 node; 347-patch signed/overrange round-trip and sRGB-equivalence checks; ProPhoto canonical C++/Python default; 45 MP conversion benchmark. Three Release test configurations passed (2/2, 1/1, 2/2). | Adobe reference fixture/comparison harness, then ICC corpus and cost. |

**Status:** Architecture. **Updated:** 2026-09-29. This is the authoritative roadmap. The current source is a prototype, not an API contract. Use the status vocabulary in [feature matrix](#feature-and-maturity-matrix); update this plan and the decision records when measurements overturn an assumption.

## Intent and constraints

LibRawOps is an installable, application-independent C++20 image engine with optional native Python bindings. It must support nondestructive RAW development and ordinary raster editing without UI toolkit assumptions. The no-copyleft policy applies to **every LibRawOps-distributed component**, including optional decoder adapters, modules, AI weights, profiles, and assets; no GPL, AGPL, LGPL, MPL, EPL, or CDDL code ships without explicit separate approval. The core remains decoder-free. A usable, legally acceptable RAW decoder path is a **RAW-file-support release gate**, tracked in [dependency ledger](#dependency-and-license-decisions).

First target: Windows x64, designed for Linux and later macOS. Typical inputs: 24–60 MP RAW plus 8/16-bit JPEG, PNG, TIFF. Work is sequenced for one developer. CPU correctness and profiling precede GPU implementation. Python is optional; a C++ consumer does not depend on Python.

**Provisional design goals, not measured guarantees:** after the first render, fit-to-screen slider updates under 50 ms; 100% viewport updates under 250 ms; full-quality 45 MP export without heavy denoise under 10 s. Revisit after Phase 3 and again on the eventual development/target hardware. This work machine is a plain workstation used as a lower-end reference benchmark, **not** the planned development machine: Intel Core i5-14500 (14 cores/20 threads), 15.7 GiB RAM, and Intel UHD Graphics 770. Its timings are useful for regression and memory-budget checks, but do not establish performance on the future machine. Record hardware role, display resolution, power mode, compiler, and exact test image with every run.

### Prototype baseline, 2026-09-29

Original Release build on the machine above, Python 3.13, synthetic 7500×6000 RGGB image with repeating values, default recipe, one timed run after process startup: 1024×768 ROI **38 ms**; full 45 MP materialized render **729 ms**; process peak working set **1,231 MiB**. This measured the initial bilinear/color/tone path and Python copies, before signed normalization and tile-domain changes. It includes no file decoding, ICC conversion, export encoding, full-quality demosaic, denoise, or fit-to-screen reduction. It cannot demonstrate any of the three design goals. The current API has no fit-to-screen render mode or export path.

First native C++ benchmark after the signed-normalization change, Release on the same work machine, synthetic 7500×6000 RGGB with spatially varying values, default recipe, 256-pixel tiles, median of three runs: 1024×768 materialized ROI **9.620 ms**; 45 MP **streaming** render **368.385 ms**. Run `RawEngineBenchmark.exe 7500 6000 3 256` after building with `RAWENGINE_BUILD_BENCHMARK=ON`. This is a different input and output mode from the original Python result, so the figures are **not** a speedup comparison. Peak RSS is not yet measured by this runner. Repeat on representative camera images and eventual target hardware.

After adding descriptor validation and a separate output clip, one work-machine run of the same native command measured **13.133 ms** ROI and **851.098 ms** full streaming. Other runs varied substantially under the current workstation load; these figures are diagnostic snapshots, not a regression gate or a measured optimization result. Recheck with fixed power mode and idle background load before attributing a cost to any stage.

After adding explicit sRGB preview, Release native runs on the same work machine (VS 2022 Build Tools/MSVC 17.14, display resolution and power mode unrecorded), synthetic 7500×6000 RGGB with spatially varying values, 256-pixel tiles, median of three runs: `RawEngineBenchmark.exe 7500 6000 3 256 legacy` gave **13.483 ms** for a 1024×768 materialized ROI and **642.573 ms** for a 45 MP streaming render. With `srgb-preview`, which adds a synthetic neutral camera→Rec.2020 matrix, linear Rec.2020→sRGB matrix, hard clip and sRGB transfer, the corresponding medians were **16.075 ms** and **850.809 ms**. This is a path comparison on synthetic data under uncontrolled workstation load, not a fit preview, warm-slider, ICC, final demosaic, or export measurement. Repeat with fixed power mode, representative images, stage-level timings and peak RSS before drawing performance conclusions.

### Working-space conversion evidence, 2026-09-29

`RawEngineColorBenchmark.exe 7500 6000 5 256` on the same Release MSVC work machine generates signed/over-range float32 pixels **per tile** and streams 45 MP with no full-frame float input. Latest median-of-five run after rebuilding the final benchmark source: ProPhoto source **39.266 ms** versus ProPhoto→Rec.2020 **97.150 ms**; Rec.2020 source **43.012 ms** versus Rec.2020→ProPhoto **93.443 ms**; ProPhoto→Rec.2020→ProPhoto **110.858 ms**. Earlier runs had source **38.725–52.087 ms** and one-conversion **92.063–94.454 ms**. The per-direction incremental estimate on the latest run is roughly **50–58 ms per 45 MP pass**; timing order, caches and uncontrolled power/background load limit that estimate. It is a narrow matrix/tile cost measurement, not a full RAW render, fit preview or export. The 347-patch native grid checks neutral, saturated, signed and over-range round trips plus equal linear-sRGB output to within **1e-5 float units**. It is a synthetic numeric check, not a perceptual or Adobe-reference comparison.

## Source audit and disposition

| Concept and source | Disposition | Evidence and migration |
|---|---|---|
| RawImage / RawMetadata in RawEngine.hpp | Keep owned decoded source; expand metadata | Owns immutable uint16 samples and validates dimensions, row stride, CFA phase, active area, and four site-specific black/white pairs. Camera matrices, illuminants, orientation, optical-black handling, masked pixels, per-row levels, and noise data remain absent. |
| RawUnpackNode | Rename and replace | It normalizes and bilinear-demosaics; it does not unpack a file. Normalization now preserves below-black and over-white samples. Preserve the bilinear code only as a known baseline/preview implementation. |
| RasterImage / RasterSourceNode | Keep typed memory source; add import adapters later | Owns immutable, finite, scene-linear interleaved float32 RGB with declared ProPhoto/D50 or Rec.2020/D65 and optional row padding. It supports tiled ROIs without a full-image intermediate. Encoded file decode, ICC/profile conversion, alpha, and orientation remain absent. |
| CameraToWorkingNode / WorkingSpaceConvertNode | Keep typed matrix foundation; expand profile semantics | The former maps a caller-calibrated camera matrix into ProPhoto or Rec.2020; the latter explicitly converts between those spaces with signed/overrange preservation. ProPhoto is the current default. DNG matrix interpretation and profile-table application remain absent. |
| WhiteBalanceNode / ExposureNode | Keep mathematical baseline; remodel | Gain and 2^stops are clear and now reject tone-mapped input; no temperature/tint model, baseline exposure, full image descriptor, or validated stage order. |
| ToneCurveNode / OutputClipNode / SrgbEncodeNode / IccDisplayNode | Temporary | The signed tone curve and final clipping are separate. The sRGB preview performs standard component encoding; the ICC node accepts a host or optional LittleCMS core adapter and validates its output. No measured ACR equivalence, monitor-profile discovery, soft proof, or perceptual gamut workflow exists. |
| Tile float RGB | Keep descriptor; expand it | `Tile::descriptor` distinguishes camera-linear, scene-linear working, tone-mapped, bounded unmanaged, display-linear sRGB, encoded sRGB, and ICC output. ICC output carries a supplied exact-profile SHA-256 digest. Camera profile identity and arbitrary built-in output conversion remain absent; optional ICC parsing is limited to RGB display/output profiles. |
| Node::render(Rect) and ImageGraph | Keep demand-driven idea; replace interface | A fixed RAW or scene-linear raster chain with no multiple inputs, halo, coordinate transform, operation ID, cache key, quality level, or cancellation. |
| GraphRecipe | Temporary | Fixed gain/exposure/tone fields, optional camera color transform, and output-mode switch cannot represent arbitrary ordered operations, masks, layers, or versioned persistence. The host ICC transform is passed separately at graph construction. Do not append hundreds of controls. |
| Renderer::render_tiles / render_image / render_roi | Keep streaming entry point; replace scheduler | Serial synchronous tile callback and descriptor-bearing materialized ROI; legacy floats-only result remains. No cache, request priority, mipmap, progress, cancellation, or thread-safe render session. Full 45 MP RGB float output is about 540 MB before copies. |
| OpenMP loops | Prototype only | Parallel entry in several nodes per tile. Keep simple loops while benchmarked scheduler/task pool replaces per-node parallelism if beneficial. |
| RawEnginePython.cpp | Replace for long-term API | Separate synchronous RAW and scene-linear raster entry points copy their input and result and construct a graph for each call. No persistent edit state or asynchronous work. |
| CMake, README, LICENSE, tests | Keep minimal build; strengthen tests | C++20 Release builds with native core/Python tests, two optional native benchmarks, and a pinned optional LittleCMS build. Shared DLL still exposes STL types without ABI policy; own and optional LittleCMS MIT notices are installed, while a full distribution SBOM/release audit remains. |

Missing entire systems: full ICC color management, camera profiles, raster codecs, typed metadata beyond current source fields, graph serialization/history, masks/local edits, layers/blends, export, cache/mipmaps, cancellation/errors, broad numerical/reference regressions, and performance monitoring.

## Recommended architecture

~~~mermaid
flowchart LR
  A[External RAW decoder or raster codec] --> B[Typed sensor or raster source]
  B --> C[RAW calibration and camera profile]
  C --> D[Scene-linear working image]
  B --> D
  D --> E[Versioned edit graph]
  M[Shared raster and parametric masks] --> E
  E --> L[Layers and compositing]
  L --> S[Tile scheduler and bounded caches]
  S --> P[Preview display transform]
  S --> F[Full-quality render]
  F --> O[Output transform and export]
~~~

### Image and color semantics

Each buffer/node output declares pixel format, channel order, profile/primaries and white point, transfer function, scene- versus display-referred state, and alpha mode. CFA samples are unsigned sensor codes, not RGB. RAW normalization yields **signed float32 camera-channel** values; white balance and color conversion may create values below 0 or above 1. The canonical editing buffer is **scene-linear float32 ProPhoto/D50 with no implicit clamp**; Rec.2020/D65 remains a supported explicit domain, with a typed conversion node. The choice follows the DNG profile-table domain and current numeric/performance evidence, **not** an ACR matching measurement. An operation requiring encoded RGB, Lab, XYZ, or a Photoshop-like display domain declares conversion explicitly. Masks are float32 coverage [0,1]. Composite color uses premultiplied float32 RGBA; blend formulas receive straight color after safe unpremultiplication. External raster import records straight/premultiplied state, ICC profile, and transfer. Preview and export each perform an explicit view/output transform, gamut mapping policy, then quantization or float export. Use float64 for matrix fitting, profile calculations, and selected reductions; consider float16 only for measured-safe caches or GPU textures. Reject or sanitize nonfinite data at documented boundaries and preserve a diagnostic count.

ICC handling uses a wrapped LittleCMS **core-only** adapter. Its core is MIT, but its fast-float and threaded plugins are GPLv3 and are excluded; see [dependency ledger](#dependency-and-license-decisions). Support embedded/monitor/output ICC, sRGB, Adobe RGB, Display P3, ProPhoto, linear variants, XYZ, Lab, white-point adaptation, rendering intent, black-point compensation, gamut warnings, and soft proofing in phases. Camera DNG color/forward matrices and dual-illuminant interpolation are separate RAW transforms. Adobe's [DNG specification](https://helpx.adobe.com/camera-raw/desktop/dng-and-file-formats/digital-negative.html) is the primary documented reference; actual ACR rendering is measured, not presumed to be fully specified.

### RAW input and stage order

The core accepts a decoded sensor plane plus a validated typed metadata object. **Implemented subset:** dimensions/stride, CFA pattern and phase, sensor-relative active area, and 2×2-site black/white levels. Requested ROIs and bilinear halos stay within the active area. **Still required for production camera support:** optical-black/masked pixels, per-row levels, linearization, orientation, make/model, as-shot neutral, analog balance, calibration illuminants, color/forward/reduction matrices, baseline exposure, lens ID/crop, bad-pixel map, and noise profile. Mark each field required, optional with documented fallback, or unsupported before accepting a camera.

Initial order to validate: active-area interpretation → sensor linearization and black subtraction → defect correction → normalization without implicit clip → highlight reconstruction and white-balance calibration → RAW-domain denoise → demosaic → camera profile to scene-linear working space → lens geometry and CA correction → global/local edits → view/export transform. This is an **Inferred design sequence**; operation-domain tests and the DNG reference/Adobe corpus decide exact WB, highlight, and denoise placement before production implementation. Bilinear remains a preview baseline. Final demosaic candidates require independent implementation from papers/specifications, license and patent review, seam tests, and quality/throughput comparison. A full proprietary-camera decoder is not assumed: format variation and missing public specifications make an in-house broad decoder a high-cost research option.

### Graph, state, and renderer

Represent each edit as a stable operation type ID, schema version, processing version, instance UUID, enabled flag, typed parameters, named input edges, optional mask edges, blend/opacity settings, and declared input/output domains. Nodes are immutable; a document edit creates a new graph revision. Serialization design is **part of graph design**, even if disk persistence lands later: versioned JSON manifest plus separate binary mask/raster assets, canonical hashes, explicit migrations, unknown-operation preservation, and processing-version pinning. Presets are partial graph-state patches with declared mask inclusion and absolute/relative parameter behavior. Undo/redo and snapshots reference graph revisions rather than pixel copies.

Each operation reports upstream ROI expansion/coordinate mapping, halo radius, edge policy, mip support, preview/final quality, and resource estimate. Scheduler requests only needed upstream tiles. Keys include source fingerprint, graph/node schema and processing versions, parameters, input hashes, color configuration, tile coordinate, mip/quality, and backend. Bounded CPU/mask/intermediate/GPU caches use LRU eviction; late edits reuse upstream tiles. Render sessions provide cancellation, priority, progress, supersession of obsolete slider requests, and thread-safe concurrent ROIs. Failures use typed errors for unsupported source/metadata, corrupt input, allocation failure, cancellation, codec failure, and backend failure. GPU execution is optional behind the same logical operations with CPU fallback.

Layers (pixel/adjustment, groups, clipping, opacity/fill, blend mode, smart-transform equivalents) are engine document semantics. Widgets, tool gestures, layout, and presentation are application concerns. One mask system provides brush, linear/radial, luminance/color/depth, add/subtract/intersect/invert, feather/density, and edge-aware refinement. Semantic AI masks are gated by model and dataset licenses. Photoshop blend compatibility may require an explicit encoded/display-referred mode instead of silently changing the native scene-linear model.

### APIs and library boundary

Expose C++ image-source, decoded-RAW, raster-source, document/graph, edit, render-session, and exporter interfaces with ownership/lifetime and thread-safety contracts. Keep implementation types private and version public schemas. Bind persistent objects to Python as an optional package; release the GIL for native rendering, support NumPy arrays with explicit lifetime rules, and expose async cancellation/progress. Do not freeze a cross-compiler C++ ABI during architecture phases. Keep a minimal CMake build and tests first; add install/export targets, static/shared variants, symbol visibility, package config, and semantic versioning after the public model stabilizes. A C ABI is optional only if a real consumer needs it.

## Phases and gates

Each phase below has a status in [feature matrix](#feature-and-maturity-matrix). A phase is **Complete** only when its applicable tests, documentation, and measurements pass on pinned inputs and hardware. Adobe comparisons are a separate compatibility status and do not block foundational library work while Adobe applications are unavailable. No feature is labeled Adobe compatible without controlled Adobe references.

### Phase 0 — Evidence and baseline (In Progress)
- **Objective:** establish reproducible source, license, quality, and performance evidence before replacing prototype behavior.
- **Why now / affected code:** Capture behavior of all prototype files before changing color semantics. Keep the current build and smoke test.
- **Add:** repeatable benchmark runner, synthetic color/geometry corpus, redistributable RAW/raster corpus manifest, numerical test conventions, source/license inventory, and an Adobe reference capture template for later use. Record the current work-machine benchmark above with its hardware role.
- **Research:** corpus redistribution rights; optional decoder candidates and library packaging boundary; plugin/transitive licenses; patent questions.
- **Tests/Adobe/performance:** existing build and smoke test plus 7×5, ramps, negative/overwhite, tile-edge and malformed-input baselines. Use published specifications and independent numeric oracles now; prepare the ACR/Photoshop capture protocol without requiring either application. Measure warm/cold ROI, 45 MP materialized render, allocations and peak RSS on this work machine; current fit preview/export are unsupported.
- **Complete when:** baseline commands, hardware/inputs, limitations, and all existing behavior are reproducible and documented.

### Phase 1 — Typed image and color foundation (In Progress)
- **Objective:** make every pixel's domain and transform explicit, and prepare controlled Adobe comparison for when an Adobe reference machine is available.
- **Why now / affected code:** RawImage, Tile, RawUnpackNode, WhiteBalanceNode, ToneCurveNode and README currently conflate sensor, camera RGB, working RGB, and display output.
- **Add:** descriptors, typed RAW metadata, an in-memory raster source API, signed float32 rules, explicit clip, ICC adapter/core-only build, camera matrix and working-space transforms, preview/output transforms. The optional LittleCMS 2.19.1 core-only backend is now implemented and scoped in the [dependency ledger](#dependency-and-license-decisions); continue with profile-aware input, monitor/output policy, real-profile corpus and measured performance.
- **Research:** DNG matrix/profile interpretation, chromatic adaptation, profile provenance, gamut/headroom behavior, ICC intent/BPC equivalence. Linear ProPhoto/D50 is the current canonical choice; measure ACR slider domains separately and revisit only with versioned evidence.
- **Tests/Adobe/performance:** numeric matrices and ramps, embedded profile round trips, negative/overwhite retention, monitor proof cases. **Start the Adobe harness design here:** define controlled neutral/color ramps, RAW neutral cases, capture settings, and expected metrics; leave Adobe result cells **Not measured** until the application is available. The first 347-patch working-space round trip and 45 MP tile conversion timings are recorded above; expand to representative profiles/images, perceptual metrics, ICC cost and memory. Defer ACR hue-slider output-error comparison until reference access.
- **Complete when:** every rendered buffer has an unambiguous domain, no undocumented clipping remains, specification-based color cases and seams pass, and ADR 0002 records a technically justified canonical working-space choice with Adobe compatibility still provisional if not measured.

### Phase 2 — Graph, edit format, scheduler and cache (Not Started)
- **Objective:** replace the fixed chain with a serializable edit DAG and a responsive, bounded renderer.
- **Why now / affected code:** Replace GraphRecipe, Node::render(Rect), ImageGraph, Renderer and Python one-shot call before adding controls.
- **Add:** versioned operation schemas and serialization format together, immutable DAG, ROI/halo/transform mapping, multi-input graph, mip/quality requests, bounded caches, cancellation, task scheduling, history-state revisions.
- **Research:** optimal tile size and scheduler vs OpenMP; stable hash/canonicalization; migration policy for unknown operations.
- **Tests/Adobe/performance:** tiled=untiled for point/neighborhood/geometry operations, deterministic replay, edits/migrations, cache invalidation, simultaneous ROI and cancellation tests. Prepare one simple-curve Photoshop comparison; run it when Adobe access is available. Measure cache hit rate, late-slider latency, allocations and thread scaling.
- **Complete when:** a late edit reuses early tiles, a halo operation has no seams, a saved graph replays identically, and obsolete previews cancel.

### Phase 3 — Production RAW, raster import and preview quality (Not Started)
- **Objective:** deliver quality RAW development, common raster input, and responsive previews on the new color/graph foundation.
- **Why now / affected code:** Upgrade the Bayer stub only after color and scheduling contracts are stable.
- **Add:** metadata-driven sensor calibration, defects, highlights, WB/temperature/tint, RAW denoise, production demosaic, camera profile, lens/CA primitives, reduced-resolution RAW preview and progressive refinement. Add approved JPEG/PNG/TIFF **read** adapters to the Phase 1 raster source API so Phase 4 controls can operate on files; keep export encoders for Phase 6.
- **Research:** exact stage order, demosaic/denoise patents and independent algorithms, camera-profile data rights, optional decoder-module feasibility, and exact-version/transitive licenses for raster readers.
- **Tests/Adobe/performance:** camera/ISO/WB corpus, false color/zipper/moiré, shadow/highlight, tile edges, malformed JPEG/PNG/TIFF, embedded ICC raster input, and prepared ACR color/tone cases for later capture. Measure fit/100% preview, 45 MP stage throughput, peak RAM; **revisit provisional latency targets at this gate** and on target hardware when available.
- **Complete when:** supported camera metadata is validated, final rendering beats the bilinear baseline on defined quality cases, approved raster readers feed the graph correctly, preview and final are consistent within documented bounds, and targets/gaps are reported.

### Phase 4 — Reusable global editing (Not Started)
- **Objective:** build global photo controls from shared mathematical and image-processing primitives.
- **Why now / affected code:** Add controls through graph operations, never through special-case renderer branches.
- **Add:** matrix and color conversion, 1D/3D LUTs, curves/levels, histograms, convolution, bilateral/guided filters, local statistics, resampling, geometric transforms, morphology and FFT only when justified. Build tone, color mixer, grading, clarity/texture/dehaze, sharpening/denoise, crop/rotate/perspective and resize from these.
- **Research:** operation domains, algorithm patents, Adobe control characterization.
- **Tests/Adobe/performance:** numeric and image regressions per primitive, operation schema round trips, isolated ACR and Photoshop comparison fixtures, MP/s and cache effect. Run Adobe comparisons when those applications are available.
- **Complete when:** each shipped global control integrates with graph, cache, Python, serialization, tests, and measured compatibility status.

### Phase 5 — Shared masks, layers and local edits (Not Started)
- **Objective:** provide one local-edit and compositing architecture for all tools.
- **Why now / affected code:** Reuse Phase 2 DAG and Phase 4 primitives rather than tool-specific mask paths.
- **Add:** raster/parametric mask graph, brushes and gradients, ranges/depth, mask combination/refinement, local adjustment node, groups, clipping, opacity/fill, blend modes, healing/clone/content removal and blur.
- **Research:** Photoshop blend-domain/alpha behavior; AI model and weight licenses; healing patents.
- **Tests/Adobe/performance:** mask algebra, edge/feather seams, alpha ramps, layer ordering, PSD comparison renders where valid, large-brush latency and cache budgets.
- **Complete when:** masks are reusable across tools, history/serialization round trip, and blend differences are quantified.

### Phase 6 — Export, metadata and interoperability (Not Started)
- **Objective:** produce color-managed, metadata-preserving deliverables and measured interchange.
- **Why now / affected code:** Output conversion can only be trusted after color and graph state are stable.
- **Add:** audited JPEG/PNG/TIFF codec adapters (8/16/float where format permits), ICC embedding, EXIF/IPTC/XMP preservation, output sharpen/resize, orientation, batch export, soft proof. Evaluate TIFF/XMP round trips and PSD/PSB feasibility without promising editable adjustment interchange.
- **Research:** codec/metadata transitive licenses and malformed-input hardening; XMP/PSD limitations; DNG notice and SDK terms if implemented.
- **Tests/Adobe/performance:** decode/encode round trips, profile and metadata preservation, corrupt files, batch cancellation, Photoshop TIFF/reference export cases, 45 MP wall time and memory.
- **Complete when:** every distributed dependency is approved, exports are color tagged and round-trip validated, and the provisional 10 s goal is measured.

### Phase 7 — Delivery and measured optimization (Not Started)
- **Objective:** ship a reusable library package and optimize proven bottlenecks without reducing image quality.
- **Why now / affected code:** Package after public model and core behavior settle.
- **Add:** CMake install/export config, static/shared builds, symbol policy, optional Python wheels, compatibility/deprecation policy, Windows/Linux CI, later macOS CI. Optimize hot kernels and buffer reuse; select GPU API only if measured end-to-end benefit outweighs transfer/cache cost.
- **Research:** platform runtime/transitive licenses, CPU SIMD dispatch, GPU device coverage and fallback.
- **Tests/Adobe/performance:** consumer projects outside this repo, wheel/import tests, CPU/GPU equivalence if GPU exists, repeatability/determinism, benchmark regression gates.
- **Complete when:** independent C++/Python consumers build and run, license manifest is complete, quality gates pass, and latency/throughput gaps are published.

## Adobe methodology and release rule

Use [Adobe comparison plan](#adobe-compatibility-method-and-evidence-ledger) as a matrix, not a promise. ACR and Photoshop are separate targets. **Adobe applications are currently unavailable on this work machine.** Design the fixtures, capture instructions, and comparison tooling now; leave Adobe results **Not measured** until access is available. For each future reference record exact application/build and ACR process version, camera/profile, Photoshop working space and color settings, 8/16/32-bit depth, operation settings, output profile/intent/BPC, resize/sharpen, export format, and ICC embedding. Adobe documents that process versions change rendering and workflow options choose output space/depth ([process versions](https://helpx.adobe.com/uk/camera-raw/desktop/get-started/overview-and-setup/process-versions.html), [workflow options](https://helpx.adobe.com/camera-raw/desktop/get-started/overview-and-setup/camera-raw-settings.html)). Use max error, MAE/RMSE/PSNR, ΔE, hue/chroma/luminance, SSIM and edge/spatial or geometric metrics as appropriate. Record measured differences and whether native quality or a distinct compatibility mode is preferable. The reference corpus covers ramps, skin, dark/noisy/highlight cases, saturated/out-of-gamut color, fine repetitive detail, alpha, multiple camera/ISO/WB/ICC/bit-depth cases, with redistributable rights recorded. No operation is “Adobe compatible” until measured.

## Checkpoint 2 — decisions and open gates

**Decisions:** standalone core; decoded RAW boundary; explicit color domains and float headroom; color and Adobe harness before filters; graph schemas and serialization together; CPU-first scheduler; minimal early build; no GPL LittleCMS plugins.

**Open research gates:** permissive optional RAW decoder path for LibRawOps RAW-file support; complete transitive license audit for all library codecs/metadata/plugins/weights; final demosaic/denoise and patent status; exact ACR control behavior; Photoshop blend domain; GPU API only after Phase 3 profiling. These gates block the affected library capability or release, but do not block architecture and test work.

## Feature and maturity matrix

**Updated:** 2026-09-29. Status values: **Not Started, Research, Architecture, Prototype, In Progress, Functional, Validation, Adobe Compatibility Validation, Optimization, Complete**. “Complete” requires numerical/reference tests, representative inputs, error handling, serialization where relevant, performance evidence, license clearance, and documentation. A prototype or demo is not Complete. Phase numbers refer to [implementation plan](#librawops-implementation-plan).

The columns track actual implementation, not aspiration. “Deferred” in GPU means the logical operation must remain backend-neutral, but no GPU code is promised before profiling. “No” in Adobe Comparison means no reference measurement exists. Python “One-shot” means only the current native render function reaches the operation indirectly.

| Feature | Architecture | CPU | GPU | Python API | Serialization | Tests | Adobe Comparison | Status |
|---|---|---|---|---|---|---|---|---|
| Typed image/color descriptors | Phase 1 | Node/tile/materialized-output descriptors track camera, linear working, unmanaged bounded, display-linear and encoded sRGB preview; adapter output carries exact ICC profile SHA-256 | Deferred | Matrix and output-mode options; ICC adapter C++ only | None | Native descriptor and Python smoke tests | Phase 1 | In Progress |
| Signed float32 working pipeline / explicit clip | Phase 1 | RAW and tone preserve signed values; separate final clip | Deferred | One-shot | None | Native core and smoke | No | In Progress |
| ICC input/working/display/output conversion | Phase 1 | Matrix-only RAW working-space conversion, explicit ProPhoto↔Rec.2020 node, typed raster memory input and sRGB preview; injectable ICC display contract and optional pinned LittleCMS core backend for RGB output profiles, intent/BPC, exact-byte SHA-256 and hard clipping. No ICC raster import, soft proof, monitor discovery or arbitrary output conversion | Deferred | RAW matrix, raster working-space, output-mode options; ICC mode C++ only | None | W3C matrix/transfer, 347-patch signed/overrange round trip, mock adapter, real sRGB ICC transfer, malformed header, concurrent calls and tiled preview tests | Not measured | In Progress |
| Camera matrices / DCP / dual illuminant | Phase 1/3 | Caller-supplied white-balanced camera RGB→XYZ D50 matrix and linear ProPhoto/Rec.2020 conversion; DNG matrix interpretation, dual illuminants, and DCP tables absent | Deferred | Matrix/working-space options | None | ICC/W3C numeric oracles, neutral/headroom/ROI tests | Phase 1/3 | In Progress |
| Rendering intent / BPC / gamut mapping | Phase 1/6 | Optional LittleCMS ICC output adapter exposes intent and BPC; sRGB/ICC preview paths currently hard-clip out-of-gamut values. No soft proof or general perceptual gamut workflow | Deferred | Output mode only; ICC options C++ only | None | Out-of-gamut preview and real-profile ICC tests | Phase 1/6 | In Progress |
| Soft proof / gamut warning | Phase 6 | None | Deferred | None | None | None | Phase 6 | Not Started |
| RAW decoder interface / typed metadata | Phase 1 | Owned uint16 sensor plus stride, phase, active area and site levels | Deferred | Same metadata options through one-shot render | None | Native core and Python smoke | No | In Progress |
| Scene-linear raster memory source | Phase 1 | Owned finite float32 RGB, declared ProPhoto/D50 or Rec.2020/D65, optional stride, shared immutable storage and tiled ROI | Deferred | One-shot float32 raster render | None | Native source/graph and Python smoke tests | ICC/file import | In Progress |
| Optional permissive RAW decoder module | Phase 0/3 gate | None | N/A | None | N/A | None | Decode baseline | Research |
| Active area / black-white / linearization | Phase 3 | Active-area bounds and site levels; no implicit clamp; no linearization | Deferred | Metadata options | None | Native core and Python smoke | No | Prototype |
| Bad pixels / optical-black handling | Phase 3 | None | Deferred | None | None | None | Phase 3 | Not Started |
| White balance / temperature / tint | Phase 3 | RGB gains only | Deferred | One-shot | None | Smoke only | No | Prototype |
| Highlight reconstruction | Phase 3 | None | Deferred | None | None | None | Phase 3 | Not Started |
| Bilinear demosaic baseline | Phase 0/3 | Existing | Deferred | One-shot | None | Small synthetic | No | Prototype |
| Production demosaic | Phase 3 | None | Deferred | None | None | None | Phase 3 | Research |
| RAW luminance/chroma denoise | Phase 3/4 | None | Deferred | None | None | None | Phase 3 | Research |
| Lens profile/distortion/CA/vignette | Phase 3/4 | None | Deferred | None | None | None | Phase 3 | Research |
| Exposure and baseline exposure | Phase 3/4 | 2^stops only | Deferred | One-shot | None | Smoke only | No | Prototype |
| Highlights / shadows / whites / blacks | Phase 4 | None | Deferred | None | None | None | Phase 4 | Not Started |
| Curves and levels, per-channel | Phase 4 | Basic unrelated tone node | Deferred | None | None | None | Phase 4 | Not Started |
| Saturation and vibrance | Phase 4 | None | Deferred | None | None | None | Phase 4 | Not Started |
| HSL / color mixer | Phase 4 | None | Deferred | None | None | None | Phase 4 | Not Started |
| Selective color | Phase 4 | None | Deferred | None | None | None | Phase 4 | Not Started |
| Color grading / color balance | Phase 4 | None | Deferred | None | None | None | Phase 4 | Not Started |
| 1D/3D LUTs and channel mixer | Phase 4 | None | Deferred | None | None | None | Phase 4 | Not Started |
| Grayscale / monochrome conversion | Phase 4 | None | Deferred | None | None | None | Phase 4 | Not Started |
| Clarity / texture / dehaze | Phase 4 | None | Deferred | None | None | None | Phase 4 | Not Started |
| Capture / creative / output sharpening | Phase 3/4/6 | None | Deferred | None | None | None | Phase 3/4/6 | Not Started |
| Crop / rotate / straighten | Phase 4 | None | Deferred | None | None | None | Phase 4 | Not Started |
| Perspective / geometric transforms | Phase 4 | None | Deferred | None | None | None | Phase 4 | Not Started |
| Resize / resampling | Phase 4/6 | None | Deferred | None | None | None | Phase 4/6 | Not Started |
| Gaussian / lens / depth blur | Phase 4/5 | None | Deferred | None | None | None | Phase 5 | Not Started |
| Healing / cloning / content removal | Phase 5 | None | Deferred | None | None | None | Phase 5 | Research |
| Red-eye correction | Phase 5 | None | Deferred | None | None | None | Phase 5 | Not Started |
| Background removal | Phase 5 | None | Deferred | None | None | None | Phase 5 | Research |
| Shared raster/brush masks | Phase 5 | None | Deferred | None | None | None | Phase 5 | Not Started |
| Linear/radial/parametric masks | Phase 5 | None | Deferred | None | None | None | Phase 5 | Not Started |
| Luminance/color/depth ranges | Phase 5 | None | Deferred | None | None | None | Phase 5 | Not Started |
| Semantic subject/sky/object masks | Phase 5, license gate | None | Deferred | None | None | None | Phase 5 | Research |
| Mask feather/density/invert/combine/refine | Phase 5 | None | Deferred | None | None | None | Phase 5 | Not Started |
| Local adjustments | Phase 5 | None | Deferred | None | None | None | Phase 5 | Not Started |
| Pixel/adjustment layers and groups | Phase 5 | None | Deferred | None | None | None | Phase 5 | Not Started |
| Blend modes / opacity / fill / clipping | Phase 5 | None | Deferred | None | None | None | Phase 5 | Research |
| Smart-transform equivalent | Phase 5 | None | Deferred | None | None | None | Phase 5 | Not Started |
| Versioned arbitrary edit graph | Phase 2 | Fixed chain only | Deferred | One-shot | None | Smoke only | No | Prototype |
| Graph/mask serialization and migrations | Phase 2/5 | None | N/A | None | None | None | No | Architecture |
| Presets / processing versions | Phase 2/5 | None | N/A | None | None | None | No | Architecture |
| Undo/redo / snapshots / comparisons | Phase 2/5 | None | N/A | None | None | None | No | Not Started |
| Halo / transformed ROI / seam-free tiles | Phase 2 | Demosaic implicit halo only | Deferred | ROI only | N/A | Small ROI | No | Prototype |
| Cache / invalidation / memory budgets | Phase 2 | None | Deferred | None | N/A | None | No | Not Started |
| Mipmaps / fit preview / progressive render | Phase 2/3 | None | Deferred | None | N/A | None | No | Not Started |
| Cancellation / priority / concurrent ROIs | Phase 2 | None | Deferred | None | N/A | None | No | Not Started |
| Structured errors and corrupt-input handling | Phase 2/6 | Basic exceptions | Deferred | ValueError only | N/A | One bounds case | No | Prototype |
| JPEG import/export | Read Phase 3, write Phase 6 | None | N/A | None | N/A | None | Phase 3/6 | Research |
| PNG import/export | Read Phase 3, write Phase 6 | None | N/A | None | N/A | None | Phase 3/6 | Research |
| TIFF 8/16/float import/export | Read Phase 3, write Phase 6 | None | N/A | None | N/A | None | Phase 3/6 | Research |
| ICC embed / EXIF/IPTC/XMP preserve | Phase 6 | None | N/A | None | N/A | None | Phase 6 | Research |
| TIFF/XMP Photoshop round-trip | Phase 6 | None | N/A | None | N/A | None | Phase 6 | Not Started |
| PSD/PSB feasibility | Phase 6 gate | None | N/A | None | N/A | None | Phase 6 | Research |
| Batch rendering / thumbnails | Phase 2/6 | None | Deferred | None | N/A | None | Phase 6 | Not Started |
| C++ standalone consumer API | Phase 2/7 | Prototype exported classes | N/A | N/A | N/A | No C++ consumer test | N/A | Prototype |
| Persistent Python/NumPy/async API | Phase 2/7 | One-shot extension | N/A | One-shot | None | Smoke only | N/A | Prototype |
| GPU backend/fallback/equivalence | Phase 7, measured gate | N/A | None | N/A | N/A | None | No | Research |
| Numerical/image/RAW/color regression harness | Phase 0+ | Small synthetic core cases | N/A | N/A | N/A | Native core and smoke | No | In Progress |
| Adobe reference harness | Phase 1+ | None | N/A | N/A | N/A | None | No references | Research |
| Repeatable performance/memory benchmarks | Phase 0+ | Native ROI/full-stream median timing; memory pending | N/A | N/A | N/A | Manual Release run | N/A | In Progress |
| LibRawOps license/SBOM gate | Phase 0/7 | None | N/A | N/A | N/A | None | N/A | Research |

### Priority rule

When image quality/correctness, architecture, Adobe similarity, and speed conflict: prioritize them in that order unless an architecture decision record gives a measured exception. A compatibility mode may deliberately use a display-referred or encoded domain, but its use must be explicit in the operation schema and reference matrix.

## Dependency and license decisions

**Status:** Research. **Updated:** 2026-09-29. Policy applies to every distributed LibRawOps component, including optional modules, dynamic libraries, plugins, model weights, bundled datasets, profiles, and LUTs. No GPL, AGPL, LGPL, MPL, EPL, CDDL, or similar copyleft components without the owner's explicit separate approval. No linking-exception assumption. An unknown license is rejected until verified. Do not derive implementation code from copyleft projects.

The table is a research ledger, not legal advice or an automatic approval. “Proprietary Safe” means compatible with the stated no-copyleft policy **after** exact-version, build-configuration, transitive-license, binary-content, notice, and patent review. A candidate is not shipped until its Decision changes to Approved with a recorded version and evidence. Maintain an SBOM and distribute required notices.

| Dependency | Purpose | License | Transitive Licenses | Proprietary Safe | Obligations | Decision |
|---|---|---|---|---|---|---|
| LibRawOps original code | Engine | MIT, repository LICENSE | C++ runtime separately reviewed | Yes for own source | Include MIT notice | Approved |
| C++ standard library/runtime | Core execution | Platform/toolchain terms | Runtime components vary by toolchain | Under review per package | Review redistribution terms | Under Review |
| CPython C API/runtime | Optional Python module | Python Software Foundation terms | Python packaging/runtime | Under review per shipped distribution | Preserve Python notices | Under Review |
| LittleCMS **2.19.1 core only** | Optional ICC output transforms | MIT [exact tag license](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/LICENSE) and MIT headers/source | Static core uses C/OS runtime; system `libm` on Linux; Windows staged DLL imports only MSVC/Windows runtime. No JPEG/TIFF/ZLIB or plugin link in audited build | **Yes for audited Windows core-only configuration; other release platforms pending** | Pin `lcms2.19.1` commit `21c582a594fe5279f90c0b93437c398f93bf62b0`, source archive SHA-256 `267705e278e2f7c2fb886c259dadcbaeb2be52748bcbc71c79f08aacacb7a709`; ship installed MIT notice; keep plugins/tools/tests/codecs off; verify release artifacts and runtime terms | Approved — Optional Core-only Build |
| LittleCMS fast-float plugin | Optional acceleration | GPLv3 [project changelog](https://github.com/mm2/Little-CMS/blob/master/ChangeLog) | LittleCMS core | **No** | Prohibited by policy | Rejected — Copyleft |
| LittleCMS threaded plugin | Optional acceleration | GPLv3 [plugin header](https://github.com/mm2/Little-CMS/blob/master/plugins/threaded/include/lcms2_threaded.h) | LittleCMS core | **No** | Prohibited by policy | Rejected — Copyleft |
| pybind11 | Scalable optional Python binding | BSD-3-Clause [source](https://github.com/pybind/pybind11/blob/master/LICENSE) | Header/build dependencies to pin | Likely, not final | Include BSD notice | Candidate |
| libjpeg-turbo | JPEG raster import/export | IJG + BSD-3-Clause, with zlib/component terms [source](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/main/LICENSE.md) | Exact API/build subset to audit | Likely, not final | IJG attribution; BSD text where applicable; source-change notices | Candidate |
| libpng | PNG raster import/export | PNG Reference Library License v2 [source](https://github.com/pnggroup/libpng/blob/libpng18/LICENSE.md) | zlib and exact build to audit | Likely, not final | Preserve notices; mark altered source | Candidate |
| libtiff | TIFF import/export | libtiff license [source](https://gitlab.com/libtiff/libtiff/-/blob/master/LICENSE.md) | Optional compression codecs vary | Unknown until exact build audit | Preserve notices and audit each enabled codec | Under Review |
| Adobe XMP Toolkit SDK | XMP parsing/serialization | BSD-3-Clause [source](https://github.com/adobe/XMP-Toolkit-SDK/blob/main/LICENSE) | XMPCore/XMPFiles and build extras to audit | Unknown until exact build audit | Include BSD notice; check XMP specification patent terms | Candidate |
| OpenColorIO | Optional complex color/LUT workflow | BSD-3-Clause [source](https://github.com/AcademySoftwareFoundation/OpenColorIO) | Large dependency tree; optional components | Unknown | Pin subset, notices, transitive audit | Under Review |
| OpenImageIO | Possible raster I/O alternative | Apache-2.0/BSD-3-Clause [source](https://openimageio.readthedocs.io/en/stable/copyr.html) | Several required/optional codecs and color libraries [build list](https://github.com/AcademySoftwareFoundation/OpenImageIO/blob/main/INSTALL.md) | Unknown for selected build | Large transitive audit | Rejected — Technical |
| LibRaw | Broad camera RAW decoder | LGPL-2.1 or CDDL-1.0 [source](https://github.com/LibRaw/LibRaw) | Optional decode packs/codecs | **No** | Prohibited by policy | Rejected — Copyleft |
| RawSpeed | Camera RAW decoder | LGPL-2.1 [source](https://github.com/darktable-org/rawspeed) | Exact build to audit | **No** | Prohibited by policy | Rejected — Copyleft |
| rawler / dnglab | Camera RAW decoder/converter | LGPL-2.1 [project](https://github.com/dnglab/dnglab) | Rust crates need audit | **No** | Prohibited by policy | Rejected — Copyleft |
| rawloader | Camera RAW decoder | LGPL-2.1 [project](https://github.com/pedrocr/rawloader) | Rust crates need audit | **No** | Prohibited by policy | Rejected — Copyleft |
| Adobe DNG SDK | DNG decode/encode candidate | Adobe custom SDK agreement; not an approved permissive license [Adobe resource](https://helpx.adobe.com/camera-raw/desktop/dng-and-file-formats/digital-negative.html) | Bundled codecs/XMP require audit | Unknown | Separate legal and exact archive review | Rejected — License Unclear |
| Exiv2 | EXIF/IPTC/XMP candidate | GPLv2+ [source](https://github.com/Exiv2/exiv2/blob/main/README.md) | N/A for rejection | **No** | Prohibited by policy | Rejected — Copyleft |
| In-house limited RAW decoder | DNG or selected camera formats | LibRawOps MIT original code | Permissive codec components only | Potentially | Independent implementation; patent/format and test-corpus review | Candidate |
| Other permissive RAW decoder | Optional LibRawOps RAW-file module | **None verified in initial broad-camera scan** | Unknown | Unknown | Verify provenance, camera coverage, safety, FFI, dependency tree | Under Review |
| Semantic mask models, profiles, LUTs, corpus assets | Optional features and testing | Per artifact; often separate from code | Model/data dependencies vary | Unknown | No noncommercial or unverifiable redistribution | Rejected — License Unclear |

### LittleCMS 2.19.1 core-only audit — 2026-09-29

The [upstream release](https://github.com/mm2/Little-CMS/releases/tag/lcms2.19.1) identifies tag `lcms2.19.1` at commit `21c582a594fe5279f90c0b93437c398f93bf62b0`. CMake fetches its official tag archive with the SHA-256 in the ledger. The source root [LICENSE](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/LICENSE), compiled `src/` files and public `include/lcms2.h`/`include/lcms2_plugin.h` carry MIT terms. The upstream CMake project reports version `2.19` internally for this `2.19.1` hot-fix tag; use the tag, commit and archive hash as the build identity rather than that CMake version string.

The exact [upstream build options](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/cmake/Lcms2Options.cmake) are forced to static core ON, shared core OFF, fast-float/threaded plugins OFF, tools/tests OFF, JPEG/TIFF/ZLIB OFF and upstream thread support OFF. CMake fails if either prohibited plugin target unexpectedly exists. The [core source list](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/cmake/Lcms2Library.cmake) compiles only `src/` into `lcms2.lib`; `cmsplugin.c` implements the permissively licensed core plugin API and is distinct from the GPL plugins under `plugins/`. FetchContent caches the complete upstream source tree in the ignored build directory; never distribute that source cache as a product artifact, since it contains disabled GPL plugin source.

The tested Release Windows install contains `RawEngine.dll`, import library, `lcms2.lib`, public headers (including the MIT `lcms2_plugin.h`), package metadata, and both MIT notices. It contains no fast-float/threaded plugin DLL, library or source. `dumpbin /DEPENDENTS` on `RawEngine.dll` listed only Windows and MSVC runtime libraries; the core is statically linked. This approval is scoped to that optional configuration. Before shipment: audit the actual distribution bundle/SBOM, toolchain runtime terms and every target platform; add malformed ICC fuzzing, real monitor/output profile corpus and performance measurements. A build-tree cache is not an approved distribution.

### RAW decoder release gate

The core API accepts decoded sensor planes plus validated metadata. Before distributing any optional LibRawOps RAW-file module, choose and validate one of:

1. A verified permissive decoder with sufficient camera coverage and a clean C/C++ boundary; audit source, transitive crates/libs, fuzzing, metadata fidelity, and camera corpus.
2. An optional LibRawOps-owned decoder module built only from independently written or permissively licensed code; keep it behind the same decoder interface and review its exact dependencies.
3. In-house decoders for a limited declared camera/format set, beginning with the formats justified by real users. State unsupported cameras explicitly. Estimate engineering cost, test samples, proprietary compression variation, malformed-input security, and long-term camera updates before choosing.

**Verified:** LibRaw is LGPL/CDDL, RawSpeed and rawloader are LGPL, and dnglab/rawler is LGPL by their project license pages. **Inferred:** no broadly covering permissive decoder found in this initial survey; this is a search result, not proof none exists. **Unknown:** whether Adobe's custom DNG SDK terms and every bundled component satisfy the policy; reject until separate review. **Release criterion:** any distributed LibRawOps RAW-file module and its SBOM contain no prohibited or unknown dependency, and supported RAW formats pass quality, metadata, and corrupt-input tests.

### Other gates

- Audit each LittleCMS plugin separately; **never** enable fast-float or threaded plugins in a LibRawOps build. Test core-only performance before seeking a permissive optimization.
- Raster adapters choose exact versions and minimum build features; do not inherit optional GPL/codecs from a package manager by accident.
- For DNG file I/O, review Adobe's [specification patent license and required notice](https://helpx.adobe.com/camera-raw/desktop/dng-and-file-formats/digital-negative.html) separately from SDK copyright terms.
- Flag demosaic, denoise, healing/content-removal, and lens-profile patents for focused research before implementation. A paper or published specification is not a license to a software implementation or patented claim.
- Maintain a generated license/SBOM check for each LibRawOps distribution artifact, including optional Python and decoder modules. Review exact binary contents and transitive dependencies before release.

## Adobe compatibility: method and evidence ledger

**Status:** Research. **Updated:** 2026-09-29. “Equivalent-looking” is an empirical goal, not a claim of identical hidden Adobe algorithms. Keep **Adobe Camera Raw (ACR)** RAW development separate from **Photoshop (PS)** post-development editing. Use **Verified** only with a primary source or a reproducible controlled experiment, **Inferred** for a stated interpretation, and **Unknown** otherwise. Never infer compatibility from the name of a slider.

Adobe confirms that [process version changes ACR rendering](https://helpx.adobe.com/uk/camera-raw/desktop/get-started/overview-and-setup/process-versions.html), and that [ACR workflow options specify output color space, bit depth, dimensions and sharpening](https://helpx.adobe.com/camera-raw/desktop/get-started/overview-and-setup/camera-raw-settings.html). [ACR output spaces](https://helpx.adobe.com/x-productkb/multi/camera-raw-color-spaces.html) and [DNG camera metadata](https://helpx.adobe.com/camera-raw/desktop/dng-and-file-formats/digital-negative.html) are documented. Exact internal tone and detail algorithms generally remain **Unknown** until controlled characterization.

**Working-space evidence:** **Verified for DNG profile processing:** Adobe's [DNG 1.7.1 specification, ProfileHueSatMapEncoding and Chapter 6](https://helpx.adobe.com/content/dam/help/en/camera-raw/digital-negative/jcr_content/root/content/flex/items/position/position-par/download_section_733958301/download-1/DNG_Spec_1_7_1_0.pdf) converts XYZ D50 to linear RGB with ProPhoto primaries before applying a profile hue/saturation map, then converts back to XYZ D50. It also describes the profile look-table path in linear ProPhoto; optional sRGB encoding affects the table's value coordinate, not the underlying primaries. **Inferred (strong, consistent community evidence) for ACR/Lightroom generally:** an [Adobe Community Expert](https://community.adobe.com/questions-712/raw-files-opening-as-adobe-rgb-1988-and-only-8-bit-in-ps-camera-raw-1082104) and a [color-management contributor](https://community.adobe.com/questions-585/color-management-in-lightroom-4-308390) describe linear ProPhoto primaries as the internal processing space; an [enhanced-profile LUT discussion](https://community.adobe.com/t5/camera-raw-ideas/camera-raw-and-lightroom-document-the-format-of-rgb-luts-in-enhanced-profiles/idi-p/12221134) explicitly calls the same interpretation a surmise. These community posts are not Adobe product specifications. **Unknown:** whether every ACR hue-based slider operates in that space, and whether this holds across all process versions. Measure those controls independently.

### Reference capture contract

When an Adobe reference machine is available, generate Adobe reference images using the prepared fixtures. For **every** case save: source file and checksum; legal redistribution/source; camera make/model/ISO and RAW format; Photoshop and ACR exact versions/builds; ACR process version; camera/profile name and version; white balance mode/temp/tint; baseline/default and every changed slider; whether automatic adjustments are off; ACR workflow space, bit depth, dimensions and output sharpening; Photoshop color settings, working space, profile assignment/conversion policy, 8/16/32-bit mode, blend-space settings, and document ICC; adjustment/layer/mask settings; export format, compression, intent/BPC, profile embedding, orientation, resize and output sharpening. Save the edit recipe/XMP where available and a lossless 16-bit or float reference whenever possible. Pin monitor profile for screenshot-only comparisons, but prefer pixel exports over screenshots.

Use one-operation-at-a-time sweeps including zero, positive/negative, and extreme values, followed by selected interactions and order changes. Export an unedited Adobe baseline for each source/profile so differences in decode/profile are separated from the control under test. Keep ACR and PS comparison pipelines independent. Characterize undocumented behavior with ramps and controlled images; record the experiment and result rather than promoting an inference to “Verified.”

### Corpus and metrics

Corpus groups: gray and color ramps; saturated/out-of-gamut patches; skin; near-black and clipped/extreme highlights; fine/repeating detail, moiré and edges; multiple noise levels/ISOs; transparent RGB and alpha gradients; 8/16/float raster; multiple camera makers/models/WBs; embedded sRGB, Adobe RGB, Display P3, ProPhoto and unusual ICC profiles. Use original or legally redistributable/CC0 files with license and checksum recorded; do not assume a downloaded RAW or Adobe profile can be redistributed.

For aligned color operations report max channel error, MAE, RMSE, PSNR, ΔE (with named formula/illuminant), luminance/chroma/hue error, histogram and outlier maps. For spatial/detail operations report SSIM plus edge, texture, false-color and noise statistics. Geometry needs landmark/reprojection and coverage error rather than only pixel MAE. Alpha/compositing needs premultiplied/straight edge cases and transparent-color tests. Publish reference images, diff heatmaps, metrics, source/build/configuration, and a qualitative note. Thresholds are established **per operation and corpus** after a baseline; no universal “compatible” threshold is assumed.

### Matrix

“Measured Difference” stays **Not measured** until a pinned reference run exists. Status uses the shared vocabulary in [implementation plan](#librawops-implementation-plan).

| Operation | Adobe Reference (ACR / PS adjustment / PS blend mode / PS transform / PS export) | Known Behavior | LibRawOps Behavior | Metric | Measured Difference | Status |
|---|---|---|---|---|---|---|
| RAW baseline/profile | ACR default render, fixed process/profile | **Verified:** profile and process version affect rendering; exact internals **Unknown** | No camera profile/color transform | ΔE, ramps, shadows, highlights | Not measured | Research |
| DNG profile hue/saturation and look tables | ACR RAW with pinned DNG profile | **Verified for DNG profile processing:** linear ProPhoto primaries, with tag-dependent table-value encoding; exact ACR implementation **Unknown** | Absent | hue/chroma/ΔE on color patches and ramps | Not measured | Research |
| RAW decode/active area | ACR unedited RAW | **Verified:** DNG defines metadata; proprietary formats vary | Four Bayer phases, no active-area metadata | pixel geometry, metadata fidelity | Not measured | Prototype |
| White balance / temp-tint | ACR Basic WB sweep | Exact temperature/tint conversion **Unknown** | RGB multipliers only | neutral ΔE, hue/chroma | Not measured | Prototype |
| Exposure / baseline exposure | ACR Basic Exposure | Slider transfer and baseline interaction **Unknown** | Linear 2^stops, no baseline exposure | ramps, RMSE, highlight detail | Not measured | Prototype |
| Highlights/shadows/whites/blacks | ACR Basic sweeps | Spatial/tone interactions **Unknown** | Absent | luminance curves, local contrast | Not measured | Not Started |
| ACR tone curve | ACR parametric/point curves | Exact interpolation **Unknown** | One arbitrary shoulder/gamma curve | ramp and channel error | Not measured | Prototype |
| Camera color mixer/HSL | ACR Color Mixer/B&W | Process-version-sensitive behavior **Verified** in Adobe process docs; formulas **Unknown** | Absent | hue/chroma/ΔE by patch | Not measured | Not Started |
| Color grading/vibrance | ACR Color Grading/Basic | Exact transfer **Unknown** | Absent | ramps, skin, saturated patches | Not measured | Not Started |
| Texture/clarity/dehaze | ACR Effects/Basic | Exact multiscale methods **Unknown** | Absent | spatial contrast, halos, SSIM | Not measured | Not Started |
| RAW sharpening/denoise | ACR Detail | Exact algorithms **Unknown** | Absent | MTF, noise/edge, false detail | Not measured | Not Started |
| Lens/CA/vignette | ACR Optics/Geometry | Profile/model behavior **Unknown** | Absent | landmarks, color fringes, falloff | Not measured | Not Started |
| Curves and levels | PS adjustments | Tool behavior partly documented; interpolation/bit-depth details **Unknown** | Absent | ramps, channel error | Not measured | Not Started |
| HSL/selective color/color balance | PS adjustments | Exact color-domain choices **Unknown** | Absent | patch ΔE/hue/chroma | Not measured | Not Started |
| Channel mixer / grayscale | PS adjustments | Parameter semantics to verify | Absent | channel matrices, ramps | Not measured | Not Started |
| Blend modes, opacity, fill | PS blend mode | [W3C compositing](https://www.w3.org/TR/compositing-1/) is a baseline, not proof of PS behavior | Absent | alpha/color ramps, edge error | Not measured | Research |
| Masks/feathering | PS layer masks | Exact feather/selection behavior **Unknown** | Absent | coverage, edge profile | Not measured | Not Started |
| Blur/sharpen/noise filters | PS filters | Exact kernels and edge rules **Unknown** | Absent | PSNR/SSIM/edge profile | Not measured | Not Started |
| Crop/rotate/perspective/resize | PS transform | Interpolation/bounds details **Unknown** | Absent | landmark and edge error | Not measured | Not Started |
| Healing/clone/content removal | PS tools | Stochastic/content-aware behavior **Unknown** | Absent | region quality and masked spatial metrics | Not measured | Not Started |
| TIFF/JPEG/PNG export | PS export | ICC/depth/resize options documented, exact encoding varies | Absent | decoded pixels, tags, ICC, geometry | Not measured | Not Started |

### Working rule and phase timing

Design the harness in **Phase 1 with color semantics**, not at final optimization. Generate deterministic fixtures and record the Adobe capture recipe now; run comparisons when the reference applications are available. Each later phase adds testable cases before its feature reaches Functional, while the Adobe comparison column remains **Not measured** until actual captures exist. If Adobe's behavior conflicts with the native scene-linear model, document a distinct compatibility mode and its color/alpha domain. Keep two outcomes visible: native image-quality judgment and measured Adobe distance. Where exact reproduction is impractical, state the difference, cause, magnitude, and whether more work is justified.

## Architecture decision records

### ADR 0001 — Standalone library boundary

**Status:** Accepted for architecture. **Date:** 2026-09-29.

**Context:** LibRawOps must be reusable by any C++ program and optionally by Python. Application workflows cannot define engine APIs.

**Options:** (A) engine embedded in one application's UI; (B) standalone core with optional adapters/bindings.

**Choice:** B. Core algorithms, image/edit state, masks, history, serialization, metadata model, and rendering have no Qt, Python, or application assumptions. UI, interaction, and layout belong to consumers. Build/install packaging matures after the public model is validated.

**Consequences:** Consumer adapters are separate packages/tests. The C++ core builds and tests without Python or a decoder. No consumer receives a special operation path in the core.

### ADR 0002 — Explicit color domains and precision

**Status:** Accepted architecture; linear ProPhoto/D50 selected as the current canonical editing default, subject to versioned revision if future evidence requires it. **Date:** 2026-09-29.

**Context:** Professional RAW and color-managed editing need sensor, camera, scene, display, output, and alpha states to remain distinct. Adobe's [DNG 1.7.1 specification](https://helpx.adobe.com/content/dam/help/en/camera-raw/digital-negative/jcr_content/root/content/flex/items/position/position-par/download_section_733958301/download-1/DNG_Spec_1_7_1_0.pdf) **verifies** linear ProPhoto primaries for DNG profile hue/saturation and look tables. The broader ACR/Lightroom linear-ProPhoto working-space claim is **Inferred (strong, consistent community evidence)**, and the domains of individual ACR hue sliders are **Unknown**.

**Options:** First, reject implicit float RGB in favor of typed descriptors, named transforms, and explicit clipping. For the canonical scene-linear editing space: **(A) linear ProPhoto/D50** minimizes conversions around DNG profile maps and may simplify measured ACR matching, but its very wide gamut includes nonphysical chromaticities ([ICC presentation](https://www.color.org/groups/hdr/HDRWG-Summer2020.pdf)) and requires careful handling of extreme/negative values and output mapping. **(B) linear Rec.2020/D65** uses physically realizable, monochromatic primaries ([ITU report](https://www.itu.int/dms_pub/itu-r/opb/rep/r-rep-bt.2246-2-2012-pdf-e.pdf)) and would retain the older prototype default; DNG profile maps and any ACR-style hue control shown by measurement to need ProPhoto use an explicit linear-ProPhoto domain, adding conversions and potential runtime/cache cost. Neither option establishes Adobe equivalence by itself.

**Choice:** **A, linear ProPhoto/D50**, is the canonical default for current editing buffers and newly constructed camera-color transforms. DNG profile operations already require this domain; choosing it avoids extra conversions around each such operation. The [347-patch round-trip and streaming benchmark](#working-space-conversion-evidence-2026-09-29) found no signed/headroom loss beyond 1e-5 and estimated roughly 50–58 ms for one 45 MP conversion pass on the work machine. That cost alone does not establish a user-visible performance difference, but it is unnecessary in the DNG path when ProPhoto is canonical. Rec.2020/D65 stays fully supported as an **explicit** input/working domain with typed conversion. No Adobe compatibility is claimed. Sensor codes stay uint16 until calibration; masks use float32 coverage; compositing uses premultiplied float32 RGBA. Metadata specifies profile/primaries, white point, transfer, channel order, precision, scene/display reference and alpha. Values below 0 and above 1 survive until an explicit output operation. Use float64 for selected matrix/profile calculations.

**Consequences:** Operations declare domains and conversions. Current tone output cannot be mislabeled linear. The C++ `RasterMetadata` and `CameraColorTransform` defaults, and Python RAW color-transform default, changed from Rec.2020 to ProPhoto in this **unversioned prototype**; explicit Rec.2020 callers retain their behavior. No edit serialization exists yet. Phase 2 schema must store both an explicit working-space identifier and a processing version; loading older implicit recipes must select/migrate their original behavior, never reinterpret them through a new default. Validate actual DNG maps, output profiles, perceptual quality and Adobe hue controls before any compatibility claim. If those data favor another canonical space, introduce a new processing version and migration. Adobe-specific encoded blend domains remain explicit conversions.

**Implementation note (2026-09-29):** The graph accepts an optional caller-calibrated white-balanced camera RGB→XYZ D50 matrix and converts it to either linear working space without clipping. The default target is now ProPhoto/D50. `WorkingSpaceConvertNode` converts explicitly between ProPhoto/D50 and Rec.2020/D65, preserving signed/over-range float32 values. An optional preview path converts scene-linear RGB to linear sRGB, applies the prototype tone curve, then hard-clips and sRGB-encodes. The graph does not interpret a raw DNG ForwardMatrix, apply a DNG profile map, discover monitor ICC data, or perform arbitrary output-profile conversion. ICC ROMM and W3C numeric reference cases cover the matrices and sRGB transfer; Adobe comparison remains not measured.

**ICC adapter and optional backend (2026-09-29):** A C++ host may inject a thread-safe `IccDisplayTransform` at graph construction for `IccDisplay` mode. The contract receives finite display-linear sRGB tiles, returns finite bounded profile-encoded RGB, and supplies the SHA-256 digest of the exact output profile bytes. The optional LittleCMS 2.19.1 core-only backend now creates a linear-sRGB source profile, parses RGB display/output ICC bytes, applies the selected intent and BPC flag, serializes calls to its transform, and hard-clips encoded output. A real generated sRGB profile tests numeric transfer, profile identity, concurrent calls and tiled equivalence. It does not discover monitor profiles, import arbitrary raster ICC, soft proof, provide gamut warnings, or establish ACR behavior. The profile digest identifies the output profile bytes only; future cache keys must also include intent/BPC, transform implementation/version, source profile and relevant recipe settings. Add independent real-profile round-trip, malformed-profile fuzz and monitor-profile cases before release.

### ADR 0003 — RAW decoding boundary and optional module gate

**Status:** Accepted boundary; decoder selection in Research. **Date:** 2026-09-29.

**Context:** The core must stay decoder-free, while every distributed LibRawOps component must comply with the no-copyleft policy. Current broad decoders found in the initial survey are LGPL/CDDL: [LibRaw](https://github.com/LibRaw/LibRaw), [RawSpeed](https://github.com/darktable-org/rawspeed), [rawler](https://github.com/dnglab/dnglab), and [rawloader](https://github.com/pedrocr/rawloader).

**Options:** (A) external decoded samples only with no RAW-file plan; (B) decoder interface plus optional module release gate; (C) decoder code inside the core.

**Choice:** B. Core accepts a validated sensor plane and rich metadata. Research a truly permissive decoder, a separately built LibRawOps-owned permissive decoder module, and limited in-house camera/DNG decoders. Require actual camera coverage, metadata fidelity, corrupt-input robustness, and full transitive license evidence before selection.

**Consequences:** RAW editing architecture proceeds; a LibRawOps RAW-file module cannot ship through an unresolved or copyleft path. Broad in-house decoding is treated as expensive and is not promised. Unsupported cameras are reported explicitly.

### ADR 0004 — Versioned graph and edit format together

**Status:** Accepted for architecture. **Date:** 2026-09-29.

**Context:** A fixed GraphRecipe cannot support arbitrary order, duplicate operations, masks, layers, cached intermediates, undo, or old edits preserving appearance.

**Options:** (A) enlarge GraphRecipe and add persistence later; (B) versioned operation schemas and immutable DAG with serialization designed in the same phase.

**Choice:** B. Each instance has a stable type ID, schema/processing versions, UUID, enabled flag, typed parameters, named inputs and masks, and declared domains/ROI/halo. Save a versioned JSON graph manifest with separate binary mask/raster assets; pin processing versions and define migrations. History stores state revisions, not images.

**Consequences:** Every new operation registers once and participates in render, cache, Python, presets, serialization, migration, and tests. Disk persistence may arrive after the schema design, but no feature can bypass the common model.

### ADR 0005 — Demand-driven tiles and bounded caches

**Status:** Accepted for architecture; sizing by benchmark. **Date:** 2026-09-29.

**Context:** Current tiles are serial output chunks; neighborhood and transformed operations have no general upstream request protocol. Late edits recompute early stages.

**Options:** (A) fixed same-rectangle pull for every node; (B) node-declared ROI mapping/halo plus scheduler and content-keyed caches.

**Choice:** B. Scheduler requests source tiles by viewport, mip, and quality; nodes declare coordinate mapping, halo, and edge policy. Cache keys include source, operation/schema/processing version, parameters, input hashes, color setup, tile, mip, and quality. Bounded LRU caches cover CPU images, masks, and later GPU resources. Cancellation and supersession prioritize current viewport requests.

**Consequences:** Tiled and untiled outputs must match; late edits reuse upstream data. Cache budgets, tile size, and task pool strategy are selected from measurements on 24–60 MP files, not fixed by the prototype's OpenMP loops.

### ADR 0006 — Wrapped ICC engine, core-only license

**Status:** Accepted for optional LittleCMS 2.19.1 core-only Windows build; platform/release audits remain. **Date:** 2026-09-29.

**Context:** Correct ICC v2/v4 parsing and transforms are foundational; writing a complete ICC engine in-house is unjustified. The [LittleCMS 2.19.1 core license](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/LICENSE) is MIT, but its fast-float and threaded plugin implementations include GPLv3 code and are prohibited. See the exact [dependency audit](#littlecms-2191-core-only-audit--2026-09-29).

**Options:** (A) custom ICC engine; (B) wrapped, audited LittleCMS core; (C) bundle LittleCMS with plugins.

**Choice:** B. Build the pinned, SHA-256-checked 2.19.1 static core only behind `RAWENGINE_WITH_LCMS=ON`, default OFF. Disable and exclude GPL plugins. Keep the `IccDisplayTransform` interface replaceable.

**Consequences:** Windows staged install and DLL dependency inspection found no GPL plugin artifact. Core-only ICC speed, malformed-profile hardening, broader embedded/monitor/output profile tests, platform audits and distribution notices remain release gates. A GPL plugin is not a permitted shortcut.

### ADR 0007 — CPU-first execution with GPU-ready semantics

**Status:** Accepted. **Date:** 2026-09-29.

**Context:** No measured bottleneck currently justifies a specific GPU API. Target platforms begin with Windows x64, with Linux and macOS later.

**Options:** (A) choose GPU API now; (B) CPU reference backend and profile first; (C) permanently CPU-only.

**Choice:** B. Logical operations declare domain, ROI/halo, precision and determinism independent of backend. CPU float32 is the reference. Benchmark thread scheduling, SIMD, allocation and cache before selecting a GPU backend. Revisit after production RAW Phase 3.

**Consequences:** No early shaders or GPU dependency. A later backend must match documented tolerances, respect cache budgets, and fall back to CPU on unsupported devices or failure.

### ADR 0008 — Prepare Adobe comparison in color Phase 1

**Status:** Accepted. **Date:** 2026-09-29.

**Context:** Building controls before controlled references risks baking incompatible color and tone semantics into the engine. Adobe [process versions](https://helpx.adobe.com/uk/camera-raw/desktop/get-started/overview-and-setup/process-versions.html) and [workflow options](https://helpx.adobe.com/camera-raw/desktop/get-started/overview-and-setup/camera-raw-settings.html) alter rendered results.

**Options:** (A) compatibility validation at the end; (B) reference harness in the color phase, expanded with each feature.

**Choice:** B. Design fixtures, capture instructions, and comparison tooling in Phase 1. Run Adobe comparisons when a reference machine is available; they do not block foundational library work. Keep ACR RAW and Photoshop raster targets separate, record exact versions/profiles/depth/export settings, and use per-operation metrics and hard-case corpus. Classify claims Verified, Inferred, or Unknown.

**Consequences:** DNG specification and independent numeric tests guide early color decisions; actual Adobe differences remain **Not measured** until reference captures exist. If later comparisons expose a mismatch, revise the processing version or add an explicit compatibility mode before claiming Adobe equivalence.

### ADR 0009 — Minimal build first, package after API stabilizes

**Status:** Accepted. **Date:** 2026-09-29.

**Context:** The prototype already builds a shared DLL, but exports STL types without an ABI policy. Full cross-platform packaging before the image/graph model settles would add churn for a solo developer.

**Options:** (A) build all packages and freeze ABI immediately; (B) minimal builds/tests/benchmarks first, then independent-consumer packaging; (C) no reusable packaging.

**Choice:** B. Maintain a working C++20 build and optional Python extension throughout. Mark API pre-1.0. After architecture validation, add CMake package config/install targets, static/shared variants, symbol visibility, semantic-version policy, wheels, notices/SBOM and Windows/Linux CI, then macOS. Add a C ABI only for a demonstrated non-C++ consumer need.

**Consequences:** Early effort goes to correctness and evidence. The release gate still requires an installable library that independent C++ and Python consumer projects can use.

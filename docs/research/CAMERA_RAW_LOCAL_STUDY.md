# First local camera RAW study — 2026-10-01

Eight user-supplied Nikon Z7 II NEF files are sufficient for the first actual-camera check of the opt-in Menon base v1 engine. All unpack as 8288×5520 uint16 RGGB Bayer sensors (45,749,760 photosites). The study uses original sensor coordinates and decoder-visible bounds; the decoder's recommended 8256×5504 crop is recorded separately rather than silently applied. Originals, decoded planes, embedded camera thumbnails, rendered views and local JSON reports remain ignored under `tests/rawfiles/`. Nothing grants redistribution of these photographs.

| Original | ISO | Inspected scene coverage |
|---|---:|---|
| `_DSC0636.NEF` | 250 | Building edges, repeating window/grille detail, foliage, bright sky and shadows |
| `_DSC0727.NEF` | 100 | Colored graffiti, shutter ridges and chromatic edges |
| `_DSC0749.NEF` | 100 | Skin, fine hair, glasses/reflections and fabric |
| `_DSC0800.NEF` | 100 | Foliage, reeds, reflected detail, architecture and sky |
| `_DSC0832.NEF` | 100 | Foliage, subject/background transitions and deep shadows |
| `_DSC0843.NEF` | 1250 | Indoor food, texture, glossy highlights and dark areas |
| `_DSC1595.NEF` | 100 | Bright cloud structure and blue sky |
| `_DSC1755.NEF` | 1250 | Night buildings, fine lattice, colored lights and noisy shadows |

Scene descriptions come from inspected embedded previews and native crops. EXIF light-source fields are unspecified; indoor/night scenes are not certified tungsten conditions. Embedded JPEGs are appearance references, not analytical RGB truth. These are local evaluation assets, not an admitted complete three-camera/12-condition redistribution corpus. ISO 1250 lies between that corpus's low (≤400) and high (≥1600) bands.

## Extraction and view contract

[Optional extraction adapter](../../tools/raw_camera_extract.py) uses an existing external research runtime, **rawpy 0.26.1 / LibRaw 0.22.0**, NumPy and Pillow. It calls open/unpack only, copies complete little-endian uint16 sensor planes and records original/decoded hashes, sensor sizes, active bounds, CFA channel indices, per-site levels, as-shot WB and selected EXIF fields. No postprocess, demosaic, WB, scaling or user corrections occur during extraction; decoder-internal decompression/linearization is inherent to unpacking. This initial container adapter supports Nikon NEF only and rejects non-RGB/non-2×2 sensor layouts or unrepresentable site levels. Decoder shipment remains unapproved; no decoder, scientific dependency or RAW-file interface was added to the engine or install.

Decoder channel order `RGBG` maps RGGB photosites through indices `[0,1,3,2]`; levels are mapped by site, never mistaken for RGB order. These files expose black 1008, decoder maximum 16383 and per-channel camera saturation 15311. The adapter deliberately selects the declared per-channel saturation values, preserves the differing maximum in provenance and allows signed/headroom values. See [rawpy's sensor API](https://letmaik.github.io/rawpy/api/rawpy.RawPy.html).

[Optional engine checker](../../tools/raw_camera_engine_check.py) binds exactly the same owned samples/metadata to immutable bilinear v1 and Menon base v1 sessions with cache disabled and one worker. Source-only float32 buffers retain signed/headroom values. It also exercises as-shot WB, zero exposure adjustment, camera-to-ProPhoto/D50 and the existing encoded sRGB preview. There is no denoise or sharpening.

The view matrix is explicitly diagnostic: the decoder's XYZ-to-camera basis is multiplied by the standard linear-sRGB-to-XYZ D65 basis, normalized by its response to a D65 neutral, inverted and mapped to XYZ D50. The [LibRaw 0.22.0 matrix calculation](https://github.com/LibRaw/LibRaw/blob/0.22.0/src/utils/utils_dcraw.cpp#L266) establishes the basis direction and neutral normalization; the adapter uses standard matrix algebra rather than copying decoder implementation code. This is not a measured camera profile, dual-illuminant interpolation, calibrated color-accuracy claim or camera-JPEG/Adobe match. Identical WB/matrix/exposure/output settings are used for both algorithms. Clipping occurs only in display/output views; source-only checks use unclipped floats.

## Results and disposition

The initial study checked four 256×256 native sensor ROIs per file (one includes the actual top/left active border). A second focused study checked four additional ROIs each on the portrait and night image, covering hair/skin/glasses and building/lattice/shadow detail. All **80 algorithm/ROI checks** passed:

- **5,242,880 observed channel values** match independent float32 site normalization byte for byte.
- Tile-256 and tile-37 outputs are byte-identical; scheduled tile-29 ROI outputs match the same pixels.
- Source footprints match the clipped radius-one bilinear or radius-six Menon halo.
- Calibrated mip-2 ROI pixels match an independent float64 average of their 4×4 native calibrated float32 pixels, with **maximum absolute difference 0** across every checked ROI.

Both algorithms also rendered full 2072×1380 mip-2 frames for all eight originals. These are not full native-resolution renders or a whole-frame pixel-parity proof. Display orientation is applied afterward to PNGs only. On this home's **Intel Core i9-14900K**, one render per original yielded median full calibrated mip-2 call time **0.368 s bilinear / 5.090 s Menon** (Menon range 4.995–5.186 s). Those calls exclude decoding, session/source copying, PNG encoding, display and export. Background/thermal conditions are uncontrolled; this is initial latency evidence, not a formal benchmark or fit-to-screen target signoff. Process peak memory and allocations remain unmeasured.

All eight initial crop boards and both focused boards were visually inspected. Menon retains visibly more fine hair/fabric, foliage and reed detail than bilinear. It also exposes more fine chroma grain in ISO 1250 shadows. Bright repeating lattice/colored-light edges still show aliasing and color artifacts; clipped diagnostic views cannot establish highlight recovery or isolate reconstruction error from saturation/profile effects. No broad superiority or moiré-free claim follows. **Practical disposition: usable opt-in prototype for further camera evaluation; keep bilinear as the default and preserve every prior synthetic rejection.**

Local evidence (intentionally not committed):

| Path under `tests/rawfiles/_librawops_local/` | SHA-256 |
|---|---|
| `extraction-v1.json` | `e0f09102f1718aa1c5b1c97d7b98bf867975ac12c62ac171ab19e1203ec43628` |
| `engine/engine-check-v1.json` | `298bb5d0b4c2902d81bc11a9a9bd3621f7be40d459ea9e68110fcab0f0c644d4` |
| `focused/engine-check-v1.json` | `091e01c0a74a37210809e129d9593cdd54a275e4e5c1c9802781e5568a15a628` |

The reports bind extraction/input hashes, native extension hash, source metadata/fingerprints, reconstruction policies, source RGB hashes, ROI/view settings, preview discrepancies, latency and crop-board hashes. The initial contact sheet is `contact-sheet.png`; per-file side-by-side crop boards and encoded mip-2 PNGs are under `engine/`, with targeted portrait/night boards under `focused/`. The focused extraction record retains the selected sensor-coordinate ROIs. Reports are local run records; use a new output directory for a new study rather than treating their timing fields as reproducible frozen scientific fixtures.

## Verification and continuation

MSVC Release home build succeeded. Default CTest passed **26/26**, including native Menon and Python integration; core-only/LittleCMS configurations were not rebuilt in this camera step. Seven optional research-adapter tests passed, covering CFA/site-level mapping, nonzero active origins, scalar-white fallback, unsupported sensors/levels, matrix direction/D65 neutral, signed/headroom fidelity and reviewed-ROI bounds/mip anchors. No camera originals or decoder are needed for those seven tests; a NumPy-equipped research Python is required.

The pull exposed two Git line-ending hazards in byte-hashed research evidence. `.gitattributes` now pins the original policy's **CRLF** checkout bytes and the diagnostic report's **LF** bytes on every platform. Policy SHA-256 is restored to the frozen Menon provenance `1f90ea27e426b393d4951fa30fb3ec42c36775766e204573bcda6b7b0a0efd32`; diagnostic SHA-256 matches the preserved `956df8bbf16f82fb4046e1f50e566ae4f404deb09d48ff3db7b0bd34a4cb1c31`. Git blob content, policy values and archived evidence remain unchanged. The complete default suite passed after the fix.

The requested initial high-ISO additions have now been supplied and checked in the extension below. No additional photographs are required to continue. Optional matching camera JPEGs help appearance review; known-illuminant gray/color targets would support later profile work. Other cameras and the formal corpus remain later work.

Next profile the multi-second Menon calibrated preview, record process peak memory/allocations, and define its practical quality/performance target before optimization or default promotion. Better noise handling, clipped-highlight evaluation, actual camera calibration, broader coverage, decoder shipment and patent/jurisdiction release gates remain open.

Home reproduction (PowerShell, from the Git root):

```powershell
& 'C:\Users\tylle\OneDrive\Documents\Playground\.msi_build_venv\Scripts\python.exe' tools/raw_camera_extract.py tests/rawfiles tests/rawfiles/_librawops_local
& 'C:\Users\tylle\anaconda3\python.exe' tools/raw_camera_engine_check.py build-msvc-release tests/rawfiles tests/rawfiles/_librawops_local/extraction-v1.json tests/rawfiles/_librawops_local/engine-new
& 'C:\Users\tylle\anaconda3\python.exe' tests/raw_camera_adapter_tests.py
& 'C:\Program Files\Microsoft Visual Studio\2022\Preview\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build-msvc-release --output-on-failure
```

These are separate research/engine Python runtimes because the existing decoder uses Python 3.13 while the home native extension uses Python 3.9. No package was installed or consumer application modified.

## High-ISO extension — 2026-10-01

The user supplied three additional untouched Z7 II NEFs: `_DSC1788.NEF` and `_DSC1790.NEF` at **ISO 3200**, and `_DSC1793.NEF` at **ISO 20000**. All retain the same 8288×5520 RGGB layout and declared site levels. Their EXIF exposures are 1/30 second; illuminant fields remain unspecified. These group portraits provide skin, fabric/printed-shirt detail, dark backgrounds and substantial visible noise. They fill the initial high-ISO request; different framing/exposure/lighting means they are not a controlled matched-ISO sequence or a known-tungsten profile reference.

The adapter now accepts `--only` to select new local NEF filenames, avoiding changes to the original eight-file extraction/report evidence. Selection rejects missing/unsupported names, duplicates and paths outside the local input directory. All assets for this extension remain ignored under `tests/rawfiles/_librawops_local/high-iso/`.

Four native 256×256 sensor ROIs per new file passed for both algorithms: **24 additional algorithm/ROI checks**, **1,572,864 exact observed channels**, exact tile/async ROI output, clipped halo planning and **zero maximum error** against independently averaged native-calibrated mip-2 pixels. Both candidates rendered full mip-2 frames for all three files; embedded previews and all three native comparison boards were inspected. Combined with the original/focused study, coverage is now **11 originals, 104 algorithm/ROI checks and 6,815,744 exact observed channels**. This verifies local engine behavior, not analytical reconstruction error or formal camera-corpus admission.

The ISO 3200 crops confirm a visible tradeoff: Menon retains more skin/printed-fabric detail while showing stronger fine chroma grain. At ISO 20000, dark-background noise becomes visibly more spatially structured than the smoother bilinear result. Neither output has denoise, and these are diagnostic views with shared WB/color/output settings. This strengthens the need to evaluate noise handling before production/default promotion; it does not justify silently changing the demosaic algorithm or its immutable version. Keep Menon opt-in and bilinear as the default.

One full calibrated mip-2 call per file measured Menon **4.975 / 4.884 / 4.913 s**, versus bilinear **0.380 / 0.366 / 0.361 s** on the same home i9-14900K. The earlier timing exclusions and uncontrolled-run limits apply. Eight optional adapter tests passed after adding confined incremental selection. Engine/binaries/policy/archived research are unchanged; the prior default CTest **26/26** result remains the latest engine suite and was not repeated for this research-only selector addition.

Local evidence (new records, earlier records retained unchanged):

| Path under `tests/rawfiles/_librawops_local/high-iso/` | SHA-256 |
|---|---|
| `extraction-v1.json` | `2dc3db13ac8d1fa004b3a11d67a8a17707eba780110337a6dd5eb879a8c06da8` |
| `engine/engine-check-v1.json` | `87c93f7d8ddd2bab33336ebc1dc3abc99eba64e812f0471aacd20e7993d74395` |

The contact sheet is `contact-sheet.png`; native comparison boards are `engine/<capture>-native-crops.png`. Full encoded preview PNGs and source/view/check hashes are recorded in the engine report. Reproduce into a new directory:

```powershell
& 'C:\Users\tylle\OneDrive\Documents\Playground\.msi_build_venv\Scripts\python.exe' tools/raw_camera_extract.py tests/rawfiles tests/rawfiles/_librawops_local/high-iso-new --only _DSC1788.NEF _DSC1790.NEF _DSC1793.NEF
& 'C:\Users\tylle\anaconda3\python.exe' tools/raw_camera_engine_check.py build-msvc-release tests/rawfiles tests/rawfiles/_librawops_local/high-iso-new/extraction-v1.json tests/rawfiles/_librawops_local/high-iso-new/engine
```

The preview/memory checkpoint is completed below. Known-light/profile/corpus coverage, denoise, highlight recovery and release gates remain separate. No commit or push.

## Preview profiling and bounded block parallelism — 2026-10-01

The new [decoded-camera benchmark](../../bench/raw_session_benchmark.py) measures owned-session initialization, four native 1024×1024 pipeline stages, full calibrated mip-2 cold/warm/late-tone renders and a 256×256 reduced viewport with warm/tone/exposure/WB edits. Stage manifests follow upstream dependencies rather than exported operation-ID order. Every report binds the input/extraction, extension, **engine DLL**, Menon source and benchmark hashes, render/cache settings, CPU time and Windows process memory counters. Output reports require new paths. NumPy remains an optional local research dependency.

Before changing native code, the Release DLL/extension/source were copied into ignored `build-msvc-release/before-preview-opt/`. Fresh-process before/after measurements used `_DSC1793` (45,749,760 photosites, ISO 20000), the same as-shot WB/diagnostic matrix and encoded view recipe, one session worker, 64 MiB cache, output tile 256 and three repeats. Timed benchmark processes ran sequentially, without concurrent builds/tests. Two exploratory one-repeat reports overlapped and are **excluded** from the comparison. Power mode, background load and thermal state were not controlled; these local medians are not service-level guarantees. Initialization includes owned sample copying, validation and fingerprinting; render times include graph/cache/output-byte copying, excluding decoding, PNG/display/export.

The source-only native tile took 87.457 ms versus 131.894 ms for the complete native encoded chain. This identifies reconstruction as the main cost in this sample; stage differences include cache storage/copy and runtime effects, so they are not exclusive CPU samples. An unchanged full preview had **zero cache hits / 378 misses** per pass at this budget. Its many native intermediate tiles evict earlier entries, while the small viewport fits and reuses upstream work. Increasing the cache or changing eviction was outside this optimization.

[MenonDemosaic.cpp](../../MenonDemosaic.cpp) now runs independent 256-pixel blocks with at most **four OpenMP threads** when a request spans at least four blocks. It honors smaller runtime thread limits, avoids active nested teams and keeps small requests/core-only builds serial. Each block retains its original boundaries, halo, checked planes, float64 equations/order and disjoint output writes. Worker exceptions are retained and rethrown after the team joins. There is no algorithm/version, schema, source identity, default or numerical-policy change. The existing point/color loops are unchanged.

| Measurement, default OpenMP environment | Before | After |
|---|---:|---:|
| Owned session initialization, one sample | 293.965 ms | 290.797 ms |
| Source-only native 1024×1024, median | 87.457 ms | 34.178 ms |
| Native WB/exposure stage, median | 118.643 ms | 41.841 ms |
| Native calibration stage, median | 120.666 ms | 43.030 ms |
| Native encoded stage, median | 131.894 ms | 54.932 ms |
| Full mip-2 cold preview, median | **5.652 s** | **1.918 s** |
| Full unchanged warm preview, median | 5.487 s | 1.942 s |
| Full late-tone edit, median | 5.452 s | 1.932 s |
| Reduced 256×256 viewport, cold median | 117.617 ms | 40.948 ms |
| Same viewport, warm median | 0.337 ms | 0.283 ms |
| Viewport late-tone edit, median | 0.903 ms | 0.617 ms |
| Viewport exposure / WB edit medians | 8.594 / 11.060 ms | 7.285 / 10.711 ms |
| Process peak working set | 338.754 MiB | 358.781 MiB |
| Process peak private commit | 1010.633 MiB | 1030.578 MiB |

Cold full preview is **2.95× faster (66.1% less wall time)** in this comparison. With `OMP_NUM_THREADS=4`, `OMP_DYNAMIC=FALSE` applying to the whole process, its separate three-repeat median is **4.127 → 1.622 s**, and viewport cold/warm/late-tone medians are **30.293 / 0.246 / 0.858 ms** after the change. The engine does not set global OpenMP policy. Process-wide environment also affects scientific runtime threading; default versus four-thread private commit must not be attributed solely to the engine. Both configurations retain exactly the same pixels.

Memory counters cover the whole process, including Python/NumPy/thread runtimes, mapped sensor, owned source, cache, output and transient buffers. Peak counters are lifetime high-water marks, sampled after rendering; they do not isolate individual render peaks. The default measured working-set increase is **20.027 MiB**. The analytical scratch bound rises from **5,297,680 bytes per block** to at most **21,190,720 bytes (~20.21 MiB) per active parallel request**, plus output/source/cache/allocator overhead. Concurrent session requests multiply that bound; this is not a global memory budget. Each block still constructs eleven scratch arrays; a full mip-2 pass at this tile size visits 726 blocks (7,986 logical scratch-array constructions), but **no allocator call-count/stack trace was collected**. Allocation tracing, materialized native-final/export and wider thread/cache scaling remain open.

**Verification:** all **37 before/after render outputs per thread configuration (74 pairs total)** have identical float32 hashes/dimensions, including changed recipes. Warm results match cold references; late-tone cached/cold-reference parity passes. Repeated native goldens pass with OpenMP thread settings 1 and 2; the normal four-thread cap is covered by the default suite. The frozen 269×263 fixture now checks simultaneous full requests as well as ROI concurrency. Rebuilt home default/core-only/LittleCMS Release suites pass **26/26, 16/16, 17/17**, respectively; ten optional adapter/benchmark-graph tests pass.

All original, focused and high-ISO camera checks were rerun into separate ignored directories: **104 algorithm/ROI checks, 6,815,744 exact observations**, tile/async/halo contracts and zero native-calibrated averaging error pass. All eleven full-frame pairs and thirteen native crop boards retain their previous exact hashes, including bilinear. Pixel-identical boards reuse the earlier visual inspection; no new visual-quality claim follows. Engine-check timings collected during verification load are not added to the isolated benchmark medians. Earlier extraction/research/policy evidence is unchanged.

Local evidence under `build-msvc-release/research/`:

| Report | SHA-256 |
|---|---|
| `profile-before-isolated.json` | `4475c0151f809c1f30112c02f53afa7869f2990b39ee39ea5e3357b33de73696` |
| `profile-after-isolated.json` | `ddefb732e85c12e2e3f079d4c77ada8eeda325bed0f604dbf18ec6e2dd9561d1` |
| `profile-before-isolated-omp4.json` | `007df0504056c6cdef1d0621ba094d364335166095cac758504478076f5bce17` |
| `profile-after-isolated-omp4.json` | `1a32ca8327aef146350ca8adf01f28bbb4bf376c7884b77299b0098ac462c36c` |
| `preview-opt-verification-v1.json` | `54bcd8ebde9671de09db39d761806f5e2cccf65c58998002c59070f87828a0b8` |

The summary binds old/new camera reports and exact parity. New camera evidence lives under `tests/rawfiles/_librawops_local/preview-opt/{initial,focused,high-iso}/`; earlier directories are untouched. Each build's `preview-opt-ctest.log` records its full suite; default-build `preview-opt-omp{1,2}-ctest.log` records thread-limit goldens. Native source LF hashes are before `0a920f90d8e9a042aff47a3f96c3c6343326bfbe4924d9e7614054b1dc6c0918`, after `06f2315b7a42152f3ef51895816a05b022aaf16b6a60445dc195553527bd1a71`; DLL hashes are before `1410b66f51a28f0d7abdadd20ba98abdeffbb9e61b600dd6c72f8c42aac2685d`, after `1b550536afddfa57ad4ebc96da37cc1e9831772f6657fc691dcb57cb671a470c`.

Reproduce with a new report path, the current rebuilt module and retained extraction (optional `--cache-mib`, `--tile-size`, `--skip-stages`):

```powershell
& 'C:\Users\tylle\anaconda3\python.exe' bench/raw_session_benchmark.py build-msvc-release tests/rawfiles tests/rawfiles/_librawops_local/high-iso/extraction-v1.json build-msvc-release/research/profile-next.json --capture _DSC1793 --repeats 3
```

**Continuation:** the [fixed high-ISO noise/detail characterization](RAW_NOISE_CHARACTERIZATION_V1.md) is now complete: twelve native crops, 24 algorithm pairs, exact repeated pixels/metrics/figures and twelve diagnostic controls. It qualifies the earlier broad chroma-grain impression: intensity gradients, color-residual magnitude and spatial correlation differ by channel/scale. Next freeze/evaluate a separate intensity/color noise-control experiment with known-noise/detail controls before any native denoise port. Current samples suffice; matched ISO/known-light targets are later controlled evidence. Menon v1/source observations, bilinear default, broader camera/profile/corpus, full decode/export, allocation tracing and release gates remain unchanged/open. HEAD `4c7248a`; local changes, no commit or push.

**Latest continuation:** the [original smoothing-control experiment](RAW_NOISE_CONTROL_V1.md) is complete: five frozen settings, 120 saved-camera outputs, 20 synthetic truth pairs, exact repeat results and seven inspected boards. It quantifies smoothing damage separately from injected perturbation removal. This is an optional baseline, not a native/adaptive NR operation or model. Next define the downstream NR graph/preview contract and evaluate a separate candidate when user research arrives; all original engine/source/profile/release gates remain unchanged.

# Native Menon base v1 — 2026-10-01

Menon base is now a usable, opt-in engine reconstruction policy: **`rawengine.menon_base`, processing version 1**. It supports the existing decoded-Bayer metadata, tiled renderer, source-footprint planning, saved graphs, bounded cache, Python RawSession, async jobs, history and calibrated mip-1/2 previews. Legacy/default reconstruction remains immutable bilinear v1. The user explicitly authorized this engine implementation after reviewing the photographic/C++ results.

The [implementation plan](../plan/LIBRAWOPS_PLAN.md) is the authoritative handoff. The prior [photographic/standalone study](MENON_PHOTOGRAPHIC_CPP_STUDY.md) and every earlier policy-v1 rejection remain unchanged. This adds an optional prototype candidate; it does not certify camera quality, production readiness, Adobe equivalence or release/shipment clearance. Real-camera captures remain an open evidence gate.

## Use the existing API

```python
session = raw.RawSession(
    bayer, width, height, metadata,
    demosaic={"algorithm": "rawengine.menon_base", "processing_version": 1},
)
result = session.render(recipe)  # Existing WB, exposure, calibration/output controls.
text = session.export_manifest(recipe)
replayed = session.render_manifest(text)
```

For camera-linear source-only inspection, remove the exported manifest's operations and set `output` to its source ID. Calibrated mip-1/2 recipes continue to require a valid caller-supplied camera matrix and supported preview output. A request cannot change the source's reconstruction policy.

```cpp
auto source = std::make_shared<rawengine::RawUnpackNode>(
    image, rawengine::RawDemosaicIdentity{"rawengine.menon_base", 1});
auto tile = source->render(sensor_roi);
// Or use source in the existing Renderer / ExecutableEditGraph.
```

New RAW manifests pin the policy in format 3. Formats 1/2 permanently mean bilinear v1 and cannot bind to a Menon source. Source sample fingerprints are independent of algorithm selection; bindings validate both fingerprint and policy, and cache signatures/history source snapshots include the reconstruction identity. Unknown algorithms/versions still reject. Refined Menon remains a standalone research mode rather than an engine option. Existing one-shot and fixed-chain compatibility APIs retain bilinear.

## Numerical and sensor contract

[MenonDemosaic.cpp](../../MenonDemosaic.cpp) adapts Colour Science's BSD implementation at commit `7bff324983fb77b41444fda3bf922e354d386d1c`. It retains the original base stages, asymmetric posterior classifier, horizontal ties, completed green/color stages, float64 intermediates and float32 RGB output. No custom sign guard, refinement or clipping is applied.

Normalize each owned uint16 photosite to float32 with that sensor site's black/white levels **before** neighboring sites are mixed. CFA phase uses original sensor coordinates; the active-area origin is retained. Observed channels are copied exactly and signed shadows/headroom remain unbounded. White balance, exposure and calibration remain downstream operations. Row padding and inactive photosites are never sampled.

At actual active-area boundaries, directional/color lookup uses whole-sample mirror extension, whose even period preserves Bayer parity. Forward chroma differences use the same mirror rule; posterior accumulation zero-extends the resulting gradient planes. Internal tile/ROI/block boundaries always obtain source support. Singleton active axes explicitly use the unchanged valid-neighbor native bilinear fallback; absent colors become zero as in bilinear v1. Empty requests return empty tiles.

## Bounded stages and composed halo

Each requested rectangle is divided internally into output blocks of at most **256×256**, including a direct request for a whole frame. For each output block:

| Stage | Stored extent relative to output block |
|---|---|
| Float32 normalized Bayer | Symmetric radius 6, clipped to active area |
| Float64 directional green and chroma planes | Symmetric radius 4 |
| Selected green and retained direction | Symmetric radius 2 |
| Green-site red/blue planes | Symmetric radius 1 |
| Final RGB | Requested output pixels |

The posterior's horizontal offsets are `[-2,0] × [-2,2]`; vertical offsets are transposed. Horizontal gradient storage expands the selected-green rectangle by `(left=2, top=2, right=0, bottom=2)`, and vertical storage by `(2,2,2,0)`. Their forward two-pixel differences therefore remain within the radius-four chroma planes. The chroma's five-tap green prediction needs radius two more, giving the **radius-six raw halo**. Every plane accessor checks its declared support, so an accidental extra dependency fails rather than silently reading a clipped tile border.

Gradients are computed once per stored cell and reused by classifier terms. At observed green sites every chroma-gradient term is zero: the classifier offsets preserve green parity, and their two-step differences also land on green. Those sites preserve the original horizontal tie directly. These are execution optimizations of the fixed equations; pixel parity checks cover the resulting behavior.

The maximum array payload per interior block is **5,297,680 bytes (~5.05 MiB)**: one float32 Bayer plane, nine float64 stage/gradient planes and one direction-byte plane. This is an analytical scratch bound, not a measured process peak. Source storage, returned RGB tiles, cache, worker/job overhead and allocator overhead are additional costs. Scratch scales with concurrent workers rather than full-frame dimensions. A materialized full-frame result still requires its full RGB allocation; the streaming renderer avoids that output allocation.

`RawUnpackNode::input_region()` reports radius six for normal Menon extents, radius one for the singleton fallback, always clipped to the true active area. Existing graph/reduced-preview planners compose that policy through calibration and other nodes.

## Verification and frozen references

[Native fixture generator](../../tools/raw_menon_native_reference.py) executes the exact pinned upstream source using the isolated scientific runtime. [The binary fixture/index](../../tests/reference/raw/menon_native_base_v1.index.json) contains **93** synthetic uint16 records, complete metadata and upstream base/fallback outputs. SHA-256: **`0b753a122a8bbf7c8e8c609df32324061e67d291078a1802f1a7ec267b78feac`**. It includes all pattern/phase/origin combinations on signed/headroom dyadics, non-dyadic site normalization, tiny/odd/thin/constant images and a 269×263 case crossing internal 256-pixel boundaries.

[Native tests](../../tests/raw_menon_native_tests.cpp) check **352,221 reference channels** bit for bit across direct, tiled and ROI reconstruction, then poison inactive/padded data and repeat. They also check empty requests, source halos, concurrent use, singleton fallback, policy/manifest replay, cache isolation, history restoration and unknown-policy rejection. The core test reads the binary directly and has no scientific or Python dependency.

[Python integration tests](../../tests/python_raw_menon_tests.py) cover saved source-only goldens, signed/headroom values, tiled/ROI/jobs, copied policy info, legacy/policy mismatch rejection and history. A separate color-matrix/WB/exposure reference starts from upstream Menon pixels and verifies native calibration followed by mip-1/2 averaging, plus the normal encoded preview and async path.

[Optional engine checker](../../tools/raw_menon_engine_check.py) loads all **160** photographic reference planes into padded, phase-shifted uint16 sensors. With the cache disabled, it performs **480 complete-image checks** (tile sizes 512/29/7), **320 ROI checks** and **320 source-footprint checks**. All native outputs are bit-identical to the saved upstream base outputs: **MSE = RMSE = max absolute difference = 0**. Each complete pass covers 7,864,320 RGB channels. Four additional all-pattern controls with white-minus-black spans as small as one and large signed normalized values also matched upstream bit for bit.

Final Release suites pass **26/26 default, 16/16 core-only and 17/17 LittleCMS** after gradient caching. The [preserved engine report/index](../../tests/reference/raw/menon_engine_base_v1.index.json) binds source/binary/fixture hashes, final test logs, photo parity, benchmark and staged-install notice. JSON SHA-256 `d51bd94d31ebcb988f8d389ae80bea8567f129e6d28a4e2d6ebc056dd41203f6`; [gzip report](../../tests/reference/raw/menon_engine_base_v1.json.gz) SHA-256 `3e1899efa221f3420e9dbe4accb112cc8e916a26e2af83831712bfae6336d8ca`. Verification uses MSVC 19.44.35228.0 x64, Menon `/fp:strict`, and bundled Python 3.12.14. The former Windows Store Python executable was inaccessible under this session's permissions; existing local build caches were reconfigured to the bundled runtime. Linux/macOS and other compilers are not yet verified. Existing LittleCMS headers emit their pre-existing `register` warnings; Menon code builds warning-free.

Synthetic source-only cold streaming, tile 256, median of three on **Intel Core i5-14500 / Balanced** measured **99.0987 ms for 1 MP** and **7,793.81 ms for 45 MP**. The 45 MP runs were 5,633.39 / 7,980.02 / 7,793.81 ms, with uncontrolled background/thermal conditions. This excludes decode, calibration, denoise, cache, display and export; it is not an end-to-end or target-hardware performance result.

Reproduce with the built extension and existing hashed photographic reference cache:

```sh
python tools/raw_menon_engine_check.py build/Release build/research/photo-demosaic-v1/references build/research/photo-demosaic-v1/report-v1.json build/research/menon-cpp/engine-check-new.json
ctest --test-dir build -C Release --output-on-failure
ctest --test-dir build-core -C Release --output-on-failure
ctest --test-dir build-lcms -C Release --output-on-failure
build/Release/RawEngineMenonBenchmark.exe 7500 6000 3 256
```

The core fixtures and Python integration tests do not need the photo cache. Source snapshots and the scientific runtime are needed only to regenerate the frozen oracle data.

## Provenance and remaining work

The exact BSD notice is retained in [third_party/colour-demosaicing/LICENSE](../../third_party/colour-demosaicing/LICENSE) and installed as `share/licenses/RawEngine/Colour-Demosaicing-LICENSE`. Its SHA-256 remains `62f431963cc2e7387c118c0798bcb323a1e0c0129b28265099ab74bf2a0c04af`. The port adds no runtime dependency beyond the existing engine configuration, whose Windows DLL retains MSVC/OpenMP/Windows imports; no NumPy, SciPy, Colour, OpenBLAS or decoder dependency is introduced by this port. A staged install verifies the notice byte for byte. Copyright permission and the existing patent/jurisdiction/shipment review remain separate.

Next obtain reviewed actual-camera RAW captures with disclosed decoder/settings and inspect calibrated detail, false color, moiré, noise, shadows and highlights. Evaluate the candidate's intended practical use and preserve the previous synthetic alias/chromatic limitations and analytical-policy rejection. Do not switch the default or claim universal quality superiority from remosaicing parity. Full RAW/preview/export timings, process peak memory, allocations, target-machine performance and release audits remain open. No commit or push was performed.

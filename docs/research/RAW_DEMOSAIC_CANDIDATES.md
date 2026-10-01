# RAW demosaic candidates and camera corpus — initial review

Reviewed 2026-09-30 against local code base `de66627` plus the versioned bilinear and quality-policy changes. An original scalar Hamilton–Adams research reference has now been measured and rejected against the frozen gates. No second native backend, third-party implementation, runtime dependency or real camera asset has been added. The authoritative continuation remains [the implementation plan](../plan/LIBRAWOPS_PLAN.md).

## Candidate comparison

| Candidate | Published design | Fit with this engine — engineering inference | Admission status |
|---|---|---|---|
| Malvar–He–Cutler (MHC, 2004) | Local 5×5 linear filters with cross-channel correction | Compact implementation; a fixed radius-two stencil would be straightforward to plan, subject to explicit CFA and border rules | Hold: known patent requires current status/family review; reviewed IPOL code is unsuitable for redistribution |
| Hamilton–Adams (1997) | Direction classification from gradients/second derivatives, then directional color interpolation | Original scalar v1 now measured, with radius-three support and explicit bilinear border | Scalar version rejected by frozen analytical/regression/improvement gates; no native admission; jurisdiction review remains open |
| Menon–Andriani–Calvagno DDFAPD (2007) | Directional interpolation, a posteriori decision and refinement | More stages and temporary data; staged dependency radius, ties and borders need a larger design/measurement slice | Retain as later benchmark candidate; patent review and implementation audit incomplete |

Primary design references: [Microsoft MHC paper](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/Demosaicing_ICASSP04.pdf), [Hamilton–Adams US grant](https://patentimages.storage.googleapis.com/79/b2/75/80e2af2ff91dc9/US5629734.pdf), and [Menon publication record and DOI](https://www.research.unipd.it/handle/11577/2452626). The Microsoft paper also explains why simple RGB subsampling does not fully model real sensor acquisition. None of these publications establishes that an implementation will meet our frozen replacement policy.

**Updated recommendation:** start with an analytical error/quantization-budget audit before another algorithm. Cross-channel correction can propagate differently quantized sites beyond a sensor half-step; derive any allowance independently using exact/quantized fixtures, preserve observed-site requirements, and explicitly version a justified contract change while retaining the original policy/results. Do not weaken thresholds to admit scalar Hamilton–Adams v1: it remains rejected under the current policy, with material chromatic regressions. Then investigate Menon, run an independent scalar comparison on all 2,368 frozen fixtures, inspect admitted camera crops, and port only a passing/admitted candidate. Menon admission and its larger staged footprint remain open; MHC retains its known patent hold. This is an engineering disposition, not legal clearance or a judgment based on real cameras.

## Provenance and patent findings

The [original MHC grant, US7502505B2](https://patentimages.storage.googleapis.com/86/f9/48/5d5f71204128d2/US7502505.pdf), records a March 15, 2004 filing, March 10, 2009 grant and **1,046 days of patent-term adjustment**. Publication age therefore cannot establish expiry. Current official status, relevant claims, related grants and target jurisdictions remain unreviewed. Search indexes are discovery aids rather than clearance evidence.

The [reviewed IPOL MHC source terms](https://www.ipol.im/pub/art/2011/g_mhcd/srcdoc/dmmalvar_8c.html) restrict use and redistribution, and identify additional patent restrictions. Do not copy, compile, port or ship that reference implementation. The original paper may inform an independently written design, but that does not resolve patent admission.

The [original Hamilton–Adams grant, US5629734A](https://patentimages.storage.googleapis.com/79/b2/75/80e2af2ff91dc9/US5629734.pdf), records a March 17, 1995 filing and May 13, 1997 grant, and identifies the related US5506619 work. Confirm current official status and relevant family/jurisdiction coverage before admission. An [IPOL author discussion from 2011](https://tools.ipol.im/mailman/archive/discuss/2011-April/000334.html) describes restricted historical reference-code terms; current per-file terms were not obtained in this review. No historical IPOL Hamilton–Adams code is an approved implementation source.

Menon's institutional record identifies DOI `10.1109/TIP.2006.884928`. The full journal article was not obtained in this run. The upstream [Colour Science Menon source documentation](https://colour-demosaicing.readthedocs.io/en/develop/_modules/colour_demosaicing/bayer/demosaicing/menon2007.html) shows staged horizontal/vertical estimates, direction decisions, red/blue reconstruction and optional refinement with differing boundary treatments. That implementation was read as a reference; no code was imported. This initial search does not establish the absence of applicable patents.

### Exact permissive reference-code snapshot

The upstream [Colour Science repository](https://github.com/colour-science/colour-demosaicing) was inspected at commit `7bff324983fb77b41444fda3bf922e354d386d1c`, dated 2026-06-27. Exact upstream bytes were hashed without retaining or installing them:

| File at that commit | SHA-256 |
|---|---|
| [LICENSE](https://github.com/colour-science/colour-demosaicing/blob/7bff324983fb77b41444fda3bf922e354d386d1c/LICENSE) | `62f431963cc2e7387c118c0798bcb323a1e0c0129b28265099ab74bf2a0c04af` |
| [malvar2004.py](https://github.com/colour-science/colour-demosaicing/blob/7bff324983fb77b41444fda3bf922e354d386d1c/colour_demosaicing/bayer/demosaicing/malvar2004.py) | `4e1b2e70841864bc91c3b1eab129d4303eb89ee7eb71140954f56e80dc01c020` |
| [menon2007.py](https://github.com/colour-science/colour-demosaicing/blob/7bff324983fb77b41444fda3bf922e354d386d1c/colour_demosaicing/bayer/demosaicing/menon2007.py) | `5621bde89d89f85962ac19afc47ad0dc63d962d7f11a3895d9a60d3aa0f8fbf2` |
| [pyproject.toml](https://github.com/colour-science/colour-demosaicing/blob/7bff324983fb77b41444fda3bf922e354d386d1c/pyproject.toml) | `73f4254287daa552278f4c8e161ef5005e6b1fd431714ceee7dcf2ed949fe5cc` |

The inspected license is BSD-3-Clause, requiring retained notice/disclaimer and prohibiting endorsement without permission. It contains no explicit patent grant. Copyright permission does not resolve the known MHC patent or Menon's open patent review. This package and its NumPy/SciPy/Colour/ImageIO dependencies are **not admitted dependencies**; a future port or installation needs its own exact subset, notices and transitive audit. The native engine and new corpus tooling keep their existing dependency boundaries.

## Engine constraints before a candidate port

These requirements follow from the current engine contracts, rather than the cited algorithms:

1. Preserve `rawengine.bilinear` processing version 1 exactly. A new backend gets a distinct immutable identity. Unknown policies continue to reject in source binding, replay, cache and history.
2. Reconstruct camera-linear RGB before explicit WB, exposure and camera calibration. Normalize each observed CFA site using its own black/white levels before mixing channels. Preserve observed samples, signed shadows and headroom without clipping. Do not move WB merely to improve a synthetic score.
3. Derive the entire dependency halo across stages. A five-tap green estimate does not determine the halo of later color-difference interpolation or refinement. Update native source-footprint planning alongside rendering.
4. Specify ties and true active-area borders, including one-pixel/thin extents. CFA parity uses original sensor coordinates and declared phase. Hostile inactive pixels and row padding must not leak into output. Never treat a tile or requested ROI edge as a sensor boundary; any reflection/fallback must be CFA-aware and covered by the algorithm version.
5. Parameterize the quality generator's selected policy and report header together; it currently uses bilinear. Verify saved source policy and runtime source_info agree, keep the original baseline immutable, and evaluate the complete 2,368-case policy without tuning thresholds after seeing scores.
6. Verify full/tiled/ROI, observed-site/analytical, cache/history/replay and native-before-reduction behavior before inspecting camera crops and recording performance. A permissive source, a correct implementation and an accepted replacement are separate gates.

## Camera-corpus contract v1

[camera_corpus_plan_v1.json](../../tests/reference/raw/camera_corpus_plan_v1.json) proposes **three camera models**, each under low ISO (at most 400) and high ISO (at least 1600), daylight and tungsten: at least **12 distinct capture conditions**. Prefer different sensor designs and at least two makers; that preference is not enforced by the validator. This is a starting engineering sample, not statistically sufficient camera coverage or a supported-camera promise.

Across those captures require neutral/color patches, skin, fine texture, slanted edges, repeating detail, deep shadows, highlight headroom and clipped highlights. Use camera-as-shot or measured-neutral RGB gains as a recorded WB reference. Retain sensor-coordinate crop selections and inspect both native and calibrated/WB preview views for false color, zippering, moiré, detail, noise and clipping behavior. Record processing/view settings with later evaluations; this initial manifest records assets and inspection ROIs, not rendered evaluation outcomes or camera profiles.

Each record requires:

- A unique capture ID and original RAW, complete padded little-endian uint16 Bayer buffer and rights-evidence file, each with a confined relative path and SHA-256.
- Width/height/row stride, Bayer pattern and phase, active rectangle, and four sensor-site black/white levels matching the current decoded-Bayer API. Any decode linearization, clipping, cropping or correction must be disclosed in the recorded decode settings and assessed for admissibility.
- Camera make/model, ISO and declared band, illuminant, positive exposure time, and positive finite RGB WB gains with their method.
- Decoder name/version/settings, recorded rights review identifier, and an explicit allowed-redistribution or local-only declaration backed by the evidence file.
- Known content tags and nonempty positive sensor-coordinate inspection ROIs confined to the active rectangle. Multiple ROIs belong to one capture; duplicate original hashes cannot count as extra coverage.

The executable synthetic record in [raw_camera_corpus_tests.py](../../tests/raw_camera_corpus_tests.py) demonstrates the exact field structure. Its opaque originals and generated samples are unit-test fixtures, not camera evidence. The repository's plan deliberately contains **zero admitted assets**.

### Validator and acceptance limits

`tools/raw_camera_corpus.py` uses only the Python standard library. It rejects unknown/duplicate fields, malformed metadata, unsupported encoding, nonfinite capture values, missing/tampered files, wrong decoded byte count, escaped paths and unreviewed rights declarations. It reports missing model count, each included camera's missing ISO/illumination cells, missing content tags and local-only assets. Coverage is the declared cross-product plus aggregate content tags; it does not infer physical noise or independently verify metadata, rights or content.

```sh
python tests/raw_camera_corpus_tests.py
python tools/raw_camera_corpus.py tests/reference/raw/camera_corpus_plan_v1.json build/camera-corpus-status-v1.json --require-coverage
```

The empty plan writes an incomplete report and exits **1**, as expected. A valid partial manifest exits 0 without `--require-coverage`; malformed evidence exits 2. `coverage_ready` can be true for reviewed local-only assets, while `all_assets_declared_redistributable` stays false. These flags are recorded declarations, not distribution authorization. Keep private originals and rights evidence outside the repository under a local manifest; do not fetch or admit samples merely because they are publicly accessible.

Real RAW captures have no analytical RGB ground truth. Keep their observed-site/tile/ROI correctness, rendered crop assessments and later camera/profile measurements separate from synthetic reconstruction error and Adobe comparisons. The corpus validator does not decode, render, score, establish asset permissions or clear a decoder for shipment.

## Next bounded checkpoint

Audit the analytical error/quantization budget first, following the ordered next-session sequence in the authoritative plan. Then review Menon equations/provenance/staged dependencies before an independent scalar study against all 2,368 fixtures, admitted real-camera crop assessment and a gated native port. Continue camera acquisition/coverage and jurisdiction review independently. A second native backend and replacement acceptance remain open; the existing HA rejection and original policy/evidence are retained.

Validation for this checkpoint: eight corpus tests passed; reconfigured existing optimized Windows trees and CTest passed **18/18 default, 9/9 core-only, 10/10 LittleCMS**. Native renderer math was unchanged. The empty corpus plan correctly failed coverage. No commit or push.

## Hamilton–Adams follow-up — dependency and admission checkpoint

### Patent evidence disposition

The two original US grants describe concurrent applications, not a parent/continuation pair. The separately retrieved [US5506619 grant](https://patentimages.storage.googleapis.com/ba/7b/60/cfd8e899bd244c/US5506619.pdf) confirms application `08/407,436`, filed March 17, 1995, granted April 9, 1996, and cross-references `08/407,423`. US5629734 adds composite directional classification and diagonal chroma decisions; the concurrent grant describes a different Laplacian selection scheme. Do not silently combine their variants under one policy name.

[USPTO MPEP 2701](https://www.uspto.gov/web/offices/pac/mpep/s2701.html) gives pre-June-8-1995 applications the greater of twenty years from the applicable filing date or seventeen years from grant, subject to applicable qualifications. [MPEP 2710](https://www.uspto.gov/web/offices/pac/mpep/s2710.html) excludes those applications from USPTO-delay term adjustment/extension. Applying the ordinary rule to the printed dates gives the following **nominal calculation**, not an official case-status determination:

| US grant | Twenty-year filing anniversary | Seventeen-year grant anniversary | Later ordinary date |
|---|---|---|---|
| US5629734 | 2015-03-17 | 2014-05-13 | 2015-03-17 |
| US5506619 | 2015-03-17 | 2013-04-09 | 2015-03-17 |

This materially narrows the identified-US-grant concern. It does not establish worldwide freedom to operate or cover later improvement claims. No delay adjustment was inferred from a secondary index. Regulatory extensions, full prosecution records and relevant later claims were not audited.

Discovery indexes surfaced these related jurisdiction leads; **neither their legal status nor completeness is verified**:

| US application | EP / German leads | Japanese lead |
|---|---|---|
| 08/407,423 | EP0732859B1 / DE69628867T2 | JP3510037B2 |
| 08/407,436 | EP0732858B1 / DE69628866T2 | JP3871072B2 |

Primary case retrieval was attempted at [USPTO 08/407,423](https://patentcenter.uspto.gov/applications/08407423), [USPTO 08/407,436](https://patentcenter.uspto.gov/applications/08407436), [EPO 96200571](https://register.epo.org/application?number=EP96200571) and [EPO 96200568](https://register.epo.org/application?number=EP96200568). USPTO returned no readable case content; EPO retrieval failed. The leads came from the indexed patent records and remain search tasks, not primary status evidence. **Disposition: engineering design research may proceed; backend admission remains open.** No reference implementation has been copied or selected.

### Proposed two-stage support contract

This is an engine design proposal based on the staged description in US5629734, not a port of IPOL or an assertion that every Hamilton–Adams implementation is identical. Formula coefficients still need a separately checked numerical specification from the original equation figures before implementation. OCR omitted those equations in this run; classifier weights must not be guessed from prose.

Use original sensor coordinates throughout. Let `p` be the requested sensor pixel and `e_x/e_y` the unit axes. Normalize observed sensor values with the existing per-site rules. The proposed stages and dependency sets are:

| Stage | Required values | Original Bayer footprint relative to target |
|---|---|---|
| Observed channel | Copy normalized observed sample | `{p}` |
| Green at red/blue | Observed green at `p ± e_x, p ± e_y`; same observed red/blue at `p, p ± 2e_x, p ± 2e_y` | Axis cross of radius 2 |
| Red/blue at green | Same-color samples one step along their CFA axis; reconstructed green at those positions and observed green at target | Compose one-step neighbor offset with radius-2 green support; bound radius 3 |
| Opposite red/blue | Observed target-color diagonal pairs; reconstructed green at those diagonals and target for correction/direction decision | Compose `(±1, ±1)` with radius-2 green support; bound radius 3 |

Choose the lower directional classifier; for exact equality use the arithmetic average of both directional predictors. Use the same symmetric tie policy for diagonal reconstruction. This is a deliberate versioned choice; do not use an undocumented horizontal/diagonal preference or floating epsilon. Determine every decision from unchanged observations and completed green values, never from partially updated red/blue values.

For the described stencil union, maximum displacement in either axis is **3**, reached by a diagonal neighbor's green dependency, e.g. `(1,1) + (2,0) = (3,1)`. Therefore a conservative `input_region` is the requested rectangle expanded by three in each direction and clipped to the true active rectangle. A radius-two footprint is insufficient; diagonal offsets do not require a full radius-four square. This result applies only to these stated stages, without extra smoothing/refinement.

An independent set-union calculation enumerated all CFA parities and required green dependencies: the bound is 3 in both axes and the outer points are present. It also checked clipped planned rectangles contain the interior stencil for zero/nonzero sensor origins, odd extents and one-pixel requests. This is a dependency proof, **not a pixel, quality or native tile test**.

### Border, precision and allocation choices to freeze before coding

For a first bounded variant, propose using the existing bilinear v1 pixel result throughout the three-pixel active-area border. Use the two-stage method only where the full radius-three support fits. Images narrower or shorter than seven pixels therefore use bilinear throughout. This avoids reflected CFA samples, missing-color extrapolation and inactive/padding reads; it may create a transition seam that must be inspected and measured. Keep this choice in the new policy identity even if a later version introduces better borders.

Green estimates needed by an interior target are evaluated at their true coordinates with their complete radius-two support, including positions within that output border. Do **not** substitute the final border pixel's bilinear green into the interior stage. Internal green estimates and final output fallback are distinct; otherwise the intended stencil and decisions change near the boundary.

Retain existing float32 normalized sample values; propose double arithmetic for classifiers/predictors and an explicit float32 green-stage result before chroma reconstruction. Copy observed channels exactly and cast each completed missing channel once. Do not clip negative/above-one values or enable floating-point reassociation. A future numerical specification must pin evaluation order and check these choices against the independent oracle.

For a requested tile, stage green only over its one-pixel expanded neighborhood, clipped to active bounds; original sample reads may extend a further two. Use immutable temporary data per render call. Bound scratch by that neighborhood's pixel count and checked element arithmetic; do not retain a hidden full-sensor green plane or share mutable scratch across render jobs. No tile/ROI boundary can trigger fallback. Native-before-WB/calibration and direct native-to-mip reduction retain their existing contracts.

### Remaining implementation admission work

- Obtain readable primary records for the identified jurisdiction leads and finish relevant-family/later-claim disposition; ordinary US date calculations alone do not close this gate.
- Verify equation figures and freeze classifier/predictor coefficients, evaluation order and tie behavior in a numerical specification, with explicit differences from the original alternatives.
- Write an independent scalar oracle only after that admission/specification checkpoint; cover all CFA patterns/phases, per-site levels, thin/offset borders, impulse/affine and adversarial directional ties.
- Then implement a separately versioned backend, parameterize the existing quality-report policy, verify composed footprint/tile/ROI/replay/cache/history behavior and evaluate the frozen replacement gates.

This follow-up changes documentation only. Previously recorded CTest results remain **18/18, 9/9, 10/10**; suites were not rerun. No second algorithm is registered, and no commit or push was made.

## Scalar reference and full synthetic result — 2026-09-30

The original grant was downloaded to the ignored research directory, SHA-256 `b1668be77e9adfbabbee5aac0ea33c1e63e0db0e421ef366a6099536a6f78d84`. Poppler rendered PDF pages 9–10 (printed pages 5–8); the equation figures on printed pages 5–7 were visually inspected. This closes the earlier OCR gap. [The original grant](https://patentimages.storage.googleapis.com/79/b2/75/80e2af2ff91dc9/US5629734.pdf) is the mathematical source; no IPOL, Colour Science or copyleft implementation code was used.

`tools/raw_ha_reference.py` is original standard-library research tooling, not an admitted library backend. Its version-1 numerical choices are fixed in source:

- Green directional classifier: absolute same-color second difference plus the adjacent-green difference. Predictor: adjacent-green mean plus one quarter of that signed second difference.
- Red/blue axial or diagonal predictor: observed target-color mean plus one half of the reconstructed-green second difference. Opposite-color diagonal selection adds the green second-difference magnitude and the observed-color difference magnitude.
- Exact ties average the two predictors. Normalized observed samples and completed green/missing-color outputs are float32; predictors/classifiers use Python binary64 with explicit order. The final three-pixel border uses the existing float32 bilinear accumulation order; observed channels are copied without correction or clipping.

The scalar oracle owns bounded active observations and a complete green plane, with a one-million-sensor-sample admission budget. That full-plane oracle storage is deliberately independent of the proposed native per-tile scratch implementation. It validates strict integer per-site uint16 metadata and confined ROIs, ignores inactive/padding samples, and is deterministic across ROI order. It does not implement scheduler/cache/history/preview contracts or serve as a camera decoder.

The evaluator regenerates each preserved fixture and verifies its Bayer, truth and metadata identity before pairing metrics. It reuses the existing reducers, score definitions and frozen policy, while retaining a separate `research_report_schema_version` with `native_replacement_accepted: false`. It never manufactures a native session identity, edit manifest, native tile result or native acceptance report. Full research reports record the reference/helper source hashes, generator/formula version, domain, preserved baseline hash, policy and all paired measurements. Quick output is explicitly incomplete (148 cases).

### Full result

| Measurement | Scalar HA v1 result |
|---|---|
| Full paired fixture cases | 2,368 |
| Observed fidelity and quantization gates | Passed in all 2,368 cases |
| Analytical reconstruction failures | 144 cases: 64 neutral affine, 64 chromatic affine, 16 neutral-mid flat |
| Per-case regression limits | 2,784 failed score/scope checks across 832 cases |
| Edge/detail improvement gates | 16/24 pass; all eight chromatic probe gates fail |
| Neutral edge/detail RGB RMSE mean improvements | About 47–72%; corresponding residual chroma also improves |
| Chromatic hard-edge RGB RMSE means | About 9% worse |
| Chromatic sine RGB RMSE means | About 22–25% worse |
| Native replacement acceptance | Not established; this scalar version is rejected for promotion |

These are native-resolution synthetic camera-RGB measurements under the fixed before-WB/calibration contract. They do not establish perceptual, camera or Adobe quality. Analytical quantization amplification and adversarial independent-color behavior are measured limitations; thresholds have not been loosened to admit the candidate.

Two complete reports were byte-identical. The rejected result is preserved as [research gzip evidence](../../tests/reference/raw/ha_scalar_research_v1.json.gz) with a [readable index](../../tests/reference/raw/ha_scalar_research_v1.index.json), independently from the native baseline. JSON SHA-256 `dbfbed9861a4e76994cfe0b1c0f3f1d8d2f3657c64fa849b96aeb22439028203`; gzip SHA-256 `14f629983cde959bf6089e31727f0f07eed45456c8679a1e8b3182b0952aa145`. The index links reference/helper sources, baseline, policy, counts and the figure; read-back/decompression matched the full JSON exactly.

**Quantization counterexample:** `neutral_mid`, packed RGGB/phase-zero with per-site levels, truth `0.37123000621795654`. At sensor `(30,25)`, reconstructed red is `0.3711903989315033`, error `3.960728645324707e-5`, exceeding the existing bound plus allowance (`3.846153846153846e-5 + 1e-6`). The color-difference correction combines differently quantized green sites. Observed green remains exact. A hand-checked test pins this behavior, so a future change cannot silently hide the counterexample or redefine the acceptance limit.

### Diagnostic crops

![Analytic truth, pinned bilinear and scalar HA crops](ha_scalar_crops_v1.png)

Four packed RGGB/phase-zero/uniform-level examples show the neutral edge/detail benefit, remaining colored edge artifacts and the border fallback. The recreated bilinear columns match the immutable native float32 hashes exactly before display conversion. Camera RGB is clipped and sRGB-encoded identically only for this illustrative inspection; no WB, calibration or tone curve is applied. Display clipping conceals signed/headroom errors, and selected crops cannot override the full numeric gates.

### Reproduction and verification

```sh
python tests/raw_ha_reference_tests.py
python tools/raw_ha_reference.py tests/reference/raw/bilinear_baseline_v3.json.gz build/ha-scalar-v1.json
```

The CLI exits 0 when evaluation succeeds even if the measured gates fail: a rejected candidate is valid research evidence. Malformed evidence fails. Add `--quick` for 148 cases, unsuitable for full coverage. The native comparator continues to accept its existing native-harness schema only.

Twelve tests cover independently hand-calculated directional/tie/chroma values, an actual sensor patch, signed chromatic flats, exact affine interiors across patterns/phases/origins, site levels, observed fidelity, internal-green versus final-border separation, thin extents, ROI partition/order, ownership, hostile padding, invalid metadata/ROI/budgets, research pairing and changed-fixture rejection, and the frozen flat counterexample. Default/core-only/LittleCMS full CTest passed **19/19, 10/10, 11/11**, followed by the final twelve-case reference suite. Native binaries, bilinear evidence and policy thresholds remain unchanged. Full research evidence and repeatability checks are linked from the authoritative handoff.

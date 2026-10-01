# Colour Science demosaicing code review — 2026-10-01

The review found a concrete boundary improvement to investigate, but no missing Menon reconstruction stage. Our scalar Menon already implements the published directional green estimates, posterior direction decision, color-difference reconstruction and three refinement stages. Colour Science's original code reproduces the important chromatic counterexamples. Its true-image boundary treatment retains substantially more neutral-detail benefit than our conservative radius-six/eight bilinear fallback.

This is a code comparison and a **164-case synthetic study**, not acceptance of a native backend. The [implementation plan](../plan/LIBRAWOPS_PLAN.md) remains the authoritative handoff. Earlier 2,368-case rejections and policy v1 are unchanged. No representative-camera, Adobe or native performance evidence was obtained.

**Completed continuation:** the [photographic/C++ study](MENON_PHOTOGRAPHIC_CPP_STUDY.md) supplies the next decision described below: Menon base improves all five photo sources, gives 35.82%/32.09% interior gains and now has a faithful standalone C++ port passing 842 comparisons. Bounded native tiles, actual-camera evidence and product admission remain open. This review's original results are unchanged.

## Exact code and execution scope

Reviewed the BSD-3-Clause source at commit **`7bff324983fb77b41444fda3bf922e354d386d1c`**: [Menon](https://github.com/colour-science/colour-demosaicing/blob/7bff324983fb77b41444fda3bf922e354d386d1c/colour_demosaicing/bayer/demosaicing/menon2007.py), [Malvar](https://github.com/colour-science/colour-demosaicing/blob/7bff324983fb77b41444fda3bf922e354d386d1c/colour_demosaicing/bayer/demosaicing/malvar2004.py), [bilinear](https://github.com/colour-science/colour-demosaicing/blob/7bff324983fb77b41444fda3bf922e354d386d1c/colour_demosaicing/bayer/demosaicing/bilinear.py), masks, mosaicing, all three demosaicing test modules, the Bayer example notebook and LICENSE. Exact bytes/hashes are retained in ignored `build/research/colour-code-review-v1` and recorded in the evidence.

The comparison executes the pinned demosaicing and mask files rather than silently substituting the installed package's implementation. The isolated research runtime supplies NumPy 2.3.5, SciPy 1.16.3, Colour 0.4.6 and Colour Demosaicing 0.2.6 helpers. Default upstream array conversion is verified as float64; the Colour array helper hash is recorded. The engine, normal tests and product dependency boundary do not import this runtime.

Fixture selection was fixed in the tool before evaluation:

- All 37 frozen probes × four CFA patterns, packed/uniform levels/phase `(0,0)`: **148 cases**.
- Neutral-mid, neutral-affine, chromatic-affine and chromatic 1/8-x sine × four CFA patterns, shifted active area/per-site levels/phase `(1,0)`: **16 additional metadata controls**.

Every fixture's full Bayer bytes, truth and metadata match the frozen native baseline. Normalized observations use the engine's float32 normalization before conversion to upstream float64. The effective pattern is derived at the active-area origin, preserving original sensor phase. Regenerated native bilinear and scalar Menon output hashes match their existing archives on every selected pair.

## What the code does, and what we do

| Concern | Colour Science implementation | LibRawOps state and implication |
|---|---|---|
| Input | A normalized 2D Bayer plane plus its effective pattern | The native source additionally validates uint16 ownership, stride, active area, phase and site-specific black/white levels. Normalize/crop/rebase pattern before calling equivalent reconstruction logic. |
| Menon green | Horizontal/vertical five-tap predictions; color-difference gradients; weighted posterior direction decision | Present in our scalar research code. Both choose horizontal on a classifier tie. No missing decision stage was found. |
| Missing red/blue | Color differences along the CFA axis at green sites, then along the selected direction at opposite-color sites | Present in our scalar code, including use of completed earlier stages. |
| Refinement | Update missing green, red/blue at green, then opposite red/blue using the retained direction map | All three stages are present in our refined scalar variant. Upstream `DDFAPD` is an alias for Menon, not another algorithm. |
| Precision | Default float64 working arrays; output is unbounded | We normalize and store scalar reconstruction stages as float32. Both preserve signed values and headroom. Output precision and stage precision are separate choices. |
| Menon boundaries | Whole-sample mirror convolution, reflected gradient padding and zero extension of classifier accumulation | Our final output falls back to bilinear throughout a six/eight-pixel band. That was a conservative support contract, not a requirement of Menon's equations. |
| Malvar | Four fixed 5×5 filters, with CFA-dependent selection and transposition; no direction classifier | Not implemented by our native engine or scalar candidates. Radius-two interior support offers a smaller C++ implementation target, subject to quality and existing admission decisions. |
| Execution | Vectorized arrays with SciPy convolution kernels | Our Menon code is a scalar research oracle. The shipped native renderer still supports bilinear only. A C++ staged implementation is a distinct integration task. No speed comparison was made. |
| Tests/examples | Saved Lighthouse EXR outputs across all four patterns, both Menon modes; example mosaics an RGB image | These check reproducibility/examples. They do not establish our all-probe replacement conditions, representative-camera quality or sensor metadata/tile contracts. Example/test image assets were not acquired or evaluated. |

The upstream algorithm functions do not supply RAW-file decoding, black-level calibration, white balance, camera-to-working color, denoising, edit graphs or tiled replay. Those surrounding responsibilities remain ours.

## Direct numerical comparison

Run upstream bilinear, Malvar, Menon base and refined with their original boundaries. Separately measure each upstream Menon output with our existing final bilinear band applied; this adapter isolates the effect of boundaries from reconstruction/precision. It is recorded as a separate identity and does not alter upstream code or policy scopes.

Across **132,594** red/blue sites in the common radius-eight interior, float64 upstream and float32 scalar direction choices differ at **448 sites**, all on neutral-affine probes with very close classifier values. The maximum interior output differences are **3.331899642944336e-5 base** and **2.2232532501220703e-5 refined**. These are branch changes near ties, so they should not be described as merely final-store rounding. About 98.10% of base and 97.20% of refined interior channels are bit-identical after float32 output conversion.

Their practical effect in this subset is small: the largest common-interior RGB RMSE difference is **3.462e-9 base** and **8.014e-9 refined**. The chromatic 1/8-x sine control has **exactly identical** interior outputs. The hard-edge controls have differences around one float32 step. With our border adapter, upstream and original scalar have the same subset failure counts: **18 correctness failures, 252/250 regression checks and 16/24 improvement gates** for base/refined respectively. Higher intermediate precision does not resolve this study's acceptance failures.

### Boundary treatment is the material difference

The table reports reduction in RGB RMSE against the preserved native bilinear baseline, using the **unchanged policy interior scope**. Each neutral row averages the same four packed/uniform CFA cases. Positive percentages mean lower error. These are synthetic reductions, not perceived photographic improvements.

| Neutral-detail probe | Upstream base with engine border band | Original upstream base borders |
|---|---:|---:|
| 1/32-x sine | 45.62% | 99.82% |
| 1/32-y sine | 47.38% | 99.82% |
| 1/8-x sine | 42.05% | 91.67% |
| 1/8-y sine | 42.21% | 99.998% |

The one-pixel policy interior still includes much of our six/eight-pixel fallback band. Original upstream reconstruction continues to the true image edge and therefore preserves gains in those pixels. This is a concrete reason to investigate a specified true-boundary treatment in a future implementation. It also explains why equal central equations need not yield equal whole-image quality.

### Original upstream code still shows the chromatic tradeoff

| Implementation with original borders | Correctness failures | Regression score/scope checks | Improvement gates |
|---|---:|---:|---:|
| Upstream bilinear | 180 | 327 | 0/24 |
| Upstream Malvar | 22 | 295 | 16/24 |
| Upstream Menon base | 18 | 280 | 16/24 |
| Upstream Menon refined | 18 | 280 | 16/24 |

All eight chromatic improvement gates fail for Malvar and both original Menon modes. On the four packed/uniform chromatic hard-edge pairs, original Menon base RGB error is about **19.07–19.96% worse** than native bilinear; Malvar is **38.52–40.53% worse**. On the deliberately independently phased chromatic sines, Malvar error can exceed bilinear by much more. Conversely, neutral probes show substantial gains. These numbers characterize the declared subset, not the complete 64-metadata policy groups or typical photographs.

Original Menon boundaries improve neutral metrics but add regression failures versus the engine-border adapter. Border improvement alone therefore does not make an accepted replacement. The independent-channel counterexamples challenge the algorithms' cross-channel model; the fact that a library implements a published algorithm does not imply dominance over bilinear on this particular test set.

### Surrounding contracts require adaptation

All selected upstream Menon and Malvar outputs retain observed samples exactly after output conversion. Upstream bilinear fails our observed-fidelity condition in **160/164 cases**, due to its default reflected convolution of masked planes at true boundaries. An independently checked all-ones 5×5 RGGB input produces **`[2.25, 0.5, 0.25]`** at the top-left pixel. The observed red was 1. This is a specific boundary behavior, not an interior interpolation difference; directly replacing our baseline would violate its contract.

All four upstream modes raise `IndexError` on `(height,width)` shapes `(1,5)`, `(5,1)` and `(1,1)` after squeezing away singleton axes. Our engine must retain its explicit thin-image handling. Neither observation implies that the upstream package is unusable for its intended image dimensions; it establishes adaptations required by our broader source contract.

## Concrete C++ adaptation route

The useful engineering route is a faithful, separately versioned implementation of a selected established method. Preserve its existing output reference and license notices, specify the desired boundary/precision behavior, and verify the result against direct upstream output before judging quality.

For Menon, retain the posterior direction map and completed stages, use a composed radius-six/eight dependency plan, and compute upstream-like true-boundary extension against the actual active area. Internal tile/ROI edges must obtain neighboring source support, not reflect as image boundaries. Evaluate float64 intermediate buffers if upstream parity is the goal; retain float32 public/output buffers and the original bilinear identity. Specify thin-image fallback separately. Do not add the rejected sign guard or silently alter existing v1 research behavior.

For Malvar, its fixed radius-two filters and four geometry cases are a smaller numerical port. The current synthetic comparison gives no acceptance justification, and the existing patent hold remains open. Neither algorithm nor its dependencies has been admitted for product shipment by this review. Borrowing or translating BSD code requires preserving its notices; the isolated SciPy/Python runtime is not proposed as an engine dependency.

**Next decision:** obtain representative photographic evidence and explicitly review the provisional replacement contract against these established methods. Keep signed/observed-site/metadata/tile correctness separate from image-quality tradeoffs. Do not relax gates just to turn a rejection into a pass, or resume custom variant tuning without a demonstrated product benefit. Once a candidate and its contract are justified/admitted, port the fixed stages and validate full/ROI/tile, replay/cache/preview and native performance. The channel-independent invention proposal is superseded by this code-informed evaluation route.

## Evidence and verification

[Comparison tool](../../tools/raw_colour_code_review.py), [preserved report](../../tests/reference/raw/colour_code_review_v1.json.gz) and [readable index](../../tests/reference/raw/colour_code_review_v1.index.json) retain all 164 paired cases, exact source/helper/fixture bindings, original-border and adapter scores, central numerical/direction comparisons and shape controls.

- Report JSON SHA-256: `4b3a650411cc59dd7c97a26478f025b2eee0d962cb249f3cb36d5cfecc0cac30`.
- Gzip SHA-256: `848c74a01b5dbcad9a5260a5046bdc982cef77fea25a42becc2cdbfbbe2c24c5`.
- Tool LF SHA-256: `1e3f63a2b9b04b87d944c6213789ef393fbaa2d18e5a3e7bb54450f6f3defd6c`.

The full declared 164-case comparison completed on bundled Python 3.12.14. An independent array-based normalization and **untraced** direct invocation matched report hashes on **32 fixtures × four outputs**, confirmed exact Menon/Malvar observations, the constant-border control, unique case/variant counts and the direction-disagreement total. Archive decompression matches the report byte-for-byte. Earlier baseline, scalar/guard studies, policy and upstream cross-check hashes are unchanged. No native code was modified or rebuilt, so native suites were not repeated; the prior recorded full suites remain 20/20 default, 11/11 core-only and 12/12 LittleCMS.

Reproduce with a Python 3.12 interpreter, the recorded pinned sources and existing isolated research runtime:

```powershell
python tools/raw_colour_code_review.py build/research/colour-code-review-v1 build/research/menon/upstream-runtime tests/reference/raw/bilinear_baseline_v3.json.gz tests/reference/raw/menon_scalar_research_v1.json.gz build/research/colour-code-review-v1/paired-review-v1.json
```

The CLI verifies exact sources/dependencies, generated fixture identities and existing rendered hashes. Exit 0 means comparison completed; it does not imply replacement or shipment acceptance. Source snapshots/runtime remain ignored local research material. Base `e2eb724`; changes are local/uncommitted, with no commit or push.

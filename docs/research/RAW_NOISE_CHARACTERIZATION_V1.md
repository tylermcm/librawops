# Local RAW noise/detail characterization v1 — 2026-10-01

This is a bounded **diagnostic protocol**, frozen before collecting numerical camera results. It adds no denoise, camera noise model, automatic setting, acceptance threshold or engine/default change. The [implementation plan](../plan/LIBRAWOPS_PLAN.md) remains the authoritative handoff. Inputs are the three local Z7 II ISO 3200/3200/20000 captures and their retained decoded extraction; originals and generated buffers/images/reports remain ignored. Existing source identities, observed-site values, signed shadows/headroom and Menon base v1 are preserved.

## Fixed selection and domains

The [ROI plan](../../tests/reference/raw/camera_noise_rois_v1.json) fixes four native 256×256 regions per capture: diffuse background, skin, plain fabric and printed detail. Coordinates were selected from full previews and native crops before measuring statistics. They do not select on numerical candidate scores. These are scene regions, not certified flat fields; skin includes real texture/boundaries, the ISO 20000 skin crop includes facial detail, and backgrounds can contain real structure. Per-capture candidate pairs use identical sensor rectangles. Different framing/exposure/light prevents cross-image ISO response or camera-noise estimation.

Inspect immutable bilinear v1 and opt-in Menon base v1 with cache disabled/one session worker. Source-only camera-linear output independently verifies every observed photosite against per-site float32 normalization, including phase/origin. Numerical RGB diagnostics use **native calibrated scene-linear ProPhoto/D50**, after as-shot WB/zero exposure adjustment and the same decoder-derived diagnostic matrix used in the local camera study. The matrix is not a measured illuminant-dependent profile. Native 256/64 output tiles must produce exactly the same calibrated bytes. Statistics precede tone, encoding, clipping and reduction. No demosaic-error truth or perceptual quality score is available.

## Original diagnostic definitions

For each RGB plane, record signed/headroom counts, extrema, mean, population standard deviation and 1/50/99 percentiles. Define three **proxies in this explicit working RGB basis**:

```
L = (R + 2*G + B) / 4
Cr = R - G
Cb = B - G
H_r(P)[x,y] = P[x,y] - mean(P[x-r:x+r+1, y-r:y+r+1])
```

`L` is a green-weighted intensity proxy, not calibrated luminance; `Cr/Cb` are differences, not perceptual chroma coordinates. Both candidates use the same basis. Uniform RGB perturbations appear in L while equal/opposite R/B perturbations appear in Cr/Cb. These definitions help separate channel-coupled variation, without claiming that all variation is noise.

Compute `H_r` at radii **1 and 4**, including the center in the square mean. Every original/residual metric uses the common **8-pixel inset (240×240)**, so scales and candidates share the same centers. Valid box sums use float64 accumulation and no padding/reflection; only real ROI samples are used. Record residual mean, population standard deviation and RMS. Joint chroma residual RMS is `sqrt((RMS(H_r(Cr))² + RMS(H_r(Cb))²)/2)`. It contains noise, texture, aliases and model artifacts together; neither radius isolates a pure noise band. A higher value can mean more noise or retained texture, and a lower value can mean smoothing.

On each residual, calculate Pearson correlation for horizontal/vertical lags **1,2,4,8** over overlapping center pairs, separately demeaned. Return null if either paired standard deviation is <=1e-12 working units: a fixed numerical-degeneracy guard, not a quality cutoff. Original L first-difference RMS (horizontal/vertical, one sensor pixel) is an additional **texture-plus-noise** proxy. Ratios compare Menon/bilinear on the same ROI only; an exactly zero denominator yields null. No estimated SNR, ISO curve, detail-retention percentage or pass/fail threshold is inferred from these single photographs.

Raw-domain diagnostics separately split observed normalized photosites into four CFA site planes; statistics use site-grid pixels (two-sensor-pixel spacing) and the same 8-site inset. Keep the green sites distinct and bind pattern/phase/site color and site levels. These measurements describe the original source before reconstruction; their units/spacing differ from calibrated RGB and must not be compared as an amplification gain.

## Controls, inspection and provenance

Independent numerical controls cover constant/affine fields, a known impulse with explicit neighborhood sums, deterministic independent perturbations, slow versus alternating chromatic signals, uniform versus opponent perturbations, signed/headroom values, degenerate correlations and metadata/plan failures. They verify the diagnostic equations, not photographic denoise performance. Repeat camera runs must reproduce calibrated-buffer, metric and figure hashes on the same runtime.

Save calibrated float32 ROI buffers locally. Inspect native 1:1 encoded candidate crops using the existing shared diagnostic recipe, alongside radius-four opponent-residual views with a **fixed +/-0.02** working-unit range. Residual images use neutral gray at zero, red for Cr and blue for Cb; only these display images clip. An eight-pixel gray margin marks centers excluded from the statistics. Image settings are identical across candidates/captures, not independently stretched.

Reports bind original/decoded/extraction/plan, engine DLL/extension, source/helper and buffer/figure hashes, sensor metadata, recipes, Python/NumPy/Pillow versions, actual elapsed research stages and Windows process-memory counters. Output directories must be new and confined to the camera folder. Timing is research workflow cost, not isolated render benchmarking; preserve confounders and add relevant observations to the PERF log. Reproduce before selecting a noise treatment; broader rights/profile/corpus, controlled noise calibration, shipment and release gates remain separate.

## Results and continuation

The [research tool](../../tools/raw_noise_characterize.py) and [twelve numerical controls](../../tests/raw_noise_characterize_tests.py) implement the frozen definitions above. All twelve tests pass on the existing Python **3.9.13 / NumPy 1.26.4** engine research runtime and the separate existing Python **3.13** decoder research runtime. Ten earlier adapter/benchmark tests also pass. No package or scientific dependency was added to the engine. Native source/binaries were unchanged in this checkpoint; prior rebuilt home CTest **26/26, 16/16, 17/17** remains the latest engine matrix and was not repeated for optional diagnostic tooling.

Two complete local runs check **24 algorithm/ROI pairs** over twelve scene crops: **1,572,864 exact observed photosites**, calibrated tile-256/64 byte parity, and identical source/calibrated/encoded hashes, raw-site and RGB metrics, and all three figure hashes. Saved calibrated buffers independently reproduce every reported RGB metric. Numerical controls verify formulas against explicit neighborhood sums, not a second invocation alone. Report bytes differ because elapsed times and output paths are recorded; numerical/figure results are exact on the same runtime. Evidence and source/helper hashes bind the current frozen protocol/ROI plan and the current rebuilt engine DLL.

All three native 1:1 comparison boards were visually inspected. Skin/printed edges retain visibly sharper intensity structure with Menon, while its dark regions show distinctive fine/short structured grain. These crops refine the earlier impression of broadly stronger "chroma noise": **brightness variation, color-difference magnitude and spatial correlation behave differently**. In the explicit linear working-RGB proxies, Menon has lower radius-four joint Cr/Cb variation in every selected crop, even where grain is visually conspicuous. Neither a higher intensity gradient nor a lower color residual proves retained true detail or lower sensor noise.

| Capture / role | Menon/bilinear L first-difference RMS | Joint Cr/Cb residual ratio, r1 | Joint Cr/Cb residual ratio, r4 |
|---|---:|---:|---:|
| ISO 3200 `_DSC1788` background | 2.207 | 0.611 | 0.671 |
| ISO 3200 `_DSC1788` skin | 1.970 | 0.611 | 0.689 |
| ISO 3200 `_DSC1788` plain fabric | 2.168 | 0.600 | 0.672 |
| ISO 3200 `_DSC1788` printed detail | 1.789 | 0.601 | 0.683 |
| ISO 3200 `_DSC1790` background | 2.205 | 0.610 | 0.671 |
| ISO 3200 `_DSC1790` skin | 1.922 | 0.579 | 0.670 |
| ISO 3200 `_DSC1790` plain fabric | 2.181 | 0.592 | 0.673 |
| ISO 3200 `_DSC1790` printed detail | 1.642 | 0.580 | 0.667 |
| ISO 20000 `_DSC1793` background | 2.281 | 0.988 | 0.850 |
| ISO 20000 `_DSC1793` skin/face detail | 2.261 | 1.031 | 0.891 |
| ISO 20000 `_DSC1793` plain fabric | 2.312 | 0.963 | 0.829 |
| ISO 20000 `_DSC1793` printed detail | 2.205 | 0.969 | 0.843 |

The per-row ratios are **within one image/ROI**, not comparisons between ISO bands. Across the eight ISO 3200 crops, Cb radius-four horizontal lag-two correlation is **0.178–0.242 Menon**, versus **−0.001–0.049 bilinear**. In the four ISO 20000 crops it is **0.065–0.118 versus 0.036–0.048**. Lag-one correlation does not rise uniformly: Cr is lower with Menon throughout, and Cb lag one falls in the ISO 20000 crops. This supports reporting scale/channel structure separately instead of attributing the appearance to one noise-amplitude number or a proven classifier fault.

The fixed residual display range clips some view channels, particularly ISO 20000 skin: **40.5% Menon / 47.3% bilinear** of common-center Cr/Cb view channels exceed +/-0.02 there. The bound remains frozen; it was not stretched after seeing the results. The local verification summary records clipping fractions for each pair. Actual buffer values and all numerical statistics remain unbounded. Use the numerical values rather than saturated residual colors to compare magnitudes.

## Workflow cost and PERF pointers

On the home i9-14900K, two fresh-process workflows took **4.692 / 4.320 s**, including input checks, six immutable source-owning sessions, 24 paired diagnostics/exports and figure writing. Process peak working sets were **314.328 / 314.133 MiB**; these are whole-process counters, not isolated kernel/allocator peaks. Render/stats timing is observational research workflow cost, not a controlled latency regression or denoise benchmark.

In the first run, combined RGB statistics cost **1.348 s**, median **51.095 ms per algorithm/ROI**. Source/calibrated/encoded render-stage medians were **3.453 / 3.337 / 4.639 ms**; separate tile-64 parity cost **6.211 ms**. Six owned-session initializations totaled **1.888 s**. The statistics timing excludes separately computed raw-site diagnostics and includes many channel/proxy/scale/lag reductions. **PERF-006** tags `rgb_metrics`/`plane_metrics` as a research-tool cost to profile if the study scales; sharing reductions may help, but all frozen statistics must remain unchanged. **PERF-003** retains the source-ownership cost. No performance optimization or global thread-policy change was introduced here.

## Local evidence and reproduction

Generated reports, three boards and 24 signed float32 calibrated buffers per run remain ignored under `tests/rawfiles/_librawops_local/high-iso/`. The versioned JSON ROI plan is configuration only, with no photograph/pixel data. The reports bind the earlier high-ISO extraction SHA-256 `2dc3db13ac8d1fa004b3a11d67a8a17707eba780110337a6dd5eb879a8c06da8` and unchanged engine DLL `1b550536afddfa57ad4ebc96da37cc1e9831772f6657fc691dcb57cb671a470c`.

| Evidence path relative to the local high-ISO directory | SHA-256 |
|---|---|
| `noise-v1/noise-characterization-v1.json` | `0aae98e186ec7bca82a9b1c5953843b5ef8a418700f8c0349f55a576b5f638ff` |
| `noise-v1-repeat/noise-characterization-v1.json` | `ca9b14c671013b0a8bebc91ae1e6a0d6d7c569789c6aae19c8091ac01b515cff` |
| `noise-verification-v1.json` | `cdb56743247883e4710f5adc17d1e24f5d51c182de96783561a1e47f1d06d21b` |
| `noise-v1/_DSC1788-noise-detail.png` | `040fd3cec158e55018d57f1f282a3f0ee7418c08858673f77fcec186be401ab5` |
| `noise-v1/_DSC1790-noise-detail.png` | `e786bc7aa5fa37fb687e3b74af843bfda8fea9fe04276751ff6d0a0227cbb9fd` |
| `noise-v1/_DSC1793-noise-detail.png` | `48eeed65eadc2c3d0540017d04f855a09760cf322ca99d4ca48d169b8849371a` |

ROI plan SHA-256 is `28ff873d10a931fc5852e595852d6c64e878a0faaf1561f2a60952ce96a70019`. Earlier extraction/engine study, archived policies and image files are unchanged. Reproduce into a new confined directory:

```powershell
& 'C:\Users\tylle\anaconda3\python.exe' tests/raw_noise_characterize_tests.py
& 'C:\Users\tylle\anaconda3\python.exe' tools/raw_noise_characterize.py build-msvc-release tests/rawfiles tests/rawfiles/_librawops_local/high-iso/extraction-v1.json tests/reference/raw/camera_noise_rois_v1.json tests/rawfiles/_librawops_local/high-iso/noise-next
```

**Next bounded checkpoint:** freeze and evaluate a separate post-reconstruction noise-control experiment with independent intensity/color treatment, using known-noise flat/affine/edge/texture controls and these fixed camera pairs. Begin with existing box-filter mathematics as an explicit smoothing control; do not select an adaptive production algorithm from these single-photograph proxies alone. Define numerical domain, constants/strength/radius, true-border/halo behavior and native-versus-mip stage order before coding; preserve signed/headroom values and untouched original demosaic observations at the source boundary. Review algorithm provenance/admission before any native denoise operation, then evaluate detail/noise tradeoffs before a port. Menon v1, bilinear default and all earlier rejections remain unchanged. No additional camera files are required for this next bounded experiment. HEAD `4c7248a`; local tools/tests/docs/configuration, no commit or push.

**Completed follow-up:** the separately frozen [original smoothing control](RAW_NOISE_CONTROL_V1.md) now evaluates five intensity/color settings on 24 saved camera inputs and known-noise flat/affine/edge/texture truth. Two runs reproduce 120 camera outputs, 20 synthetic pairs and seven inspected boards; strong smoothing suppresses perturbations while damaging true edges/texture. Its camera scores use 232-square centers, rather than the 240-square centers here. These diagnostics/evidence remain unchanged. Next define a downstream NR graph/preview contract and compare a separate candidate when independent research is available; native/adaptive denoise remains unimplemented.

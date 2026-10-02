# Original intensity/color smoothing control v1 — 2026-10-01

This protocol is frozen before collecting results. It is an original, optional research control, not a selected adaptive denoiser or a native engine operation. The [authoritative handoff](../plan/LIBRAWOPS_PLAN.md) and [earlier noise diagnostics](RAW_NOISE_CHARACTERIZATION_V1.md) remain separate. No demosaic identity, source observation, default, engine dependency or release disposition changes.

## Frozen equations and settings

Input is finite native scene-linear ProPhoto/D50 RGB, float32, with absolute values <=8 and dimensions 1..1024 per axis. Convert to float64 for the following equations, preserving negative samples and headroom:

```
L = (R + 2*G + B)/4; Cr = R-G; Cb = B-G
L'  = L  + intensity_strength * (box_r(L)  - L)
Cr' = Cr + color_strength     * (box_r(Cr) - Cr)
Cb' = Cb + color_strength     * (box_r(Cb) - Cb)
G' = L' - (Cr'+Cb')/4; R' = G'+Cr'; B' = G'+Cb'
```

These are working-basis intensity/color **proxies**, not physical luminance or perceptual chroma. The square box includes its center, clips its support at the true input-image boundary, and divides by the actual number of available samples. No padding, reflection, clipping, sharpening or noise estimation occurs. Float64 summed-area reductions implement the existing box-mean mathematics; they are not claimed bit-identical to the engine's sequential box accumulation. Round reconstructed RGB once to float32. Both strengths zero bypass all equations and copy the exact input float32 bytes, including signed zero. Reject nonfinite/invalid inputs and unsupported settings before output.

The [configuration](../../tests/reference/raw/noise_control_v1.json) fixes five controls before scores: bypass; intensity-only radius-one strength 0.5; color-only radius-one strength 0.5; both radius-one strength 0.5; both radius-four strength 1. No score-driven tuning or production recommendation is made. The last preset deliberately illustrates aggressive detail loss.

## Borders, tiles and stage order

Radius is measured in native pixels. An interior tile needs the radius-wide input halo; trim the halo after filtering, with only true image edges using clipped support. Numerical partition error must be <=1e-6 working units on the bounded inputs, not silently reported as exact bytes. This is a research equivalence tolerance, not an eventual native acceptance contract. Tests compare against independent explicit neighborhood sums including singleton axes, corners, impulses and nonzero tile origins. Missing-halo counterexamples must show why independent tile-edge clipping is wrong.

The proposed experiment filters native calibrated RGB **before** direct 2x2/4x4 reduction and before tone/encoding. Synthetic checks measure the difference from filtering an already reduced image; these stages do not generally commute. The engine's current `BoxBlurNode` runs in reduced pixels for previews. This experiment does not add or change an engine preview anchor.

## Known truth and camera protocol

Four 96-square clean fixtures are flat, affine, sharp intensity/color edges, and sinusoidal/printed texture. Flat includes signed/headroom channel values. Freeze NumPy PCG64 seed 20261001, Gaussian proxy-noise standard deviations L=0.02, Cr=0.015, Cb=0.015, and float32 input quantization. Independent Gaussian proxy noise is an explicit synthetic test distribution, not a fitted camera-noise model. Generate each fixture's noisy input once, sharing it across all controls. Save clean/noisy inputs and processed clean/noisy buffers with hashes. On the common 8-pixel inset record RGB and L/Cr/Cb RMS error to clean truth, clean-filter bias, and filtered-noisy minus filtered-clean RMS; the last isolates the injected perturbation response. This separates known smoothing damage from removed perturbations. It does not certify real-camera quality.

For white independent flat proxy noise and an interior N-sample box, strength a has predicted variance factor `(1-a)^2 + (2*a-a*a)/N`. Check its coefficients independently with impulses and include predicted factors in the report. A finite single realization need not equal the ensemble prediction.

Reuse the 24 signed calibrated buffers from the frozen high-ISO study, verifying their report and buffer hashes. There is no RAW decode, reconstruction or original-file reread in this checkpoint. Filter each 256-square crop, then discard **four pixels on every side for every control including bypass**. The retained 248-square images have valid real-image support for every preset; crop borders are not passed off as actual sensor edges. Existing diagnostics on retained images use their additional eight-pixel inset, giving **232-square common centers**, twelve pixels inside the original ROI. New metrics therefore cannot be directly compared with the earlier 240-square-center scores. Camera scores remain texture-plus-noise proxies without clean truth. Reuse both demosaic baselines and all four fixed scene roles; no new ROI selection.

Display all five retained controls at native 1:1 through the existing native `render_raster` ProPhoto-to-sRGB-preview path, zero exposure and its unchanged default display recipe. Display clipping is separate from stored signed linear buffers. Bind display module/DLL and encoded hashes. Inspect six boards (three captures, two demosaics) plus a synthetic truth board. Use the same display settings for all columns.

## Evidence, provenance and continuation

New local output directories must be confined under `tests/rawfiles/` and must not exist. Reports bind configuration/protocol/tool/helper/test hashes, input report/buffers, runtime/hardware, native display binary, all output hashes, actual filter/statistics/display/workflow times and whole-process memory. Repeat in a fresh process and compare numerical results and output/figure hashes; exclude timings and paths from reproducibility claims. Record material costs with PERF IDs. Existing frozen evidence must not be overwritten.

Only original proxy transform equations and existing square-mean mathematics are implemented here, with no imported denoiser, weights, dataset or new dependency. This is research provenance, not patent/distribution clearance for a future production algorithm. Native implementation requires separately recorded algorithm/data/dependency admission and a native numerical/preview/tile contract. Next compare this measured control with the user's independent research before selecting an adaptive classical or learned candidate. Current photographs suffice for the control; they are not a training or certification corpus.

## Results — completed bounded control

The [original tool](../../tools/raw_noise_control.py) implements the frozen settings. [Twelve independent controls](../../tests/raw_noise_control_tests.py) pass on both existing Python/NumPy research runtimes. Neighborhood mathematics is checked against explicit Python `math.fsum` sums; impulse coefficients independently verify the ensemble variance formula. No native filtering operation or model was introduced.

Two fresh-process runs reproduce all **120 camera-control outputs**, **20 synthetic control pairs**, reported numerical metrics and **seven figures**. Local verification hashes 350 output assets across both runs and recomputes all 240 saved camera buffers and their metrics exactly. All 120 full-versus-64-tile camera comparisons have **maximum error 0** on these inputs; the declared general research tolerance remains 1e-6, not a universal bit-exact contract. The bypass displays match all **24** corresponding retained crops in the earlier boards exactly. All six camera boards and the synthetic truth board were inspected at native 1:1.

Known-truth interior RGB RMS results make the smoothing tradeoff explicit:

| Fixture | No treatment | Both half r1 | Both full r4 | Full-r4 clean-filter bias | Full-r4 injected perturbation response |
|---|---:|---:|---:|---:|---:|
| Flat | 0.022453 | 0.013010 | 0.002515 | 0 | 0.002515 |
| Affine | 0.022588 | 0.012959 | 0.002314 | ~1.5e-10 | 0.002314 |
| Sharp edges | 0.022385 | 0.015298 | 0.030410 | 0.030386 | 0.002477 |
| Texture | 0.022301 | 0.014411 | 0.028165 | 0.027946 | 0.002476 |

All values are linear working-RGB units on the same centers. The strong control suppresses the injected perturbation on every fixture but its edge/texture smoothing damage raises total error **above bypass**. Smaller settings trade less smoothing damage for a larger remaining perturbation. This is a transparent baseline, not selection of a production strength or algorithm. Camera noise is not assumed to follow this synthetic Gaussian distribution.

Across the 24 paired camera inputs, intensity-half-r1 has L gradient ratios **0.523–0.770** versus its own bypass and leaves the Cr/Cb radius-four proxy unchanged to float32 rounding. Color-half-r1 has Cr/Cb radius-four ratios **0.712–0.789** and leaves the L gradient proxy unchanged to rounding. Both-half-r1 combines those responses. Both-full-r4 lowers the gradient proxy to **0.044–0.421** and color residual to **0.115–0.176**, while visibly blurring facial, fabric and printed-edge structure. Lower camera scores are **not measured noise-removal percentages**: they include lost real structure, with no clean truth. Ratios pair each camera/demosaic/ROI with its own bypass; there is no cross-ISO noise curve.

Filtering before 4x4 reduction differs from filtering already reduced pixels: maximum synthetic discrepancy reaches **0.147460 working units** on the sharp-edge fixture under full-r4. Even flat truth with injected perturbations produces a different result. The current reduced `BoxBlurNode` therefore cannot silently stand in for a native NR preview stage.

## Workflow costs and local reproduction

On the i9-14900K, processing/analysis/display/export workflows took **8.807 / 8.382 s**, with setup-inclusive totals **8.912 / 8.480 s** and whole-process peak working sets **106.898 / 106.734 MiB**. This reuses small saved RGB crops and excludes decode, full RAW ownership/reconstruction and native NR; it is not comparable with earlier full-camera workflow memory or an engine performance target.

Across all 120 camera controls including 24 bypasses, median filter/statistics/display costs were **9.076 / 42.446 / 4.203 ms**; totals **1.058 / 4.658 / 0.449 s**. Filter median by preset: bypass **0.475 ms**, intensity-half-r1 **9.037 ms**, color-half-r1 **9.405 ms**, both-half-r1 **10.397 ms**, both-full-r4 **9.784 ms**. PERF-007 records this optional float64 proxy/summed-area/rounding cost and possible shared intermediates if the evaluation corpus grows. PERF-006 continues to track research statistics; different center counts/settings make this no controlled regression. Full-frame cost, allocator attribution and native NR remain unmeasured.

Reports/buffers/figures remain ignored under `tests/rawfiles/_librawops_local/high-iso/`. Each output folder retains the exact frozen protocol prefix in `protocol-frozen.md`; results appended here do not rewrite that snapshot. Configuration SHA-256 is `ff87429c11ce1d8f401f5d6e74771b56621e4b5d4f0e14356bec7a6d191cb0b2`.

| Local evidence relative to the high-ISO directory | SHA-256 |
|---|---|
| `control-v1/noise-control-v1.json` | `101c476dffd008334819d397357928354478e011ff6861dc7abbd4d0ff231ed0` |
| `control-v1-repeat/noise-control-v1.json` | `9d2fdc937751e967012388a028c29da70fe463c6f1f77b227a4769cf4cae0164` |
| `control-verification-v1.json` | `dc0bf2afc6894af0302bc01494d6370f77eace099fe2b289b6cf58e62e44842c` |
| `control-v1/synthetic-truth-control.png` | `f6d13848364b5a15d97a7ed9f3c2dcb671b0dd355e7f193116a21f943d4cb6e9` |
| `control-v1/_DSC1793-menon_base-smoothing-control.png` | `8664b20fb0e028144a39fa08e4640c0848d807cfd3e96c47f7f8a1f22e0bdd26` |

```powershell
& 'C:\Users\tylle\anaconda3\python.exe' tests/raw_noise_control_tests.py
& 'C:\Users\tylle\anaconda3\python.exe' tools/raw_noise_control.py build-msvc-release tests/rawfiles tests/rawfiles/_librawops_local/high-iso/noise-v1/noise-characterization-v1.json tests/rawfiles/_librawops_local/high-iso/control-next
```

Use a new output directory. Native engine sources/DLL, original policy files and earlier evidence remain unchanged. Latest full engine suites remain the previous 26/26, 16/16 and 17/17; not rerun for optional research-only code. Next define the optional downstream NR graph/preview contract and evaluate a separately identified candidate when the user's [independent research](NR_DEEP_RESEARCH_PROMPT.md) is available. No production denoiser selection, native port, package installation, commit or push occurred.

**Latest continuation:** the user supplied the research. Its [review/corrections](NR_RESEARCH_REVIEW_2026_10_01.md) and [camera NR architecture contract](NR_GRAPH_CONTRACT_V1.md) now prefer a separate manual-variance camera-linear candidate before WB. Existing stage boundaries and native-before-mip calibration are verified; no native NR is registered. Next freeze/evaluate exact local-shrinkage equations/support/identity with known-truth RGB/Bayer controls. This ProPhoto smoothing control, its frozen protocol snapshot and all recorded results remain unchanged.

# Review of supplied non-generative NR research — 2026-10-01

The user supplied “Deep Research Review: Non-Generative Noise Reduction for LibRawOps” after the [original smoothing baseline](RAW_NOISE_CONTROL_V1.md). Treat it as research input, not an implemented algorithm, independent quality evidence, or distribution approval. The original text is retained locally without modification at `tests/rawfiles/_librawops_local/high-iso/nr-contract-v1/user-research.txt`, SHA-256 `7743eb5ffe70d0c641b14f9ded160288e7aa1d01954f47ff692c369fcca771bd`. This reviewed summary and the [integration contract](NR_GRAPH_CONTRACT_V1.md) are repository documents; the single authoritative handoff remains [the build plan](../plan/LIBRAWOPS_PLAN.md).

## Direction to carry forward

Prefer a bounded classical **research candidate** in native, normalized camera-linear RGB immediately after unchanged demosaicing and before WB/exposure/color calibration. Evaluate bilinear and Menon independently. The earlier ProPhoto control remains an immutable evaluation baseline; it was never a production-domain decision. Keep automatic noise analysis and learned inference as later, separate milestones. Existing photographs are development/visual regression material, with no clean truth or calibrated sensor-noise model.

The report's “VACRS” is its author's working name for a constrained local variance/shrinkage design. It is not a verified published algorithm, novelty claim, frozen formula, or native backend. The [SciPy Wiener documentation](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.wiener.html) confirms a relevant established local-window/noise-power filter family; we use it as conceptual context and do not import or copy that implementation. [Brooks et al., Unprocessing Images for Learned Raw Denoising](https://arxiv.org/abs/1811.11127) supports modeling the camera processing/noise distribution for RAW learning. It does not prove that this proposed camera-linear classical candidate outperforms a working-space candidate.

| Report proposal | Disposition for LibRawOps |
|---|---|
| Camera-linear before WB/color conversion | Preferred first candidate domain; current source and WB are mechanically separable. Compare later rather than claiming universal superiority. |
| Manual effective post-demosaic variance first | Adopt for the next research protocol. Explicit units/provenance and separate demosaic validation; no ISO-only calibration claim. |
| Positive local mean plus gain in [0,1] | Useful camera-channel convex-range invariant; does not guarantee photographic truth or retain all low-SNR texture. |
| 7x7 kernel and three-pixel halo | Conditional only. Exact coefficients and every secondary dependency must be specified and composed. |
| Shared edge protection | Unspecified. Its formula, inputs, bounds and support must be frozen before coding. |
| Automatic/global noise estimator | Defer. Analyze once for a pinned upstream image, share immutable results; never infer a different global profile independently per tile. |
| Learned residual/noise-conditioned model | Later feasibility work requiring its own data, weights/runtime admission, graph support and validation. No pretrained model selected. |
| Proposed RMSE/MTF/latency percentages | Recommendations from the report, not adopted gates or existing project results. Freeze any future screen before seeing candidate scores. |

## Corrections and unresolved details

1. **Zero strength is not an identity in the printed formula.** With `g=(S²-k*N²)/(S²+epsilon)`, setting k=0 still gives g<1 whenever epsilon>0 and S² is finite. A future operation must explicitly return original float32 bytes at zero strength/disabled state. Noise-free channels also need a defined identity rule; do not depend on an epsilon-adjusted formula to supply it.
2. **A support claim needs a dependency proof.** Mean and second moment over raw input in 7x7 support need radius three. Computing a gradient of neighboring means adds a radius; filtering a variance field can add another radius-three support. A globally supplied analysis artifact is a separate dependency, not hidden per-tile work. No exact radius is adopted until the candidate's complete dependency graph is frozen.
3. **Local population variance is not a direct noise estimator after demosaicing.** Even independent constant-variance samples give expected noise contribution `sigma²*(1-sum(w_i²))` to weighted local variance, since the weighted mean is itself noisy. For correlated errors with covariance C it is `sum(w_i*C_ii)-w^T*C*w`. Bilinear/Menon produce spatial correlations and can mix real structure into these moments. Subtracting one supplied marginal variance is a heuristic requiring validation, not an exact separation of signal and noise.
4. **Convexity applies to the candidate's camera channels and its actual local input values.** It does not promise no overshoot relative to unknown clean truth, remove demosaic artifacts, prevent edge movement, or establish the same per-channel bound after arbitrary matrix mixing when each channel uses different gains. Independent intensity/opponent smoothing in the earlier control has a different invariant; do not transfer this claim to it.
5. **Border behavior is a processing decision.** The report suggests reflection; the earlier smoothing control clips/renormalizes real support. Choose and freeze a new candidate's actual-image border rule, including singleton axes, without rewriting either demosaicer or archived control.
6. **Existing scene crops do not measure pure camera-noise suppression.** Background/fabric still contain structure, and every original camera image has already been inspected. None can retroactively become an untouched holdout. New capture sessions are needed for a genuine held-out camera-quality/training evaluation; split scenes/bursts before crop extraction.
7. **The current engine is decoder-free.** The report's generic LibRaw-obligation language must not imply a new core dependency. Rawpy/LibRaw remains in an existing external, research-only unpack runtime; that arrangement gives no decoder distribution approval.
8. **The library needs a stable optional operation, not a permanent application postfilter.** Its current calibration anchor already forces its upstream camera path to native resolution before calibrated float32 averaging. This permits a future pre-WB NR node without moving reduction or altering Menon. It does not mean the node is implemented today.

## Scoped primary-source checks

Checked on 2026-10-01; this is a scoped check, not a complete audit of all repositories, datasets, weights, binaries or claims named in the report.

| Primary source | Verified scope / consequence |
|---|---|
| [BM3D authors' software page](https://webpages.tuni.fi/foi/GCF-BM3D/) | Official v4.0.3 distributions carry non-profit education/research restrictions and prohibit unauthorized industrial/profit-oriented use. Retain literature context; no download, import or distribution approval. |
| [ONNX Runtime v1.30.0 release](https://github.com/microsoft/onnxruntime/releases/tag/v1.30.0) | Official `latest` resolves to this tag in the current check, consistent with the supplied version claim. No binary/provider/license audit or runtime selection occurred. |
| [DirectML execution-provider documentation](https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html) | Confirms sustained engineering/new Windows development moving to WinML, stated DirectML 1.15.2/opset-20 support limits, sequential execution/disabled memory-pattern requirements and no concurrent Run on one DirectML session. A future provider adapter must respect its actual pinned runtime constraints. |
| [Unprocessing paper](https://arxiv.org/abs/1811.11127) | Supports realistic camera-pipeline modeling for learned RAW denoising; no released code/weights or dataset admitted. |

Other source/weight/dataset/patent statements remain leads requiring exact-artifact review. Original implementation and a permissive source license are not a freedom-to-operate conclusion. No legal-clearance status was changed, third-party denoiser downloaded, package installed, or model admitted. The CVF landing page returned 403; the primary arXiv paper record was available, so the review continued without depending on that unavailable page.

## Current-engine feasibility evidence

An ignored read-only probe uses `_DSC1793`, the fixed background/skin ROIs, both existing demosaicers and the same diagnostic recipe. It renders source/WB/zero-exposure/calibration taps through dependency-closed manifests; no NR node or changed native code is involved.

- Four case checks preserve **262,144** independently normalized observed photosites and exact source tile-256/64 bytes.
- WB output equals independent float32 per-channel gain multiplication; zero exposure is byte-identical. Source and WB differ on all four cases, proving distinct usable stages.
- Eight mip-1/2 calibrated outputs equal direct native calibrated 2x2/4x4 float32 averages exactly.
- All four attempts to execute proposed, unregistered `rawengine.camera_noise_reduce` reject as unknown. Documentation does not silently create a runtime feature.

Local report: `tests/rawfiles/_librawops_local/high-iso/nr-contract-v1/nr-stage-feasibility-v1.json`, SHA-256 `87d6d05be358a8da54ab8fe3a7cf7d838217fd14018bc39377604e5c2a4a24be`. It binds user text, extraction/decoded samples/ROI plan, native binaries, six engine source hashes, recipe, runtime and stage output hashes. The workflow took **0.734 s** after input checks, including two source-owning initializations **291.507 / 291.136 ms**, consistent with the existing PERF-003 ownership observation. Whole-process peak working set **301.117 MiB** includes mapped/owned RAW and runtime memory; no native NR throughput, new regression or allocator root cause is measured. No new PERF ID is needed for this existing cost.

## Next bounded checkpoint

Freeze a separately identified optional **camera-linear manual-variance local-shrinkage research protocol**, including exact kernel/edge rule/strength/numerics/borders/full support, then record algorithm provenance/admission appropriate to the implementation scope before coding. Use known-truth RGB and sensor-mosaic Gaussian/signal-dependent controls through both demosaicers, measure removed residual/detail bias and reuse fixed camera crops only for observational checks. Keep the earlier control's equations/scores immutable and avoid direct domain/center-count comparisons without a common downstream tap. An adaptive candidate must earn continuation with predeclared evidence; native graph integration, automatic estimation, learned training and release remain later steps.

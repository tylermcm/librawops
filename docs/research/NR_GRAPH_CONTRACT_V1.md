# Optional camera noise-reduction graph contract v1 — 2026-10-01

**Status: Architecture.** This document defines the integration requirements for future NR, following the [supplied research review](NR_RESEARCH_REVIEW_2026_10_01.md). It is not a registered operation, frozen candidate algorithm, manifest-format migration or native implementation. The proposed `rawengine.camera_noise_reduce` name is unregistered and current enabled execution rejects it. The [authoritative plan](../plan/LIBRAWOPS_PLAN.md) remains the continuation point.

## Placement, domains and unchanged source identity

For the first research candidate, require this explicit native RAW path:

```
immutable decoded RAW + existing normalization/demosaic
  -> optional camera_noise_reduce
  -> existing white_balance
  -> existing camera-linear exposure
  -> existing camera_to_working calibration/preview reduction anchor
  -> scene-linear edits
  -> tone/display/output
```

Input/output descriptors are `ImageDescriptor::camera_linear()`; output extent/origin is the same active sensor rectangle as its input. Strength/variance have units before WB, exposure and camera calibration. NR must not secretly change CFA samples, normalization, active area, demosaic identity or canonical source fingerprint. Original observed-site guarantees apply at `RawUnpackNode`, before the intentional downstream NR modification. Keep bilinear v1 default and Menon v1 opt-in. RAW source fingerprints describe original decoded samples/metadata; denoise identity belongs in operation state and upstream graph signatures.

A camera-linear descriptor alone does not prove that WB/exposure has not happened. The first candidate protocol must validate the upstream path/placement and noise units; accepting a mislabeled post-WB variance profile merely because its descriptor still says camera-linear is incorrect. Direct working-space/raster NR is a different future operation contract, not an implicit reinterpretation of the same input. The ProPhoto smoothing baseline remains independent research evidence.

## Native execution and exact preview stage order

The first camera NR node supports native final input/output only. It need not independently claim reduced support. `CameraToWorkingNode::supports_level` checks native support upstream, `input_level` maps preview work to native, and `render_level` calls native calibration before direct 2x2/4x4 averaging. Inserting a native NR node before WB therefore allows the existing anchor to execute NR at full sensor resolution for mip-1/2 preview.

Canonical preview is **reduce(calibrate(expose(WB(NR(demosaic)))))**, including each existing float32 rounding stage. It is not NR of already reduced RGB. Keep original sensor coordinates/active-area origin upstream and zero-based reduced output coordinates downstream. Odd last cells average actual sample counts; geometry after the anchor keeps its existing level/bounds mapping. Source planning maps reduced requests to their native footprints, expands by complete NR support, then composes the demosaic halo and clips to the active sensor boundary.

No approximate preview path is enabled by this contract. A later approximation needs its own explicit quality/identity, separately measured error against canonical preview/export and persistence/cache policy. The supplied one-display-code-value suggestion is not an adopted acceptance threshold. Do not replace the anchor with current reduced `BoxBlurNode` behavior.

## Versioned operation and analysis state

Use the existing operation schema/processing-version mechanisms when eventually implemented. Exact parameters and family name require a candidate-specific numerical contract before registration; illustrative fields below are requirements, not executable JSON:

| State | Contract |
|---|---|
| Algorithm identity | Explicit admitted algorithm ID and immutable processing version; kernel, border, expression order and edge rule are part of that version. |
| Strength | Finite, bounded range frozen per candidate; zero means exact original output, without filtering/analysis/model execution. |
| Manual noise input | Explicit finite nonnegative effective post-demosaic variance per camera channel, in normalized pre-WB sample squared units. Distinguish variance from standard deviation. No implied ISO-to-variance curve. |
| Provenance | State whether values are manual, synthetic or calibrated; record calibration/estimator schema, capture/source/demosaic identity as applicable. A manual slider is not a measured camera profile. |
| Automatic analysis, later | Immutable, separately versioned artifact bound to the exact upstream stage, normalization/demosaic and analysis settings; algorithm must not infer a separate global profile per tile. Unknown/no-profile behavior must be explicit, without hidden guesses. |
| Learned assets, later | Exact model/weight and external-tensor hashes, normalization/noise-map schema, supported graph operators/support/stride, runtime/provider/precision policy and admission record. Do not persist machine-local paths as model identity. |

Disabled same-domain operations reuse the upstream node under current graph semantics. An enabled zero-strength operation must explicitly bypass filtering and preserve original float32 bits, including signed zero; validate structural parameters/algorithm availability before executing any enabled operation. All-zero manual variance must also have a defined exact identity rule. Changing a strength/profile is an edit, not a source replacement or silent mutation of algorithm version. Old manifests gain no implicit NR and retain their original rendering. Enabled unsupported versions/algorithms/assets reject; no guessed substitution.

When a candidate is admitted, add its known-parameter validation, descriptor/path constraints and construction to `EditGraph.cpp`, and expose explicit optional RawSession recipe settings in `RawEnginePython.cpp` with saved-manifest/job/history coverage. No parser, registration, binding or build file is changed in this architecture checkpoint.

## Cache, lifetime and dependency requirements

Existing operation cache signatures include canonical operation identity plus upstream signatures; level/quality and tile bounds remain part of tile keys. Therefore NR output state must include all pixel-affecting settings/profile identities, with the upstream source record retaining demosaic selection. WB/exposure/tone changes downstream should reuse unchanged NR tiles rather than trigger re-analysis of a pre-WB input. A change to the upstream samples, normalization, demosaic, manual variance, candidate version or analysis artifact must invalidate affected results.

Bind learned runtime/provider/precision differences to runtime cache identity where they affect pixels; exact placement in operation versus backend namespace is a later adapter decision. Existing cache is process-local, so a runtime-version field is not a new requirement for every current scalar graph. Persist the processing/asset contract needed for eventual saved-edit reproducibility; do not confuse a process-local cache with a release bundle/SBOM or reproducible inference stack.

Own immutable analysis/model snapshots through request/job/history lifetimes. Global analysis is a separate bounded/cancellable task with declared full-image dependency; `required_source_regions()` for the local render cannot conceal it. Concurrent requests must never see a partially updated profile or shared mutable scratch. Unknown analysis must not silently produce different profiles for viewport/export.

## Numerics, borders and support

Preserve finite signed shadow samples and highlight headroom. No hidden [0,1] clamp, transfer function, output sharpening or false claim that non-generative guarantees truthful detail. Freeze supported input magnitude, intermediate precision, float32 rounding, nonfinite/overflow behavior and cancellation/error propagation before implementation. A candidate may reject out-of-contract data; it must not silently clip it into a supported range.

For the proposed local mean/shrinkage family, positive normalized weights and per-channel gains in [0,1] can bound output by actual local input camera-channel extrema. Check this property in the exact implementation, including float32 rounding and borders; do not extend it to unknown clean truth or arbitrary downstream channel mixing. Define constants as fixed points and exact bypass independently of epsilon regularization. Define roundoff treatment for slightly negative moment-derived variance rather than allowing an invalid square root/division.

A complete candidate-specific support proof is required. A sole 7x7 raw-input mean/second moment has radius three; secondary smoothing, gradients of neighboring statistics or inference preprocessing may enlarge it. Compose NR support with the demosaic halo when requesting original photosites. Request real upstream halo, filter it, and commit only the requested core. Artificial tile boundaries never receive true-image border treatment. Candidate border mode must explicitly cover actual active edges, singleton axes and nonzero origins; it may differ from earlier immutable algorithms.

For any later pixel-unshuffle/strided model, prove phase/alignment/padding dependencies against original image coordinates and the actual exported graph; a receptive-field width alone does not establish tile equivalence. Global pooling/attention/normalization requires an explicit global dependency and a shared artifact or a separate unsupported/tiled policy.

## Memory, cancellation and performance

Bound internal blocks and scratch independently of the caller's requested rectangle; no full-frame intermediary merely because a viewport tile maps to native pixels. Account for simultaneous active requests, halo amplification, scratch precision, model buffers and analysis storage. Keep the existing cache pixel-byte budget distinct from transient allocations. Reuse scratch only after profiling proves value and concurrency/failure behavior is validated.

Current rendering polls cancellation between output tiles. The first CPU implementation must at least retain this guarantee and cap each indivisible block; finer cancellation inside NR needs an explicit implementation contract, not a documentation claim. Model inference needs its own provider-safe termination/queue policy. Record ownership/kernel/analysis/cache/display costs separately with PERF IDs and avoid nested parallel oversubscription.

The report's +25% preview/+100% final-render recommendations are not adopted budgets or measured capability. Establish comparable no-NR and candidate stage/full-preview/native-export measurements on declared reference hardware before setting a continuation budget. Current saved-RGB smoothing and stage-boundary probes do not measure a future NR implementation.

## Ordered next checkpoints

1. Freeze candidate research equations: exact positive kernel, local variance/noise rule, shared edge protection, identity, strength, numerical/border/support contract, and independent provenance review. Start with manual effective variance; automatic estimation and model training are separate.
2. Evaluate an optional original reference outside the native engine. Use known-truth signed/headroom/flat/ramp/edge/texture fixtures, multiple fixed noise seeds/families, and a Bayer-noise route through both immutable demosaicers. Show removed residual, truth error, clean-filter damage, texture modulation and randomized-tile/source-phase equivalence. Freeze any gates before scores and label synthetic versus camera evidence.
3. Inspect fixed camera pairs through identical downstream WB/calibration/display. Existing images are development data, not untouched holdouts; require new controlled scenes/calibration for later camera/model claims. Keep baseline archives immutable and compare domains at a common downstream tap.
4. Only after candidate continuation/admission, port under a new optional native operation and verify parser/cache/history/jobs, footprint composition, exact bypass, true borders/concurrency/cancellation, and canonical preview/export averaging. Run required default/core/LCMS checks and profiling appropriate to that port.
5. Later add separately validated global noise analysis and/or a custom trained model. Audit exact data/code/weights/runtime/provider artifacts and record release disposition independently of implementation success.

No new camera captures are required to freeze the first manual-variance protocol. The classical method's efficacy and admission remain open; there is no native NR operation, automatic camera model or selected learned backend yet.

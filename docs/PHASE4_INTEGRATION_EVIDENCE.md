# Phase 4 integration and evidence audit

This is an evidence inventory under the [authoritative plan](plan/LIBRAWOPS_PLAN.md),
not a separate handoff. Current library foundations are opt-in. Production
photographic/profile/corpus, cross-runtime, release and broad Phase 4 signoff
remain open. The local testing UI remains outside the installed library.

## Current binding boundary

The executable graph registry currently contains **36 types**. The new
`tests/edit_control_binding_tests.cpp` covers **20 bounded controls** in both
working spaces, each at native Final and mip1/2 Preview: **120 bit-exact direct
C++ versus saved/parsed graph comparisons**,120 exact native source-footprint
comparisons and120 warm-cache replays. It also checks40 canonical serialization
round trips and160 enabled/disabled extra-parameter/schema rejections. These
are selected fixtures, not exhaustive parameter-space proof. Dedicated tests
still provide independent mathematical truth; direct-versus-graph replay tests
the binding boundary rather than reimplementing each equation.

| Group | Covered saved types (`rawengine.` prefix) | Domain and level rule | Existing objective evidence |
| --- | --- | --- | --- |
| Tone/color | `curves`, `levels`, `saturation`, `vibrance`, `channel_mixer`, `lut1d`, `lut3d`, `color_mixer`, `color_balance`, `grayscale`, `grading`, `lut1d_large`, `lut3d_large`, `dehaze` | Scene-linear ProPhoto/D50 or Rec.2020/D65;map requested-level float32 inputs | ToneOps C++/Python equation,extreme/identity,RAW/analysis/jobs/cache/history tests;PERF-012 through023 and026 |
| Spatial | `clarity`, `texture`, `sharpen` | Same working RGB;requested-level neighborhoods and true-boundary staged halos | Spatial C++/Python rational/kernel/halo/partition/support/RAW/history tests;PERF-024/025/027 |
| Geometry | `rotate`, `projective`, `cubic_resize` | Same working RGB;native reconstruction before direct output mip1/2 | Geometry C++/Python independent rational and frozen replay,resource/uint32/native-camera/install tests;PERF-030/031/032 |

All20 use schema1/processing2. Geometry and neighborhood controls copy explicit
native bounds/settings; point controls preserve their descriptor. Native and
requested-level source coordinates differ deliberately: source-footprint
inspection resolves either to native source coordinates through `input_level`.
Cubic resize alone adds explicit disabled relative-canvas/input-domain admission;
other disabled controls retain their existing validated passthrough/domain rules.
No change to those rules is inferred from this audit.

The audit found and corrected saved factory narrowing for three float64 controls:
rotation angle, saturation amount and vibrance amount. Focused regressions fail
against archived old native libraries and pass current libraries. Remaining
current double settings reach typed constructors through `number`/typed settings
parsers; the twenty-control replay test protects these boundaries. Global
`scalar` deliberately returns float for earlier APIs that declare float controls,
including white-balance/exposure/tone and linear mix. Their existing versions
and arithmetic remain unchanged.

## Other registered pipeline types

`rawengine.box_blur`, `rawengine.camera_to_working`, `rawengine.convolution`, `rawengine.crop`, `rawengine.exposure`, `rawengine.icc_display`, `rawengine.legacy.fixed_chain`, `rawengine.linear_mix`, `rawengine.orientation`, `rawengine.output_clip`, `rawengine.resize`, `rawengine.srgb_encode`, `rawengine.tone_curve`, `rawengine.white_balance`, `rawengine.working_space_convert`, `rawengine.working_to_srgb`.

These retain dedicated core/edit-graph/spatial/RAW/preview/manifest/history/
multi-source/ICC tests. Camera calibration, working-space conversion, explicit
output transfer/clip, legacy migration and ICC resource identity are distinct
pipeline gates; they are not claimed as covered by the new20-control comparison.
Legacy ICC/native-only/reduced-level limits remain as declared by their APIs.
Cube file import is an input adapter for the versioned LUT types, not an extra
registered operation. File bounds/grammar/ownership/notices/install tests remain
separate. Current registry/source/test hashes are pinned by
`build-msvc-release/research/phase4-integration-inventory-v1.json`.

## Remaining plan gates

- Representative/profile-aware photographic quality: the existing corpus still
  has zero formally admitted captures and seeks at least three camera models.
  The eleven local Nikon inputs provide engineering diagnostics. Personal taste
  and preferred edits are not requirements; Adobe comparisons are a separate
  explicitly selected target.
- Production highlights/shadows/whites/blacks, broader curves/HSL/selective
  controls, capture/creative sharpening/raster denoise and general transforms
  retain their open plan requirements. Bounded foundations do not close them.
- Controlled stage/full-frame/concurrency/allocator measurements remain open.
  Current camera API costs include graph/cache/copies, with overlapping work
  confounding causal regression claims. PERF-032's repeated cubic horizontal
  row work is a concrete next profiling candidate before any optimization.
- Broad shipped-control integration, numeric/image/schema/performance and
  Phase4 quality/documentation signoff stay open until the selected shipped
  subset and remaining declared gates are complete. This audit adds evidence
  without redefining the release subset or changing checklist counts.

Camera NR remains deferred until specifically resumed. No UI, dependency,
default, commit or push change accompanies this audit.

## Verification checkpoint - 2026-10-03

Inventoried all 36 registered types and added a common saved-control binding gate for 20 bounded controls in both working spaces at native Final/mip1/2 Preview:120 exact direct/saved outputs,120 native source footprints,120 warm-cache replays,40 canonical round trips and160 enabled/disabled schema/extra-parameter rejections. Current full default69/69,core37/37,LittleCMS38/38 pass. Library source/native,point-camera reports and installed consumers remain bound to the verified precision correction. Broad Phase4 production/corpus/profile/performance/release gates remain open;142 checked/138 open,HEAD85c989d,no UI/commit/push,camera NR deferred.

Full test logs are copied into immutable research/control-binding-full-logs-v1 before future focused CTest runs. The earlier precision-correction default68 LastTest.log was overwritten by a focused run before archival;its recorded hash/result survives but the physical full log is unavailable. Exact previous core36/LCMS37 logs were preserved separately. The current complete69/37/38 logs supersede that diagnostic limitation. PERF-028 now requires full-log snapshots before focused reruns. New test development corrected local C++ declaration/default-argument mistakes and initially passed native rather than upstream-level bounds to a direct source-footprint query;the corrected fixture follows input_level and native source-region conversion. No library change was needed for these test fixes. All current kernel/source/native hashes match the point-precision audit;only the added test target/test source and evidence documentation changed. See [the integration ledger](../PHASE4_INTEGRATION_EVIDENCE.md) and control-binding-final-verification-v1.json.

## Controlled cubic performance checkpoint - 2026-10-03

Measured and reduced PERF-032 cubic work without changing frozen equations or source support. Validated identity blocks now move/rebase the fetched tile;nonidentity vertical accumulation reuses the center/consecutive replicated horizontal rows with scalar storage while retaining every ordered logical tap. Sequential old/new/new/old benchmark:36 selected synthetic cases,1008 exact repeats and unchanged source requests/payload/caps;median paired speedups identity5.1019x,enlarge1.1431x,shrink1.0436x. All414 frozen channel maps/276 tile-ROI/12 identities/54 constants/27 extreme-zero-subnormal maps exact. All108 camera references/partitions,12 historical inputs and12 already inspected boards byte-identical. Full default69/core37/LittleCMS38 and fresh installed C++/Python consumers pass. Graph3300/7437/159;142 checked/138 open,HEAD85c989d,no UI/commit/push,camera NR deferred.

The serial consumer owns a1025x769 deterministic signed/headroom raster in both working spaces. Identity/enlarge(1.5x)/shrink(approximately4x),center/true-edge,native Final/mip1/2 Preview use up-to128-square output ROIs. Seven repeats follow one warmup;old/new/new/old ordering reduces timing-order confounding but is not a confidence interval. Median aggregate before/after(ms):identity1.583325/0.350150,enlarge4.793400/4.178050,shrink34.332200/32.969975;paired per-case speedup medians are distinct statistics. Initial source-copy fractions1.59%/0.56%/3.22%;remaining time includes support scans,tap generation,complete finite validation,reconstruction,reduction,allocations/copy and instrumentation,not an isolated kernel. No horizontal intermediate plane or new allocation/cache was introduced;identity removes native allocation by transferring the validated source tile. Source/native logical ceiling264588 bytes and fixed axis tables unchanged,with only scalar row/value reuse. Camera API5.114600/0.320250ms under overlapping test work is diagnostic,not a paired camera speedup. Camera workflow22.7866182s,peak working set377544704/commit1080373248 bytes include source/cache/research arrays/boards. New native replay helper originally compared the status header to the pre-native contract;comparison now excludes only Status and appended checkpoint metadata,confirming the normative body is unchanged. No engine change was made for that tooling failure. Graphify Python-module invocation was unavailable;the installed graphify.exe CLI succeeded.

## Bounded spatial stage baseline - 2026-10-03

Completed a bounded serial stage baseline for clarity/texture/sharpen under PERF-024/025/027:144 selected configurations in both working spaces,native Final/mip1/2 Preview,interior/true-edge128-square ROIs,amount0/.5 and low/mid/max radius or scale. All1008 repeated outputs exact;one input fetch per case equals declared support and payload. Source allocation/copy/reduction timed separately from remaining node work. Logical pixel/vector estimates are derived and bounded for these requests,not allocator tracing or full-frame/concurrency qualification. Engine/native/current69/37/38 evidence unchanged;142 checked/138 open,HEAD85c989d,no UI/commit/push,camera NR deferred.

The profile separates source reduction/copy from remaining node work and records per-repeat clocks and logical vector estimates. Dedicated arithmetic/ROI/invalid-input/replay tests remain the independent correctness gates. Full-frame,allocator/concurrency/cancellation attribution and profile-aware photographic qualification remain open. Sharpen evidence is tagged PERF-027;the earlier ledger reference to PERF-029 was corrected.

## Selected simultaneous spatial renders - 2026-10-03

Added selected simultaneous-render evidence for current clarity/texture/sharpen:36 serial/2-worker/4-worker batches on a shared immutable source/control,both spaces/native Final/mip2 Preview,max radius/scale,true-edge128-square requests. All588 outputs exactly match serial references. Whole-process peak working set19210240/commit14409728 bytes measured;these include owned raster/thread/runtime/output and do not isolate scratch allocations. PERF-024/025/027 records batch/throughput data without changing engine scheduling. Current native/full69/37/38/install evidence unchanged;142 checked/138 open,HEAD85c989d,no UI/commit/push,camera NR deferred.

The direct consumer leaves graph/cache/TileScheduler/cancellation and allocator attribution as separate open gates. Per-request scratch/caller/source budgets multiply with simultaneous requests;the measured lifetime process peak is not a scratch cap.

## Paused native sharpen checkpoint - 2026-10-03

Paused at the user request on2026-10-03. Current native includes validated cubic identity/scalar row reuse and sharpen shared RGB traversal. Latest full Release69/69 default(177.60s),37/37 core(162.39s),38/38 LittleCMS(162.59s) pass;immutable full logs saved in sharpen-traversal-paused-full-logs-v1. Current sharpening38880 frozen channel maps/1080 tiles/9450 constant channels pass exactly;all144 camera outputs,12 historical inputs and12 already inspected board hashes match previous native. Fresh installed sharpening/point/cubic C++ and sharpening Python consumers passed. Sharpen prototype48 fixtures/1344 comparisons showed radius1/2/3 median1.0600x/1.0861x/1.1626x speedups;these are prototype results only. Final native paired timing and final sharpen audit have NOT been run.142 checked/138 open,HEAD85c989d,no UI/commit/push,camera NR deferred.

Resume by completing the pending sharpen native performance checkpoint:run research/run-sharpen-stage-paired.py sequentially after competing tests finish,inspect the144-case/4032-repeat old/new/new/old output/source equality and measured speedups,then run research/record-sharpen-traversal.py for final audit/document bindings. The latest full69/37/38 pass and fresh install are already complete;do not rerun them solely to collect timing or overwrite immutable paused logs. If timing does not justify the optimization,assess/revert only the sharpen traversal against before-sharpen-traversal and reverify;preserve cubic/other prior work. Then continue the documented bounded clarity finite-check proof/prototype under Phase4 performance gates. Broad profile/corpus/allocator/cancellation/release gates open,camera NR deferred,no UI/commit/push.

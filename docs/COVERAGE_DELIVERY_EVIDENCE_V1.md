# Scalar coverage delivery and Python graph-session acceptance

2026-10-04 bounded acceptance, worktree based on HEAD `5792d22`.
[Frozen contract](COVERAGE_DELIVERY_CONTRACT_V1.md). This completes scalar tile
streaming/image assembly, shared RGB/scalar scheduling and Python coverage
source/session/history/job integration. Accepted mask graph/source/alpha arithmetic
and RGB/RAW/ICC defaults remain unchanged. Brush/local/RGBA/assets remain required.

## Native delivery and queue

[CoverageDelivery.hpp](../CoverageDelivery.hpp) and
[CoverageDelivery.cpp](../CoverageDelivery.cpp) expose CoverageRenderer with
row-major callbacks, validated actual scalar payloads, owned image assembly and
cooperative cancellation. Native Final/Preview and mip-1/2 Preview use the
CoverageNode's declared extent. Empty contained viewports yield no samples.
Request checks reject unsupported levels, invalid extents/ROI/tile size and
impossible materialized storage before rendering.

TileScheduler adds typed coverage futures sharing the existing worker pool,
pending budget, priority/FIFO policy and supersession namespace. A group can
replace either RGB or scalar requests. Native tests use promise gates rather
than sleeps to prove mixed priority/FIFO, shared full-queue admission, replacement
at a full budget, both queued payload replacement directions, active RGB/scalar
cancellation, invalid replacement isolation, source-fault recovery and queued
job cancellation during scheduler destruction. Running work completes before
destruction returns. The injected bad_alloc is a source-boundary exception,
not a claim of actual allocator-failure coverage or cancellation latency.

[Native tests](../tests/coverage_delivery_tests.cpp) also pin accepted frozen
mask-chain bits at native/mip-1/mip-2, native Preview, 1/2/8 tile partitions,
every single-pixel ROI, shifted native source origins, streaming assembly,
empty requests, actual wrong ROI/storage/nonfinite/out-of-range rejection,
callback exceptions and cancellation before/between/after the last tile.

## Explicit Python types and ownership

RasterGraphSession accepts mixed RGB and coverage source specs with stable UUIDs.
Coverage specs require coverage/width/height, with optional native x/y and
row_stride_samples. Raw bytes/native float32 typed buffers are copied under the
GIL before native canonical admission; noncontiguous, wrong typed/endian buffers,
bad dimensions/stride and color/profile fields are rejected. Padding is ignored,
visible values are packed and immutable, and source_info returns copied scalar
identity/extent metadata. Coverage exports use format4; RGB-only defaults stay
format2. Replacement fully validates before atomically publishing a source ID.
Saved fingerprints prevent rendering a mixture of old/new source snapshots.

Explicit scalar graph methods: render_coverage_manifest,
submit_coverage_manifest and submit_coverage_manifest_latest. Existing RGB
methods reject scalar outputs; scalar methods reject RGB. Results contain
width/height and owned packed native float32 bytes, one sample per pixel.
RenderJob output_kind/done/result/cancel/progress and timeouts apply to typed
futures. Output progress excludes internal mask/source visits.

Core Python history adds render_coverage, submit_coverage,
submit_coverage_latest and compare_coverage. RGB/scalar revisions retain
commit/undo/redo/save/restore and pinned source identity/lifetime semantics.
Opposite-kind render/submit/compare and scalar RGB-analysis requests fail closed.

[Six standard-library Python tests](../tests/python_coverage_delivery_tests.py)
compare frozen chain bytes in both working spaces and all supported levels,
tile partitions and single-pixel scalar ROIs. They independently compute a
canonical padded/origin source SHA256; check ctypes/native/raw bytes and endian
admission, caller mutation isolation, metadata copies, cache reuse/clear,
mixed RGB output/source footprints, strict payload/request guards, repeated job
results/progress, cancellation, atomic replacement failure, old job/history
pinning, restore identity guards, parent close/owner destruction, history close
and concurrent whole-source replacement/render snapshots. Selected Python
workers exercise scalar jobs at native/mip-1/mip-2. No large-scale concurrency,
allocator attribution or photographic quality claim is made.

## Reproducible evidence

Full Release suites pass: **84 default / 47 core / 48 LittleCMS /
85 LittleCMS+Python**. Exact source/native/Python/log bindings:
`build-msvc-release/research/coverage-delivery-full-v1/verification.json`,
SHA256 `bfadc7ef6617cb43da9b508d29a147c66ffe8842eb607adf7a19d18c854a49ff`. Frozen contract SHA256 `240761966d3fd1971fc319a8e48ffb83cae32de57b052f1414c7805e9c072191`.

Fresh default and LittleCMS+Python installations compile exported-target C++
delivery/mask/source/alpha consumers and pass all four programs. Each also passes
the six Python coverage tests using its installed extension and engine DLL.
Installed/copied/build DLL and Python module hashes match; Python loader verifies
the installed module and explicitly loads/proves the installed bin/RawEngine.dll.
Receipt: `build-msvc-release/research/coverage-delivery-installed-v2/verification.json`,
SHA256 `59d1b7b7369eb5a202e126a368079fef69e3191a4ccce13ff13a4c8cab2ce0e2`.

The first installed v1 attempt passed default native consumers but failed Python
extension dependency discovery. Its logs/install/build remain preserved. An
explicit installed-library preload in the v2 test loader resolved this host's
import path; no engine arithmetic or public packaging change was made. This is
installed-consumer evidence on the configured Windows runtime, not clean-machine
runtime redistribution certification. No successful receipts were overwritten.

Exact pre-delivery graph/scheduler/Python/CMake/handoff artifacts, four accepted
mask DLLs and Python modules remain under
`build-msvc-release/research/before-coverage-delivery-v1`, with bindings.json.
Older mask/source/alpha receipts describe their historical binaries. MaskOps,
EditGraph, CoverageSource, CoverageOps, RAW/RGB kernels and frozen mask oracle
fixtures are byte unchanged by this delivery step.

```bat
build-msvc-release\build-pinned-camera.cmd
build-msvc-release\research\run-with-windows-env.cmd build-msvc-release\research\run-ctest-tool.cmd --test-dir build-msvc-release -R CoverageDelivery --output-on-failure
```

Versioned full/installed orchestration and logs remain in ignored research storage.
Use fresh output/install/build versions for reruns. Viewer/test/research artifacts
remain outside library installation; no UI changes, commit or push were made by
the agent during this step.

## Next original-plan work

Continue brush/raster painting, then parametric/range and feather mask nodes,
with explicit replay/numeric/source-ID/ROI/cache/history/jobs contracts. RGBA
graph/layers and binary asset persistence remain open. The original broad
coverage/alpha and reusable-mask rows remain open; checklist stays **145 checked /
135 open / 280 total**. Camera NR remains deferred. This accepts the bounded
delivery/session gate without claiming the original plan is finished.

Feature implementation stays first. The performance log retains PERF-028
workflow observations and PERF-039 repeated validation/ownership hypotheses;
PERF-040 adds the unmeasured Python source-copy/admission overlap. No dedicated
profiling, benchmark expansion or optimization was performed.

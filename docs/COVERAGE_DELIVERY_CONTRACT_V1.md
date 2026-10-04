# Scalar coverage delivery and Python graph-session contract v1

Frozen bounded engineering scope, 2026-10-04. Extends the accepted
[typed mask graph](MASK_GRAPH_CONTRACT_V1.md) without changing its arithmetic,
saved schema, source identity or existing RGB/RAW/ICC defaults. Brush, parametric,
range/feather masks, RGBA graph/layers and binary asset persistence remain later
original-plan work. Performance investigations and fixes remain deferred.

## Scalar rendering and shared jobs

CoverageRenderer streams validated CoverageTile payloads in row-major tiles at
native Final/Preview or mip-1/2 Preview, according to CoverageNode extent/support.
RenderRequest has the existing viewport, tile_size, mip and quality semantics.
Reject null/empty callbacks, zero tile size, unsupported levels and out-of-bounds
requests. Empty contained viewports return no tiles and an empty owned image.
Check full actual tile ROI, storage and finite [0,1] values before callback or
assembly. Returned images own packed scalar float32 samples; output equals
untiled requested-level node output, independently of tile partition.
Cancellation is cooperative before/between tiles and after the last callback;
an aborted image raises RenderCancelled without a partial success result.
Do not claim in-node preemption or cancellation latency.

TileScheduler adds typed future<CoverageTile> submit/submit_latest overloads.
RGB and coverage share the same worker pool, pending budget, priority/FIFO
ordering, group namespace, cancellation and completion policy. Newest group
submissions can supersede either payload type. Queue admission validates the
request before cancelling/replacing prior group work. Exceptions settle only
the affected future; subsequent queued work remains executable. Node ownership
pins immutable graph dependencies. Scheduler destruction cancels queued jobs
and waits for running work, preserving the existing RGB policy.

## Python ownership and source admission

RasterGraphSession continues to own 1-64 stable UUID source records. Existing
RGB specs remain unchanged. A coverage spec contains `coverage`, `width`,
`height`, optional `x`, `y` (native origin, default zero), and optional
`row_stride_samples` (zero/omitted means width). No RGB, working_space,
row_stride_pixels, input/output profile or other source fields are admitted.
Dimensions/origin/stride must be non-boolean uint32 integers. The C-contiguous
buffer contains exactly stride*height native-endian float32 samples. Raw byte
buffers or native float32 typed buffers are accepted; other typed/endian buffers
are rejected. Shape does not substitute for explicit dimensions/stride.

Copy every Python buffer while holding the GIL before releasing it for native
admission. CoverageImage then owns the canonical packed visible snapshot;
padding has no validation/identity/output influence. Caller mutation or release
cannot change an admitted source. Admission/fingerprinting/rendering release
the GIL. Source metadata is copied, with kind coverage_raster_f32, canonical
content_sha256, width/height and native x/y; no working_space or ICC identity.
Exports use explicit format4 whenever any owned source is coverage. RGB-only
exports retain their existing format2 policy. Coverage-only sessions use the
document's existing ProPhoto working-space default without attaching a color
space to the scalar source. No implicit conversion or mask-as-RGB storage.

Replacement fully admits the new typed source before an atomic source-ID swap.
Failed replacement leaves the previous source intact. New manifests must use
the new identity; old submitted jobs and history revisions retain their old
source handles. Existing source count, subset selection, cache sharing, session
close and output-profile behavior remain. No Python references cross into
native worker callbacks.

## Explicit Python scalar output

Graph sessions add render_coverage_manifest, submit_coverage_manifest and
submit_coverage_manifest_latest with the existing manifest/options/priority/group
argument conventions. Existing render_manifest/submit_manifest remain RGB and
reject scalar output. Scalar methods reject RGB output before rendering or
dispatch. Results are (width,height,owned bytes) with one packed native float32
sample per pixel; existing RGB results retain three samples per pixel.
RenderJob.result/done/cancel/progress/timeout behavior is preserved for both
payload types. output_kind reports rgb or coverage. Progress counts successfully
rendered output tiles, not internal mask/source visits. Repeated result retrieval
returns independent Python bytes; submitted jobs pin their sources.

EditHistory adds render_coverage, submit_coverage, submit_coverage_latest and
compare_coverage using the existing revision/request conventions. Commit,
undo/redo, save/restore, limits and immutable pinned source bindings apply to
format4 RGB or scalar revisions. RGB and scalar methods reject opposite-kind
revisions; comparisons require both requested revisions to match the method's
payload kind. Parent session replacement/close/destruction cannot alter pinned
history outputs; explicit history close rejects new work and cancels its jobs.
Histogram/local RGB analysis continues to reject scalar output.

## Acceptance

Native tests require exact native/reduced/ROI/partition streaming/image results,
empty/guard/actual-tile validation, callback failure and cooperative cancellation.
Promise-gated shared RGB/scalar queue tests require priority/FIFO, pending budget,
cross-kind group replacement, active cancellation, invalid replacement isolation,
source fault recovery and destruction behavior. Do not use timing sleeps to
establish queue ordering.

Standard-library Python tests pin the accepted independent mask-chain bits and
coverage-source identities; exercise padded/origin/typed-buffer admission,
caller ownership, native/reduced/ROI/cache, mixed masked RGB, atomic replacement,
saved manifests/history, payload guards, jobs/progress/timeout/lifetime/close and
source identity rejection. Existing full RGB/RAW/ICC tests must pass. Fresh
installed native consumers and Python module smoke tests are required. Preserve
older accepted source/native receipts. The original broad mask/alpha rows remain
open until all required brush/local/RGBA scope is implemented and qualified.

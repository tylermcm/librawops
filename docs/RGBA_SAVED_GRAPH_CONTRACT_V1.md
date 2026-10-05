# Saved RGBA integration contract v1

2026-10-04. Extends the accepted typed native RGBA foundation without changing
its arithmetic. Original coverage/RGBA checklist row stays open until all
saved/cache/footprint/delivery/jobs/Python/history gates pass.

## Serialization and strict execution

Format 5/process 2 adds `premultiplied_rgba_raster_f32` owned sources, requiring
explicit admitted working space and canonical native fingerprint. Format 1-4
retain their previous meaning; older formats reject new RGBA kinds/types/domains.
Existing scalar operations can execute in format 4 or 5. RGBA domain names are
`premultiplied_scene_linear_prophoto_d50` and
`premultiplied_scene_linear_rec2020_d65`; alpha has the existing Coverage domain.

Five new schema1/process2 operations have empty parameters, normal blend mode,
opacity1, no mask edges and no extension fields:

| Type | Inputs | Output | Disabled behavior |
| --- | --- | --- | --- |
| rawengine.rgba_premultiply | image RGB, alpha Coverage | Same-space premultiplied RGBA | Rejected: this adapter changes payload type |
| rawengine.rgba_apply_coverage | image RGBA, coverage Coverage | Same-space RGBA | Alias image after validating both declared inputs |
| rawengine.rgba_source_over | source RGBA, backdrop RGBA | Matching-space RGBA | Alias backdrop after validating both declared inputs |
| rawengine.rgba_alpha | image RGBA | Coverage | Rejected: adapter changes payload type |
| rawengine.rgba_straight_rgb | image RGBA | Same-space RGB | Rejected: adapter changes payload type |

Require exact named ports, declared/actual domains and matching native extents,
including disabled operations. Ordinary RGB/scalar operators reject RGBA image
edges. Binding has exactly one RGB/Coverage/RGBA handle, matching kind, bounds,
space and fingerprint. Unknown operations remain preserved but cannot execute.
Format-5 graph output has explicit RGB, Coverage or RGBA accessors; using the
wrong output accessor rejects. Output profile applies only to actual ICC output.

## Cache and source traversal

Share existing cache generations, LRU and budget across all three payloads;
cache key includes a distinct payload tag. RGBA entry charges 256 bytes plus
actual four-channel float storage. Validate returned ROI, working space, full
storage/finite/alpha invariants before caching. Tiny budgets bypass cache;
in-flight clear must not repopulate the old generation. Failures never publish
partial entries. Every enabled operation signature binds the complete canonical
operation and all ordered used input signatures; disabled signatures alias the
chosen input, excluding unused source footprints.

Iterative source-ID traversal merges requests per node/type/level and retains
every named dependency. Native RGBA nodes and explicit adapters map a requested
preview cell to native Final before traversing RGB/scalar/RGBA branches. Source
records sharing one native node retain distinct IDs. Subsequent RGB geometry,
filters and scalar masks compose with the adapters' mapping and actual halos.
Do not infer color/alpha mixing equivalence across preview reduction.

## Delivery, ownership and Python

RgbaRenderer mirrors scalar row-major tiled streaming and owned assembly,
validates every returned tile/space and checks cancellation before/between and
after callbacks. Empty in-bounds delivery viewports return empty typed storage;
native node requests remain nonempty. TileScheduler adds typed RGBA futures to
the existing workers, pending budget, priority/FIFO and cross-payload latest
groups. No additional worker pool or UI integration.

RasterGraphSession admits `{rgba: native float32 buffer/bytes, width, height,
x?, y?, row_stride_samples?, working_space?}` as copied premultiplied sources;
working space defaults to the established ProPhoto default. Reject RGB/coverage/
profile and unknown field mixtures. Buffer ownership and padding policy match
RgbaImage. Export format5 if any owned source is RGBA, otherwise retain existing
format selection. Explicit render_rgba_manifest/submit_rgba_manifest/latest and
history render_rgba/submit_rgba/latest/compare_rgba return packed float32 RGBA
bytes in the existing `(width,height,bytes)` tuple shape. Wrong typed methods
reject; RenderJob.output_kind returns rgba. Jobs/history retain pinned sources
after caller mutation, revision navigation and session closure.

Independent frozen native fixtures remain truth. Add saved replay/canonical
serialization, every source/port/domain/version/disabled guard, source-ID merge,
cache payload separation/accounting/clear/fault recovery, mixed RGB/scalar/RGBA
jobs and supersession, atomic history and owned Python buffers, both spaces/all
levels/partitions, full four-configuration and fresh installed consumer gates.
Log materialization/copying/validation observations under PERF-047; performance
fixes, photographic qualification, layers/assets and camera NR remain deferred.

# Typed mask graph and masked RGB adjustment contract v1

Frozen engineering scope, 2026-10-04. Implements the next shared-mask graph gate
under the existing Phase 5 rows. The [coverage source](COVERAGE_SOURCE_CONTRACT_V1.md)
and [coverage/alpha](COVERAGE_ALPHA_CONTRACT_V1.md) numeric contracts remain intact.
Brush/parametric/range/feather nodes, RGBA graph/layers, Python coverage sources,
binary asset persistence and production qualification remain required later work.

## Typed native nodes

`CoverageNode` returns CoverageTile, with native extent, supported render levels,
level extent and upstream ROI/level mapping. It is separate from RGB Node.
`CoverageRasterNode` owns/adapts an immutable CoverageSourceNode without changing
that source's admission, canonical identity or reduction arithmetic.
CoverageInvertNode and CoverageCombineNode preserve their input native extent;
combine requires matching native extents, including origin. Both support a level
only if all participating inputs support it. They process already reduced input
coverage at that requested level, with no halo. They do not claim equivalence to
native mask algebra followed by reduction.

Each actual input tile must match the requested ROI and complete finite [0,1]
storage. Validate all inputs before mapping, including endpoint/constant cases.
The following original, explicit coverage algebra is versioned; no Photoshop
behavior is inferred:

| Operation | Ordered binary64 map, then binary32 storage |
|---|---|
| invert | 1 - a |
| add | min(1, a + b) |
| subtract | max(0, a - b) |
| intersect | a * b |

Generated numerical zeros are positive zero. No epsilon, threshold, RGB encoding,
coverage gamma or automatic edge refinement is introduced. Replay qualification
is nearest-even with gradual underflow, no FMA/reassociation, unchanged caller
floating environment. The arithmetic cannot overflow binary64 or exceed the
declared coverage range after its explicit clamp.

`MaskedMixNode` is a scene-linear RGB Node with base/layer RGB branches and one
typed coverage branch. Matching exact ProPhoto/D50 or Rec.2020/D65 descriptors and
native extents are required. Amount is finite binary64 [0,1]. Per pixel compute
`w = double(mask)*amount`; w==0 copies base bits, w==1 copies layer bits.
Otherwise per channel compute `delta = double(layer)-double(base)`, then
`scaled = w*delta`, then `mapped = double(base)+scaled`, with those explicit stages.
Identical channel bits copy directly. Validate complete actual RGB/coverage tiles
before endpoint returns; signed/headroom colors survive. Check mapped against
finite +/-FLT_MAX before casting; throw overflow_error, without clipping.
This arithmetic is independent from the old LinearMixNode, which remains exact.
The operation evaluates base, layer and mask at the requested level before mixing.
It has no alpha, layer/group/fill or encoded-domain blend interpretation.

## Saved state and source binding

New source kind `coverage_raster_f32` and domain `coverage` are admitted in explicit
manifest format 4 and processing version 2. A coverage source has canonical v1 content_sha256 and UUID,
with no working_space, ICC or demosaic fields. Format 4 retains format-3 explicit
Bayer reconstruction policy. Format-1 migration and existing format-2/3 RGB
serialization/defaults remain unchanged; there is no implicit format upgrade.

New executable types require format 4, schema 1, processing version 2, normal
blend_mode, opacity 1, no operation extension fields and the exact following ports:

| Type | Input ports | Mask ports | Parameters | Domains |
|---|---|---|---|---|
| rawengine.mask_invert | mask | none | empty | coverage -> coverage |
| rawengine.mask_combine | base, layer | none | mode: add/subtract/intersect | coverage -> coverage |
| rawengine.masked_mix | base, layer | coverage | amount | matching scene-linear RGB -> RGB |

Disabled invert bypasses mask; disabled combine/masked_mix bypasses base. Saved
settings, ports, versions, domains and all upstream types/extents are still
validated. Disabled operations share the base upstream identity and execution
footprint; their inactive layer/mask edges do not affect rendering. Enabled
operations include every input and mask content/signature in their identity,
including amount zero and coverage endpoints. Existing RGB operations continue
rejecting mask metadata or coverage edges unless explicitly extended.

BoundEditSource retains the old RGB handle and adds a separate coverage handle.
Exactly one handle must be populated with the matching saved kind. Verify saved
record, runtime content fingerprint and exact native bounds. A scalar source
cannot masquerade as RGB or carry a color/ICC/reconstruction identity. All named
input/mask edges participate in existence/cycle/topological validation.
Coverage outputs expose explicit coverage accessors; RGB output accessors reject
them. Existing RGB Renderer/TileScheduler entry points must fail closed on scalar
output rather than dereference an absent RGB handle.

## Cache, footprints and history

TileCache shares one LRU budget, generation, stats and eviction policy across
RGB/coverage. Keys include payload kind in addition to signature, ROI, mip and
quality; charge actual 3-channel or scalar float payload plus the existing fixed
entry allowance. There is no mask-as-RGB storage or independently additive mask
budget. Clear prevents in-flight old-generation work from repopulating either
payload. Cached return storage is copied; actual tile validation precedes admission.

Graph signatures include canonical source record and every enabled upstream
signature. Shared mask branches may reuse upstream coverage tiles across revisions
and consumers. Planner traverses all reachable RGB and coverage dependencies,
composes RGB transforms/halos and coverage ROI/level mappings, maps coverage source
cells to native coordinates, and unions shared source-ID footprints. Singular
helpers reject outputs using multiple distinct source IDs, including mask sources.
Disabled masked nodes expose only their base footprint. No in-node cancellation
or new mask scheduler policy is claimed.

Core EditHistory keeps immutable RGB or coverage graph revisions and pinned source
bindings, with strict format-4 JSON round trips, source identity checks and
navigation/retention semantics. The existing scheduler can execute masked RGB
outputs because they remain RGB Nodes and own their coverage dependencies.
Dedicated scalar streaming/jobs and Python source/session integration are later
required gates, not a claim of this bounded core acceptance.

## Acceptance gates

An independent Fraction oracle pins staged mask algebra and masked RGB outputs,
including tiny coverage, amount endpoints, signed/headroom/extreme color and
finite-range boundary behavior. The mapped-range guard remains defensive; valid
convex-mix fixtures do not claim an overflow case. C++ tests must cover actual/declared tile validation,
native/reduced/ROI/partition parity, strict ports/domains/versions/enabled-disabled
guards, source substitution/fingerprint/extent mismatch, format-1/2/3 regression,
mask-only cycles, shared mask cache identity/reuse/invalidation/budget/generation,
RGB/cov key separation, mixed source-ID footprints, crop/halo composition,
immutable history/restore/lifetime and masked RGB queue delivery/recovery.
Fresh installed consumers and full configured builds are required after native
integration. No broader row is checked off until its full required scope passes.
Observe routine development costs in [the performance log](PERFORMANCE_ISSUES.md);
dedicated performance investigation/fixes remain deferred.

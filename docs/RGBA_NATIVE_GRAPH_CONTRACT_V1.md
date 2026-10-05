# Typed native RGBA graph foundation v1

Bounded implementation contract, 2026-10-04, under the existing Phase 5
float-coverage/premultiplied-RGBA row. That original row stays open until saved
graphs, typed cache/delivery/jobs, Python and history integration are accepted.
This stage adds native typed nodes and immutable source ownership only.

## Payload, ownership and identity

Reuse the frozen [alpha arithmetic](COVERAGE_ALPHA_CONTRACT_V1.md) unchanged.
RGBA remains a separate payload from RGB `Tile`/`Node` and scalar coverage.
Native bounds are nonempty with uint32-representable end coordinates. RGBA has
four finite binary32 samples per pixel, alpha in [0,1], signed/headroom color,
and exact ProPhoto/D50 or Rec.2020/D65 scene-linear working-space identity.
Transparent premultiplied input must contain numerically zero color.

`RgbaImage` copies visible pixels from an exactly sized stride-times-height
vector; padding is ignored, including nonfinite padding. Row stride is measured
in float samples, zero means four times width. Canonical storage is tightly
packed; transparent pixels become four positive zeros, other color bits are
preserved. Source snapshots share immutable state and lazy thread-safe SHA-256.
Fingerprint bytes are ASCII `librawops.source.rgba.premult.f32.v1` including one
terminating NUL, then little-endian uint32 x/y/width/height, working-space tag
1 for ProPhoto or 2 for Rec.2020, then canonical float32 sample words. Stride,
padding and caller mutation cannot change identity.

## Nodes and explicit adapters

`RgbaNode` owns typed render, bounds, working-space and native footprint APIs.
`RgbaRasterNode`, `RgbPremultiplyNode`, `RgbaApplyCoverageNode` and
`RgbaSourceOverNode` compose immutable inputs. Matching native extents and
working spaces are required. RGB premultiplication requires an exact working
RGB descriptor captured at construction. Its caller supplies the RGB output
extent; runtime tiles must match the requested native rectangle and descriptor.
No RGB extent is inferred from a transformed node's original source bounds.

`RgbaAlphaNode` explicitly extracts scalar alpha. `RgbaStraightRgbNode`
explicitly exposes checked float32 straight RGB, with overflow rejection and
the matching exact scene-linear descriptor. `straight_rgb_float64(tile)` gives
owned binary64 RGB storage for safe access to all admitted tiny-positive-alpha
pixels; no epsilon floor or clipping. Both adapters use the existing pixel
functions, validate entire returned RGBA storage and preserve their endpoint
rules. Ordinary RGB controls do not implicitly accept RGBA.

## Levels, ordering and footprints

Support native Final/Preview and mip 1/2 Preview when all used upstream inputs
support native Final. Every operation requests native Final inputs, applies the
frozen per-pixel operation at native resolution, then reduces its own result.
Alpha/straight adapters likewise run at native resolution before reduction.
This deliberately differs from unpremultiplying a reduced RGBA tile. Reduced
origin is zero; extent is ceil(width/2^mip), ceil(height/2^mip). Native origin is
retained. Each reduced pixel averages its true clipped native cell in global
row-major order, with sequential binary64 accumulation, binary64 division by
actual count and final binary32 storage. No cascaded mip reduction, halo,
replicated border, unweighted color average or source-level substitution.
If stored reduced alpha is zero, all stored colors become positive zero.

Native footprint mapping uses uint64 coordinate products and clips against the
true native extent. All point inputs receive the same native rectangle; input
level is native Final. Exposed typed input pointers describe every edge for
subsequent saved-graph dependency traversal. Bounds/level/extent/resource
admission occurs before calling upstream. Actual returned ROI, channel count,
space/descriptor, alpha and finite values are checked even for bypass cases.

## Resource and evidence scope

Per-vector and aggregate logical payload sizes must fit size_t before upstream
render. Counts include requested native input/output vectors and reduced output;
they exclude upstream scratch, ownership, cache, allocator and concurrent work.
The initial implementation materializes the native requested rectangle and
validates at operation boundaries. Log duplicate validation/materialization as
an observation; defer optimization until functional construction is complete.

Require independent Fraction/nearest-even fixtures for product-before-average,
source-over ordering, alpha/straight adapter ordering, clipped cells, subnormal
alpha and full-range signed color; independent hashlib fingerprint agreement;
native/Preview partition and exhaustive ROI parity; immutable ownership;
threaded source identity; malformed actual tiles, invalid levels, spaces,
extents, resource admission and float32-access/source-over overflow guards.
Retain inherited four-mode replay limits: exact bits are asserted for nearest
even with gradual underflow; caller rounding controls are never changed.

This stage does not add blend modes, layers, resampling of RGBA, a saved schema,
file loading, Python APIs, caches or schedulers, and does not claim production
or Adobe qualification. Existing RGB and scalar graph behavior remains covered
by regression tests. No new third-party code or runtime dependency is introduced.

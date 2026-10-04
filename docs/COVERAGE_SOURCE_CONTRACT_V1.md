# Immutable coverage source contract v1

Frozen bounded source scope, 2026-10-03. This extends the accepted
[coverage/alpha numeric foundation](COVERAGE_ALPHA_CONTRACT_V1.md) toward the
original Phase 5 reusable mask graph. It does not complete that graph, painting,
layers or the broader coverage/alpha checklist row.

## Admission and ownership

`CoverageMetadata` contains native `Rect bounds` and caller `row_stride_samples`.
Bounds must be nonempty and their exclusive end coordinates must fit uint32.
Zero stride means width; any other stride must be at least width. Caller storage
must contain exactly stride*height binary32 samples, with checked size arithmetic.
Only visible samples define the source: every visible value must be finite and
in [0,1]. Row padding is ignored, not rendered, hashed, retained or validated.

The constructor takes a const vector reference and copies visible samples into
fresh, tightly packed storage. Retained caller pointers, shared vectors and later
caller mutations cannot affect this snapshot. Owned metadata reports stride equal
to width, and owned samples have exactly width*height elements. Visible negative
zero is canonicalized to positive zero; no other float32 bits change. This is the
explicit canonical source admission boundary, without changing the older RGB
source fingerprint format or the alpha helper's negative-zero acceptance.

Copies share immutable source state. A source node owns such a snapshot by value,
not a caller reference. Source and output lifetimes are independent; render output
owns fresh storage. A fingerprint is computed once per shared snapshot with
thread-safe memoization. Constructors reject malformed metadata/storage/visible
values with `invalid_argument`; allocation failures propagate. A moved-from image
is only assignable/destructible until given a new snapshot.

## Identity

Coverage source fingerprint v1 is SHA-256 of the following bytes, in order:

1. ASCII `librawops.source.coverage.f32.v1` followed by one NUL byte.
2. Native x, y, width, height as four little-endian uint32 values.
3. Canonical visible float32 sample bits in native row-major order, little-endian.

Stride, row padding, memory addresses and caller layout are excluded. Origin,
visible dimensions and every visible bit are included. Thus padded/packed inputs
and positive/negative input zero produce the same canonical owned identity, while
moving a mask on the native canvas changes its identity. It is distinct from RGB,
RAW and ICC source hashes. The fingerprint does not yet define a saved asset or
document schema; it supplies canonical content identity for later graph binding.

## Levels, sampling and footprints

`CoverageSourceNode` has a scalar return type and does not inherit the RGB-only
`Node`. Supported levels are native mip 0 Final or Preview, and mip 1/2 Preview.
Other quality/mip combinations reject, including unknown enum values. Native
bounds retain the declared origin. Reduced bounds are zero-based with dimensions
ceil(width/scale), ceil(height/scale), where scale=2 or 4.

Native render copies canonical float32 bits exactly. A reduced output pixel
(u,v) averages the direct native rectangle beginning at native origin+(scale*u,
scale*v), ending at min(scale*(u+1),width), min(scale*(v+1),height) relative to the
origin. Iterate present samples in y-then-x order, accumulate in binary64 without
FMA/reassociation, divide by the actual sample count, then store binary32. No
zero padding, edge replication, cross-tile averaging or mip-1 cascade is used.
The rules apply equally to singleton axes and odd dimensions. There is no RGB,
transfer, luminance or alpha interpretation of these scalar samples.

The native footprint of a reduced ROI is exactly the bounding rectangle of its
native sample cells, clipped at the true source edge; native ROI maps to itself.
Requests must be nonempty, fit checked uint32 bounds and be entirely within the
requested-level output extent. Out-of-bounds or invalid-level requests reject
before allocating or accessing pixels. Returned bounds equal the requested ROI.
Full/partitioned/ROI renders are exact by construction because each output pixel
uses the same global source cells and ordered arithmetic.

Numeric replay is qualified for binary32/binary64 nearest-even and gradual
underflow. The implementation does not alter caller floating environment.
Coverage sums have at most 16 values in [0,1], so binary64 overflow is impossible;
ordered summation rounding still matters and is represented in the independent
oracle. There is no production pyramid, mask smoothing or post-composite
reduction equivalence claim.

## Integration and acceptance

An independent Python integer/Fraction oracle must pin canonical fingerprints,
native bits and staged reduced outputs, including padded input, negative zero,
tiny positive values, odd/singleton dimensions, nonzero origins and extreme valid
coordinates. Native tests must prove ROI/partition parity, exact footprint
planning, complete admission, snapshot/output ownership, immutable copy lifetime
and concurrent fingerprint/render behavior. Installed C++ consumers must exercise
the installed public header and exported library.

Next, the shared typed mask graph must reference these snapshots through named
mask edges and verified source IDs, with extent/domain validation, source-region
traversal and complete signatures. Mask algebra/brush/parametric nodes, cache,
jobs/history/Python and persisted assets remain required subsequent work. Old RGB
nodes continue rejecting masks/RGBA unless explicitly extended. No UI is added.
Keep ordinary build/test observations in [the performance log](PERFORMANCE_ISSUES.md);
dedicated performance investigation and fixes remain deferred.

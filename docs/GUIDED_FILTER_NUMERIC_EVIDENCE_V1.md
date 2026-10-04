# Working-Y guided-filter pre-freeze numeric evidence v1

Status: research evidence, 2026-10-03. The [contract draft](GUIDED_FILTER_CONTRACT_DRAFT_V1.md)
remains unfrozen. The compiled programs below are isolated prototypes, with no
engine DLL, node, saved type, Python binding or default graph change.

## Finite-intermediate envelope

This proof concerns the draft's ordered centered arithmetic, for every finite
binary32 RGB input, either declared working space, radius0..8 and admitted epsilon.
It establishes finite binary64 intermediates, not mathematical accuracy or a
finite binary32 output. Output overflow must still reject.

Use round-to-nearest, down, up or toward-zero binary64 arithmetic, without FMA or
reassociation. For a representable finite bound B, rounding an exact result in
[-B,B] cannot leave that interval in any of these modes. All power-of-two bounds
below are normal, exactly representable binary64 values. Each accumulation has
at most289 terms. If each rounded term has magnitude at most2^e, induction bounds
the kth rounded partial sum by k*2^e: this endpoint is exactly representable for
k<=289 at the exponents used here. Thus the completed sum is bounded by2^(e+9).
This argument covers the ordered sums without assuming exact additions, favorable
cancellation or a statistical distribution.

Finite binary32 channel magnitude is strictly less than2^128. Promotion to
binary64 is exact. Both actual binary64 coefficient pairs have 0<wr<1 and 0<wb<1.
Actual clipped window counts are integers1..289 and exactly representable. The
following deliberately loose bounds apply to every channel and coefficient:

| Rounded quantity | Absolute bound |
|---|---:|
| RGB difference from the coefficient/output anchor | 2^129 |
| Difference between two RGB differences | 2^130 |
| Each wr/wb product | 2^130 |
| First green-plus-wr term | 2^131 |
| Ordered working-Y difference D | 2^132 |
| First-pass Y / RGB sums | 2^141 / 2^138 |
| Y / RGB mean offsets after division by count>=1 | 2^141 / 2^138 |
| Centered Y / RGB residuals | 2^142 / 2^139 |
| Squared Y residual / Y-RGB cross-product | 2^284 / 2^281 |
| Variance / covariance sums | 2^293 / 2^290 |
| Population variance / covariance after count division | 2^293 / 2^290 |
| variance+epsilon | 2^294 |
| Regression slope | 2^314 |
| Final D(i,k)-Y mean offset | 2^142 |
| Final RGB difference+RGB mean offset | 2^139 |
| Slope*centered guidance | 2^456 |
| Final per-center correction term | 2^457 |
| Ordered correction sum / averaged correction | 2^466 / 2^466 |
| Input anchor+averaged correction | 2^467 |

Squared residuals and their sum are nonnegative. Dividing by a positive exact
count preserves that sign. The rounded denominator is at least epsilon: adding
a nonnegative variance to the representable positive epsilon cannot round below
epsilon. Since epsilon>=2^-24, covariance/denominator has magnitude at most
2^290/2^-24=2^314. No zero denominator or negative-variance repair is necessary.
All bounds are far below binary64 overflow. Underflow or cancellation can change
accuracy, but cannot violate this upper envelope. No lower bound on a nonzero
residual, variance, covariance or correction is asserted.

The implementation must retain finite-intermediate checks and reject an actual
ordered final value outside [-FLT_MAX,FLT_MAX] before the one final float32 cast.
That guard tests the implemented value, not the exact-real kernel target. It
does not clamp a slightly out-of-range result or silently substitute exact-real
membership. A failed allocation is still possible; the numeric proof is not an
available-memory guarantee.

## Strict compiled arithmetic and accuracy findings

`build-msvc-release/research/guided-filter-precision-v1/` compiles original
centered arithmetic with MSVC19.40.33812.0, C++20, Release and `/fp:strict`.
`pass-v1/report.json` SHA256
`995d211f64d485bb7ef90e4cef25183ae8484a24586a1fc71f5181ef2c6a4f01`
binds its source, executable, fixture bits, stdout and the then-current draft.
590 fixtures generate2360 records across four rounding modes. Selected fixtures
cover both spaces,epsilon endpoints,translated/last-addressable bounds,large
offsets,subnormals,signed zeros,finite binary32 extremes,near-cancelling guidance,
seeded extreme RGB and invalid parameters/source values.

Independent exact Fraction weighted-RGB expanded kernels and integer-directed
float32 rounding check69,285 numeric channels:68,101 equal the reference value.
There are152 invalid-input rejections and213 actual output-overflow rejections;
36,878 tile,5,985 ROI and96 independent-thread comparisons are bit-exact within
each rounding mode. These are prototype arithmetic comparisons, not engine
cache/job/graph acceptance. The maximum observed instrumented exponent is257;
that measurement is distinct from the universal conservative bound above.

The selected-case accuracy check is
`1e-5*max(max_abs_input_channel,abs(exact_target))*max(1,kernel_row_L1)+4*2^-149`.
Its maximum observed ratio is0.24999875000624996. This is a fixture acceptance
bound, not a universal theorem or a promised ULP bound. Near-zero cancellation
can produce enormous ULP counts. In a selected Rec.2020 near-cancelling fixture,
red input magnitude reaches2.7444638030242367e37. A mathematically zero red output
under directed rounding becomes magnitude5.5793980804635185e28, about2.033e-9 of
that input-channel scale. Even nearest rounding has a selected mathematically
zero green target with an actual residual of-293203116032, about2.757e-26 of its
1.0633823966279327e37 input scale. These residuals remain visible in the evidence;
there is no near-zero snap, relative-output accuracy or universal neutral/zero
preservation claim for varying fields.

One extreme-random/downward case rejects even though its largest exact target
is inside FLT_MAX by approximately3.031e-84 of FLT_MAX. This is retained as an
actual ordered boundary decision. The supplemental channel analysis report
`research/guided-filter-numeric-analysis-v1/report.json` SHA256
`391574e1467764633b8924e2c3944f33836362c449d943df32cddc1790710ec8`
binds these details to the immutable precision report.

## Rectangle, capacity and actual-layout checks

`research/guided-filter-guards-v1/` compiles the unchanged preserved prototype
into a separate executable. It checks35,709 assertions including566 expected
rejections,8,721 composed-halo rectangle cases and48 actual payload cases across
four rounding modes. Tests cover clipped edges/interiors,translated image bounds,
exclusive endpoints exactly2^32,missing/extra/misplaced source bounds and storage,
empty/outside requests,nonfinite source including a needed halo outside output,
radius-zero validation and vector-capacity overflow without huge allocations.
This checks the pure helper; engine descriptor/revision/schema guards remain open.

The compiled coefficient struct is exactly56 bytes: seven binary64 values.
Its actual vectors match `12N+56A+12O` numeric bytes for positive radius and
`12N+12O` for radius zero. Clipping reduces N and A. A256-square output,radius8,
unclipped support has5,924,864 numeric bytes. This includes supplied source and
output numeric storage but excludes vector capacity slack,allocator metadata,
upstream/cache ownership,retained results and concurrency. Each vector length
is checked against `max_size()` before its allocation; products use checked
dimension logic and uint64 exclusive endpoints. Input storage is exact, including
the complete composed2r halo; output contains only the requested rectangle.

The helper preserves rounding mode and MXCSR control bits, including FTZ/DAZ.
Ordinary arithmetic exception-status flags may accrue and are not promised to be
restored. The harness restores its own rounding controls. The prototype has no
shared mutable coefficient/input/output state; selected independent threads are
exact under each caller-selected mode. This is not a throughput or scheduler test.

## Remaining gates and timing limits

Before freeze: clean/high-ISO camera prototypes,requested-level reduce-first
semantics and camera/synthetic artifact inspection;settle node descriptor,
revision/digest,strict saved schema and identity boundaries. Then implement and
verify native/Python/graph/RAW/cache/jobs/history/ICC/installed consumers and full
configured suites. Production/profile/corpus,portable numeric behavior and
algorithm/release qualification remain open. Camera NR remains deferred.

PERF-036:the precision executable's0.3839162s covers many small fixture runs,
copies,ROI/tile replays,guards and threads. Exact-reference verification7.0332328s
is research-tool cost. Neither is isolated kernel/API/45MP/allocator profiling.
The algorithm still performs two ordered window passes per coefficient and one
coefficient-neighborhood pass per output. No radius-independent complexity or
speed claim follows from these checks.

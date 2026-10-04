# Working-Y guided-filter finite-check placement

Status: proof and bounded native candidate, 2026-10-03. Native performance and
acceptance must be verified before this candidate is accepted. This supplements
the [frozen arithmetic contract](GUIDED_FILTER_CONTRACT_V1.md) and its
[finite-intermediate evidence](GUIDED_FILTER_NUMERIC_EVIDENCE_V1.md); neither
frozen arithmetic nor parameter/version/domain/border/resource policy changes.

## Preconditions and representation gate

The helper still validates settings, exact supported descriptor, needed rectangle,
storage capacity/length and every needed RGB sample before filtering or radius-zero
identity. Therefore all admitted samples are finite. Radius is at most8, each
actual clipped window has1..289 samples and epsilon is finite in[2^-24,65536].
Both pinned working-Y coefficient pairs have0<wr<1 and0<wb<1.

Only skip per-sample intermediate guards when float and double report IEC559,
radix2, respectively24/53 significand digits, maximum exponents128/1024, minimum
exponents-125/-1021 and storage sizes4/8 bytes. This is the existing binary32
input/binary64 arithmetic case. Other representations retain every old guard.
Compile with strict floating-point rules; no FMA, reassociation, sliding sums,
rounding-mode/FTZ/DAZ mutation or replacement of directed subtraction is introduced.

## Complete finite-input argument

The existing conservative bound covers every ordered arithmetic intermediate,
not only measured fixtures. For a normal exactly representable power-of-two bound
B, any of the four supported rounding modes maps an exact result in[-B,B] into
that interval. For at most289 terms bounded by2^e, induction bounds the kth
rounded partial sum by k*2^e. These integer multiples are exactly representable
in binary64 at all exponents below, so the complete sum is bounded by2^(e+9).

| Quantity | Conservative magnitude bound |
|---|---:|
| Promoted finite binary32 sample | strictly less than2^128 |
| Ordered RGB anchor difference | 2^129 |
| Difference of RGB differences / weighted products | 2^130 |
| Green-plus-wr term / complete ordered Y difference | 2^131 / 2^132 |
| First-pass Y / RGB sum and mean offset | 2^141 / 2^138 |
| Centered Y / RGB residual | 2^142 / 2^139 |
| Squared Y residual / Y-RGB product | 2^284 / 2^281 |
| Variance / covariance sum and count-divided value | 2^293 / 2^290 |
| Variance plus epsilon | 2^294 |
| Covariance/count/denominator slope | 2^314 |
| Output centered guidance / RGB correction offset | 2^142 / 2^139 |
| Slope-guidance product / per-center correction term | 2^456 / 2^457 |
| Ordered correction sum / count-divided correction | 2^466 / 2^466 |
| Input anchor plus correction | 2^467 |

Counts are positive exact integers. Squared residuals and their ordered sum are
nonnegative; division preserves this sign. Adding a representable positive
epsilon to a nonnegative variance cannot round below epsilon. The denominator
is consequently at least2^-24. All bounds lie well inside binary64's finite
range. Underflow, cancellation and subnormal behavior can affect accuracy but
cannot increase magnitude beyond this envelope. No lower bound on a nonzero
correction is claimed. The published7,803 representable summation-endpoint
checks support the bound construction; the proof supplies its universal scope.

Thus the removed guards cannot detect a nonfinite value on admitted binary32/
binary64 input. Their removal does not expand accepted settings, source storage
or descriptors and does not excuse an actual binary32 output overflow. The proof
does not imply a universal ULP/relative-output bound, positive weights, convex-hull
preservation, noise reduction or a no-overshoot guarantee.

## Retained checks and ordered behavior

Keep unconditional checks on every complete input-halo sample, guide/channel
mean offsets after the first pass, variance and denominator, every regression
slope, each averaged output correction, mapped output and final float32 cast.
Keep the nonnegative variance/denominator guard, actual finite float32 range guard,
all radius-zero/signed-zero identity rules, storage/rectangle/capacity guards and
descriptor pinning. Non-IEC559 or differently sized/ranged representations retain
the original per-difference, per-product/residual/term and per-addition guards too.

The three fixed arrays, coefficient table and output buffer retain their exact
layout and sizes. Loop nesting, absolute y/x traversal, actual counts, two moment
passes, every arithmetic expression and operand order, zero-slope bypass and one
final float32 cast are unchanged. In particular the reverse RGB difference is
recomputed; it is not obtained by negating the forward difference. Saved schema1/
process2, cache signatures, parameters, defaults and graph insertion are unchanged.
Cancellation remains at renderer tile boundaries.

## Evidence and acceptance boundary

The17-file accepted source/native/evidence baseline is archived at
`build-msvc-release/research/guided-filter-perf-baseline-v1`, report SHA256
`a0b2695618e010f04bad2f2e6cbeacd5f5fbd2f2ac97129efb19ff2e1da7ac15`.
The verified paused snapshot binds its source, copied DLL, private candidate,
profile executable and result. Profile review SHA256
`eb923f3287396d77c0012b1c3498636e469955ce52aad66e4adadd952006e7c1`
verifies all9 paused bindings and17 archived files.

Private baseline/candidate/forced-full-guard fallback produce7080 records against
frozen590-fixture/four-mode observations,207855 exact float32 channels,456 invalid
and639 actual output-overflow rejections,5985 exact ROI and96 thread comparisons.
Rounding and MXCSR control bits are unchanged; exception-status flags may accrue.
Both working spaces, signed zeros, subnormals, finite extrema, offsets, true
borders, near-zero cancellation and actual float32-boundary decisions are retained.

Twenty-four serial128x96 source/profile cases cover both spaces, translated
interior/top-left bounds, radii1/3/8 and epsilon endpoints. Source copy and input
validation/allocation are small in this study; ordered moments and output
corrections dominate. Paired private candidate median speedups are4.2736x/7.6197x/
8.4038x for radii1/3/8. Per-coefficient clocks perturb stage timings; the paired
timings use uninstrumented copies and alternating order. These are prototype
results, not native gains or a comparison to the256-square API benchmark.

Before acceptance, verify the actual changed DLL against the frozen helper/node/
saved graph records and paired old/new native timing with stable source work.
Rerun dedicated Python, camera/RAW/raster/ROI/tile/mip/cache/jobs/history/ICC gates,
fresh installed consumers and all configured full suites. Measured serial API
cost must be reported separately from isolated-helper timing. Reject a candidate
if native output/rejection behavior changes or repeatable cost gates do not hold.
Full-frame/allocator/concurrency/portable/production/profile/corpus/release gates
remain open. No new headline checklist rows, UI, commit/push or camera NR.

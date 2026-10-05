# Mask refinement numeric and resource evidence v1

Pre-native specification evidence, 2026-10-04. This accompanies
[the frozen contract](MASK_REFINEMENT_CONTRACT_V1.md); it does not establish
compiled/native, saved execution, photographic or release acceptance.

## Independent scope

`tests/reference/mask_refinement_oracle.py` retains the candidate's exact
Fraction/IEEE-stage functions as an unchanged dependency. IEEE decode, rounding
and encode are integer/rational implementations, not platform float casts.
The generated `mask_refinement_v1.hpp` contains 238 density scalar cases,
138 frames and 2,001 output-cell/required-input-region records. Frames include
singleton, single-axis, odd/even, shifted and uint32-edge extents; density
endpoints/tiny binary64 values, binomial radii 0/1/2/3/8/32, guided radii 0/1/8,
epsilon endpoints and both supported working spaces. Signed/headroom/extreme
RGB and coverage subnormal/signed-zero samples are retained.

The candidate checks supply 65 explicit assertions, including hand-derived
density direction/endpoints, exact constant fields, the clipped [1,2,1] feather
response [1/3,1/2,1/3] to [0,1,0], and constant-guide two-box response
[5/12,4/9,5/12] to [0,1,0]. They demonstrate that a one-radius guided halo is
insufficient and provide separate feather/guided native-before-mip witnesses.
Twenty-six rejected candidate cases include nonfinite/out-of-range coverage,
nonfinite guide at radius zero, and invalid integral/range controls.
These checks are distinct from complete future native/schema rejection gates.

Nearest-even binary64/float32 stage outputs are the frozen numeric fixture
policy. Other active rounding modes, FTZ/DAZ behavior, portable compilers and
runtime stage equivalence are unverified here. Kernels must not change caller
floating-point controls. Native acceptance must separately exercise their
observable behavior; this document establishes no universal error/ULP bound.

## Conservative finite-intermediate envelope

This is an analytical envelope for the specified ordered arithmetic, not a
measurement or permission to remove finite checks. Binary64 has ample exponent
range relative to all these bounds. Float32 coverage magnitude is <=1; guide
magnitude is strictly below 2^128. Supported working-Y coefficients are positive
and below1. The largest guided window has 289 samples/centers, below2^9.

For each addition/subtraction/product/division below, use a deliberately loose
extra power-of-two margin for rounding. A sum of <=289 values bounded by M has
magnitude <2^10*M: its sequential IEEE relative error factor is below2 because
289*2^-52 <1/2. Underflow cannot create a larger exponent or a nonfinite value.

Guide channel differences are bounded by2^130, differences of those differences
by2^132, coefficient products by2^133, and the two staged sums defining D by
2^137. Sum/division give |mg|<2^148. Mask differences are <=1; the deliberately
loose corresponding bound is |mp|<2^11. Thus |dy|<2^150 and |dp|<2^13.
Squared dy and its summed/divided variance are <2^312; covariance is <2^175.
Every squared term is nonnegative, so staged variance is nonnegative. Adding
epsilon retains a denominator >=2^-24 and <2^313. The slope is <2^200.
The correction product is <2^351, each center correction <2^352, its ordered
sum/division <2^363 and final unclamped map <2^364. A common **2^400** upper
envelope therefore safely contains every specified guided intermediate.
This includes all admitted guide float32 exponents and epsilon endpoints;
it does not imply small cancellation error, nonnegative regression weights or
unclamped output membership in [0,1]. Final coverage clamping is explicit.

Density uses finite operands in [0,1]; its stages cannot overflow. Tiny density
products can underflow and are deliberately exercised by the scalar fixtures.
For feather, every Pascal coefficient through row64 fits uint64; the maximum
is C(64,32)=1832624140942590534. Do not accumulate the entire integer row in
uint64: its sum is 2^64. Converted weights and their binary64 sum are finite;
the included sum is >=1. Weighted coverage sums/division, followed by the
vertical weighted pass, stay below2^140 even with deliberately loose bounds.
This bound is far looser than convex averaging and remains safely finite.

## Geometry and payload

Thirty-five integer resource checks cover singleton, 7x5, 256-square and
uint32-wide/single-axis extents. At an unclipped 256-square request:

| Operation | Maximum selected radius | Logical numeric payload |
| --- | ---: | ---: |
| Binomial feather | 32 | 1,327,104 bytes |
| Working-Y refinement | 8 | 3,364,864 bytes |

These include needed input samples, intermediates and native output. Preview
additionally retains four bytes per reduced output cell. They exclude upstream
graphs, cache, allocator capacity/metadata, Python results and concurrent jobs;
they are not process-memory measurements or allocation ceilings. Native code
must guard actual vector maximum sizes and products before requesting inputs.
Every frozen cell record maps its clipped native reduction block, adds r/2r
support as appropriate, and clips to actual source bounds. Future native
ROI/tile/source-ID replay must match those records exactly.

No performance fix, benchmark comparison, UI, trained model or photographic
reference was introduced. Existing PERF-045 capability-query cost stays deferred.

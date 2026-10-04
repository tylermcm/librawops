# Master curves, shape-preserving cubic and midtone gamma levels v1

Status: frozen before native implementation, 2026-10-03. This freezes the two
new controls under the existing Phase 4 curves/levels expansion requirement.
It does not close that production requirement. Existing curves, levels,
grading, LUT and tonal-range types, defaults, limits and arithmetic remain
unchanged. No dependency, UI, default insertion, headline checkbox or publication.

## Domain and public interface

Both nodes accept one immutable input with RGBFloat32 scene-linear
ProPhoto/D50 or Rec.2020/D65 descriptor. Null input, camera-linear, encoded RGB,
or any other descriptor rejects. Preserve the input descriptor and extent.
Signed channels and headroom are allowed; no encoding, profile selection,
white balance, clipping, gamut mapping or scene statistics is implicit.

The new C++ interface in `ToneOps.hpp` is:

```cpp
enum class CurveInterpolation { Linear, ShapePreservingCubic };
struct CurveKnots { std::vector<CurvePoint> knots{{0,0},{1,1}}; };
struct ExtendedCurvesSettings {
    CurveKnots master;
    std::array<CurveKnots,3> channels;
    CurveInterpolation interpolation = CurveInterpolation::Linear;
};
struct GammaLevelsSettings {
    std::array<ChannelLevels,3> channels;
    std::array<double,3> gamma{1,1,1};
};
// Export validators and immutable ExtendedCurvesNode/GammaLevelsNode,
// each constructed from shared_ptr<const Node> input and settings={}.
```

`CurvePoint` and `ChannelLevels` retain their existing binary64 fields.
`validate_extended_curves_settings` and `validate_gamma_levels_settings` validate
every field even for identity. Constructors copy settings and retain input;
caller mutation cannot change a constructed node. Identity defaults preserve
the original float32 bits after complete input validation.

## Extended curves: admission and ideal mathematical shape

Each of master/R/G/B has 2..256 knots. Every x/y is finite with magnitude
at most 65536. x increases strictly; y may increase, decrease, flatten or change
direction for creative editing. Every binary64 secant
`d[i]=(y[i+1]-y[i])/(x[i+1]-x[i])` is finite with magnitude at most 65536.
Both enum values are admitted; any other C++ value or saved token rejects.
There is no separate x-spacing threshold: even tiny flat segments are admitted
when their finite secants satisfy the bound. No epsilon merges knots.

For cubic interpolation, endpoint tangents equal the adjacent secant.
Interior tangent is zero if either neighboring secant is zero or their signs
differ; otherwise it has the common sign and the smaller absolute secant.
Use sign comparison, not multiplication of secants, to select this minmod rule.
For segment width h, its scalar Bezier controls are:

```
P0 = y[i]
P1 = y[i]   + (h*m[i])/3
P2 = y[i+1] - (h*m[i+1])/3
P3 = y[i+1]
```

In exact arithmetic the two normalized tangents lie in [0,1]; the interior
control offsets sum to at most 2/3 of the y span. The controls are ordered in
the direction of the segment secant, so the derivative's quadratic Bernstein
terms have that sign. The ideal curve is monotone within each individual
segment, has no segment overshoot, is C1 at knots, and is exactly flat on flat
segments. Arbitrary y ordering does not imply global monotonicity. The ideal
segment derivative magnitude is at most 1.5 times the absolute secant. C1 and
shape claims refer to the ideal function; rounded coefficients/evaluation and
final float32 quantization have separately characterized error.

This is a conservative known limiter selected for this engine. It is not
weighted-harmonic PCHIP, a novelty claim or an Adobe/editor equivalence claim.
[Fritsch and Carlson (1980)](https://doi.org/10.1137/0717021),
[Fritsch and Butland (1984)](https://doi.org/10.1137/0905021) and official
[SciPy PCHIP documentation](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.PchipInterpolator.html)
provide interpolation background. No implementation code was incorporated and
no SciPy runtime is introduced. The independent real-arithmetic proof and
selected rational checks cover the specified minmod rule itself.

## Extended curves: ordered native mapping

All scalar work and stored derived coefficients use binary64, with separate
operations, no contraction, reassociation, fast-math or table approximation.
For each original channel x, apply the master scalar curve first, then that
channel's scalar curve. Keep the intermediate as binary64. Check finite
intermediates, but do not reject a master value solely for exceeding float32:
the channel curve may bring it back into range. Only the final composed value
is checked against finite float32 maximum and cast once, with finite-cast check.

Per-curve evaluation is frozen as follows:

```
if all knots have x == y: return the input scalar unchanged
find the first knot with x greater than the input (upper_bound)
left = first index if before all knots, otherwise previous knot index
if input == knots[left].x: return knots[left].y
segment = min(left, knot_count-2)
if interpolation == Linear or input is outside the knot range:
    if secant[segment] == 0: return knots[left].y
    return knots[left].y + (input-knots[left].x)*secant[segment]
t = (input-knots[segment].x)/(knots[segment+1].x-knots[segment].x)
blend(a,b,t):
    if a == b or t == 0: return a
    if t == 1: return b
    if t <= .5: return a + t*(b-a)
    return b + (1-t)*(a-b)
A = blend(P0,P1,t); B = blend(P1,P2,t); C = blend(P2,P3,t)
return blend(blend(A,B,t),blend(B,C,t),t)
```

Precompute secants/tangents/controls in the exact ordered forms above.
Outside the range use endpoint-secant linear extrapolation, anchored at the
first/last saved knot; never extrapolate the cubic or clip to an endpoint.
Identity takes precedence over saved signed-zero knot signs: numerical x==y
identity preserves the input's sign bits. Otherwise exact knot equality returns
the saved binary64 y. A component whose master and channel curves are both
identity preserves its original float32 bits directly.

This master applies the same scalar map separately to R/G/B. It can alter RGB
ratios, signs and perceived hue. It does not preserve luminance/chromaticity;
the separately frozen tonal-range node supplies a different common-gain edit.
Reversing master/channel order or casting between them changes the operation.
For example, the retained draft fixtures at input .25 give .2109375 master-first
versus .21875 channel-first. At input .5029296875, one final float32 cast gives
.4401211142539978 versus .4401211440563202 with an intermediate cast.

The coefficient bounds keep work finite in binary64 for finite float32 input:
master magnitude is below 2^145 and final channel work below 2^162; interior
controls remain on the bounded knot scale. These conservative bounds do not
prove finite float32 output. Endpoint-anchored cancellation can lose relative
accuracy near zero: an extrapolated exact 2*x at the minimum normal float32
returns zero in a retained nearest-rounding example with knots near x=1.
This prohibits a universal relative/ULP accuracy promise. It is not hidden by
clipping, an epsilon bypass or changing the existing curve implementation.

## Gamma levels: admission and ordered mapping

For every channel, the four endpoints are finite with magnitude at most 65536.
Require input_white > input_black and output_white >= output_black.
Compute h=input_white-input_black in binary64; require h >= 1/65536.
The affine secant `(output_white-output_black)/h` must be finite and <=65536.
Gamma is finite in [.25,4], default1. These rules apply even for constant output
or identity. The positive input-width bound controls normalization; it does
not bound the derivative of the signed power near zero.

Given a,b,c,d = input_black,input_white,output_black,output_white,
and precomputed h=b-a and s=(d-c)/h:

```
if gamma == 1 and a == c and b == d: preserve original input float32 bits
if double(input) == a: mapped = c
else if double(input) == b: mapped = d
else if gamma == 1:
    if double(input) > b: mapped = d + (double(input)-b)*s
    else: mapped = c + (double(input)-a)*s
else if c == d: mapped = c
else:
    u = (double(input)-a)/h
    if u == 0: mapped = c
    else:
        power = 1 if abs(u) == 1 else pow(abs(u),1/gamma)
        v = copysign(power,u)
        mapped = c + (d-c)*v
require finite mapped and abs(mapped) <= finite float32 max
cast once to float32; require finite rounded result
```

The gamma1 affine branch intentionally uses the existing LevelsNode's exact
anchor, secant and operation order, including active signed-zero arithmetic.
Do not replace that branch with normalization/power or a constant-output
shortcut: doing so changes zero signs. A saved endpoint comparison follows
identity and precedes active arithmetic. Active gamma's constant output and
normalized zero return c exactly; abs(u)==1 avoids libm for the exact signed
unit value. Other active zero signs follow the specified IEEE operations.
There is no global promise to preserve input zero signs for active edits.

Signed power defines negative normalized values without a complex result.
Signed input and normalized values outside [0,1] are retained, so black/white
endpoints are anchors rather than clamps. The ideal power is continuous and
monotone; output is nondecreasing, constant when c==d. For gamma>1 its ideal
derivative is unbounded at normalized zero. It can amplify small variations and
grain there; no noise, highlight recovery or perceptual quality guarantee.
For gamma<1, headroom grows strongly and can exceed float32; reject overflow.

Finite float32 input, h>=2^-16 and exponent in [.25,4] bound |u| below 2^145,
power below 2^580 and active mapping work below 2^598. There is no binary64
overflow under these admitted bounds, but final float32 overflow remains a
necessary guard. Tiny arbitrary input spans would invalidate the normalization
bound and are rejected, including constant-output settings. Pow accuracy and
underflow depend on the target runtime; they are measured separately.

## Validation, levels, resources and graph identity

Before any identity or constant bypass, validate the supported render level,
nonempty rectangle with exclusive x/y endpoints <=2^32, then the upstream
tile's exact bounds, descriptor, storage count and ALL finite input samples.
Fetch exactly one upstream tile at the requested level and modify that owned
returned tile in place. Invalid input/output returns no partial tile.

Support mip0 Final/Preview and mip1/2 Preview iff input supports that level.
Point support is the requested rectangle, without a halo or spatial scratch.
Upstream reduction occurs BEFORE these nonlinear edits. There is no native-map-
then-average substitution or special full-frame algorithm. Translation, uneven
tiles, geometry composition, RAW after calibrated working conversion and
multisource composition must follow the existing source-support contracts.

At most 1024 total curve knots are copied. Worst-case logical numeric persistent
tables are 16,384 knot bytes, 8,160 secant bytes and 16,320 interior-control bytes,
plus flags/containers/input/settings metadata. Construction can use at most one
256-element tangent vector (2,048 logical bytes) at a time. These are numeric
payload bounds, not allocator/capacity/ABI/peak-copy guarantees. Linear mode may
omit cubic control storage. Mapping uses bounded binary search and fixed scalar
work; no retained image, image-sized double buffer or mutable processing cache.
Gamma levels uses 15 copied binary64 parameter values and bounded per-channel
derived scalars, no dynamic table. Concurrent renders use separate local tiles.
Measure parameter validation/copy, signature, lookup, six possible scalar curve
maps or three powers per RGB pixel, tile copies/cache/API independently under
PERF-035 before optimizing. 45MP, allocator and concurrency costs remain open.

Saved types are distinct `rawengine.curves_extended` and
`rawengine.levels_gamma`, each schema1/processing2. Exact REQUIRED parameters:

```json
{"master":[[0,0],[1,1]],"red":[[0,0],[1,1]],
 "green":[[0,0],[1,1]],"blue":[[0,0],[1,1]],
 "interpolation":"shape_preserving_cubic"}

{"input_black":[0,0,0],"input_white":[1,1,1],
 "output_black":[0,0,0],"output_white":[1,1,1],"gamma":[1,1,1]}
```

Interpolation accepts exactly `linear` or `shape_preserving_cubic`. Knot pairs
have exactly two numeric entries; gamma/endpoint arrays have exactly three.
Reject missing/extra keys, booleans/non-numbers/nonfinite/out-of-range values,
invalid token, bad dimensions and schema/process changes even when disabled.
Enabled execution requires one image input, matching supported scene-linear
domains, normal blend, opacity1, no masks/extensions. Disabled operations retain
existing strictly validated bypass/domain semantics. No manifest-format bump.
Source/upstream/type/schema/process/exact binary64 values and interpolation,
domains/extent/rectangle/mip/quality remain in graph/cache identity even for
identity. Python reaches these controls through explicit saved manifests on
existing RAW/raster/multisource render, jobs/latest, history, analysis/histogram,
ownership and source-region paths. No viewer or new mutable session option.

The host rounding mode affects active results and derived coefficients; no
rounding-mode or flush-policy mutation is introduced. Strict MSVC probes cover
four modes. Near finite float32 maximum, a directed-rounding intermediate can
cross the final guard although the ideal composed curve lies at the maximum.
Guard the actual ordered binary64 result. Identity remains bit-exact in all
modes. Cross-runtime/libm/FTZ policy and production platform release remain open.

## Pre-native evidence and required acceptance

`build-msvc-release/research/curves-levels-scope-v1/` retains 2,451 exact rational
Bezier/Hermite and derivative-sign checks on 19 segments, plus endpoint,
extrapolation, identity, master/order/cast and centered-tangent overshoot cases.
The naive flat-segment value 1.046875 contrasts with the selected exact 1.

`research/curves-levels-precision-v1/pass-v2/` retains 40 curve compositions,
24 gamma settings and 20,585 finite rational/180-digit references, plus
nonfinite input cases. Strict MSVC19.40 /O2 /fp:strict prototype passes 83,364
records over four rounding modes with 16 invalid-setting gates. Nearest gamma
outputs match exact integer-rounded high-precision float32 truth; other modes
differ by at most one ULP on selected gamma cases. Curve acceptance uses a
magnitude/conditioning-scaled absolute-error bound, not a universal ULP bound.
Worst measured error/bound ratio is .011733; selected samples do not establish
a universal libm error bound. Four directed-rounding final guard differences
and explicit near-zero curve cancellation examples are retained.

The corrected gamma1 prototype matches current native LevelsNode exactly on
4,176 channel results in both spaces/four modes, with 264 expected nonfinite or
overflow rejections. Additional 32 exact binary64 knots and 160 binary64
identity checks pass. Signed-gamma and spline transfer/derivative plots were
inspected: gamma>1 steepens near zero, minmod flattens extrema/flat segments and
avoids the naive centered-slope overshoot. These are mathematical diagnostics,
not photographic/profile qualification. Research fixture/counter corrections
are recorded in the checkpoint; successful artifacts are immutable.

Before implementation, archive this frozen contract, current sources/native
binaries, full/default/core/LCMS/LCMS+Python evidence and independent fixtures.
Native acceptance must replay independent polynomial/high-precision/extreme/
identity/rounding/rejection evidence; tile/ROI/mip/reduction order, strict saved
validation, direct/saved bindings, cache, RAW/multisource/geometry, jobs/history/
analysis/source ownership; selected camera artifacts and isolated API cost;
installed consumers and all four full build/test configurations. Existing
controls' frozen fixtures must remain unchanged. Production photographic/
profile/corpus, portability/libm, release, 45MP/allocator/concurrency and broader
Phase 4 signoff remain open. No personal preferred edits are required and Adobe
compatibility is a separate qualification.

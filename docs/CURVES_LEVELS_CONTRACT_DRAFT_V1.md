# Master curves, shape-preserving spline and midtone levels: draft v1

Status: research proposal, 2026-10-03. Not frozen or native.
Continue the existing Phase4 curves/levels expansion row. Existing `curves`,
`levels`, `grading`, LUT and tonal-range schemas, limits and arithmetic remain
unchanged. No new dependency, UI, headline checkbox, commit or push.

## Domain and intended behavior

Both proposed controls operate on signed scene-linear RGBFloat32 in declared
ProPhoto/D50 or Rec.2020/D65, after requested-level input reduction. No implicit
encoding, white balance, profile selection, clipping or gamut mapping. They are
point maps with copied immutable parameters. Existing levels continues to supply
its exact affine contract; existing curves continues to permit piecewise-linear
creative/inverting knots and signed endpoint extrapolation.

The new master curve applies the same scalar curve independently to R/G/B;
per-channel curves then operate on those intermediate scalars. This can alter
linear RGB ratios and perceived hue, as expected of channel editing. It is not
a luminance/chromaticity-preserving master curve. The existing tonal-range node
provides a separately defined common-gain luminance edit. Order and intermediate
precision must be explicit rather than inferred from a familiar control name.

## Shape-preserving cubic research

[Fritsch and Carlson (1980)](https://doi.org/10.1137/0717021) studies monotonicity
conditions for cubic interpolation. [Fritsch and Butland (1984)](https://doi.org/10.1137/0905021)
describes a local monotone cubic method. Official
[SciPy PCHIP documentation](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.PchipInterpolator.html)
documents a weighted-harmonic slope policy and C1/no-overshoot behavior.
These are mathematical background only; no source implementation was inspected
or copied and no SciPy runtime is proposed. The candidate below selects a
conservative minmod slope rule; it must not be called that PCHIP algorithm.

For strictly increasing x knots and finite y, let d_i=(y_(i+1)-y_i)/h_i,
h_i=x_(i+1)-x_i. Endpoint tangents equal the adjacent secant. At each interior
knot, set m_i=0 when either neighboring secant is zero or their signs disagree;
otherwise use their common sign times min(abs(d_(i-1)),abs(d_i)). This admits
arbitrary creative y ordering and flattens a local extremum rather than banning
it. On each individual segment, both normalized endpoint slopes are in[0,1].

Represent the Hermite segment as a cubic Bezier with scalar controls
`[y0,y0+h*m0/3,y1-h*m1/3,y1]`. They are ordered in the direction of its secant:
the two interior offsets sum to at most2/3 of the segment's y span. The derivative
is a quadratic Bernstein combination of nonnegative (or nonpositive) control
differences, proving segment monotonicity and no overshoot in real arithmetic.
Shared knot tangents prove C1 continuity. Flat segments have zero endpoint
tangents and remain exactly flat. This is a conservative policy selected for
this engine, not a claim of novelty, optimal accuracy or equivalence to standard
weighted-harmonic PCHIP.

Use explicit endpoint-tangent LINEAR extrapolation outside the knot range,
not extrapolated cubic polynomials or endpoint clipping. Preserve exact saved
knots and all-x==y identity bits. Master precedes the selected channel curve,
with binary64 intermediate and one final float32 cast proposed. Composing two
old CurvesNodes introduces an intermediate float32 cast and is a different
operation. Proposed interpolation choices are `linear` and
`shape_preserving_cubic`, shared across master/channels. Proposed saved type
`rawengine.curves_extended` uses distinct schema1/process2; exact parameters,
missing/extra/disabled validation and binary64 identity require freezing.

## Midtone gamma levels research

Per channel, normalize x using explicit input endpoints, apply a signed gamma
power, then map to explicit output endpoints:

```
u = (double(x)-input_black)/(input_white-input_black)
v = copysign(pow(abs(u),1/gamma),u)
mapped = output_black + (output_white-output_black)*v
```

Gamma1 proposes a separate exact affine path; input/output identity endpoints
and gamma1 must bypass float32 bits after full input validation. Exact normalized
zero/one need explicit endpoint handling. Signed values and headroom are not
clamped to[0,1]. Positive finite gamma in[.25,4] matches existing grading bounds
but the new levels normalization/offset semantics remain distinct. Gamma>1
has an unbounded ideal derivative at normalized zero; continuity alone does
not justify a no-noise-amplification claim. Strong gamma on headroom can exceed
float32 and must reject rather than clip. Libm precision and active signed-zero
behavior require independent high-precision and native characterization.

Proposed type `rawengine.levels_gamma`,schema1/process2,with exactly the four
existing three-channel endpoint arrays plus a three-channel `gamma` array.
Endpoint limits, tiny input-span admission/reciprocal conditioning, constant
output-span bypass, precise affine/power/offset order, exact knots/zeros/ones,
overflow checks and temporary storage are still UNRESOLVED. Do not implement
or freeze them by simply copying an old levels factory and adding pow.

## Gate order and integration

First retain independent rational Bezier/Hermite equivalence, derivative-sign
and no-overshoot checks over flat/extreme/nonuniform knots, exact endpoints,
identity and linear extrapolation, master/channel-order and intermediate-cast
counterexamples. Inspect transfer/derivative plots. Contrast minmod versus a
naive centered tangent with a concrete overshoot counterexample and record the
conservative rule's accuracy/shape tradeoff. Do not use a native direct/saved
comparison as the independent mathematical oracle.

Then settle signed levels gamma with Decimal/integer special-case truth and
precision/domain/conditioning/artifact bounds before freezing. Choose full
curve/table/control limits, strict enabled/disabled schema/domain/version
admission, immutable settings/cache signatures, point ROI/upstream footprints,
native/mip order, RAW/multisource/geometry composition and Python jobs/history/
analysis/ownership/resource policies. Preserve old curve/levels replay exactly.

Only after a frozen contract and pre-native archive should new nodes/types be
implemented. Acceptance includes independent native/Python rational/high-
precision/extreme/identity/rounding/rejection gates, tile/ROI/mip/cache/source-
support, RAW and mixed-source behavior, jobs/history/analysis/lifetimes,
diagnostic camera artifacts, installed consumers and the four full build/test
configurations. Production photographic/profile/corpus, cross-runtime, 45MP,
allocator/concurrency and broader Phase4 signoff remain open. Personal preferred
edits are not acceptance requirements; Adobe comparisons remain separate.

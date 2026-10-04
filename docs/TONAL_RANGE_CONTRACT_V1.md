# Original highlights, shadows, whites and blacks contract v1

Status: frozen before native implementation, 2026-10-03.
This addresses the existing Phase 4 tone-control requirement. No native node is
implemented at this checkpoint. No new headline checklist row, dependency, UI,
default, commit or push. Production/profile/corpus qualification remains open.

The intended controls brighten/darken selected luminance ranges, preserve a
monotone neutral ramp, preserve finite signed/headroom data and preserve working
RGB chromaticity before rounding. All four controls must work together over
their full admitted range. No automatic scene statistics, local detail recovery,
sensor highlight reconstruction, black-floor lift or display gamut mapping is
implied. The existing levels node controls explicit black/white endpoints.
Representative photographic/profile qualification remains a separate gate;
the implementation requirement will not be checked off for a prototype alone.

## Research boundary and provenance

[Reinhard et al., Photographic Tone Reproduction (2002)](https://www-old.cs.utah.edu/docs/techreports/2002/pdf/UUCS-02-001.pdf)
distinguishes global luminance mapping from spatial adaptation and identifies
contrast/clamping artifacts. Its operator targets display reproduction; that
does not establish the semantics of a scene-linear editing control. None of its
code or mapping equations is used by the candidate below.
[W3C CSS Color 4](https://www.w3.org/TR/css-color-4/#color-conversion-code)
provides primary color-conversion context: luminance belongs to linear XYZ and
depends on the declared RGB space. Existing engine working-Y coefficients are
retained rather than applying sRGB weights to ProPhoto or Rec.2020.

The compact polynomial warps, interval choices, composition, signed extension
and arithmetic below are original design choices. The cited work supports the
domain/artifact questions, not the selected intervals or a photographic-quality
claim. No editor source, undocumented Adobe formula or third-party code was
consulted or incorporated. A production contract must explicitly expose these
semantics rather than present four familiar names as evidence of equivalence.

## Frozen curve and controls

Four finite binary64 amounts lie in [-1,1], default0. They are normalized
brightness controls, not EV stops or sensor recovery strengths. Positive amounts
move positive luminance upward within the selected interval; negative amounts
move it downward. The fixed intervals, in scene-linear working-Y units, are:

| Control | Interval [a,b] | Maximum signed displacement at its midpoint |
| --- | --- | --- |
| blacks | [0,1/8] | 1/32 |
| shadows | [0,1/2] | 1/8 |
| highlights | [1/8,2] | 15/32 |
| whites | [1/2,8] | 15/8 |

For a current nonnegative scalar x inside an open interval, define

```
t = (x-a)/(b-a)
B(t) = 16*t*t*(1-t)*(1-t)
W(x;amount,a,b) = x + amount*(b-a)*B(t)/4
```

At/outside either endpoint, or for amount0, W returns x exactly. Apply blacks,
shadows, highlights, whites in this fixed order, each to the previous result.
This is an ordered composition, not a weighted sum sampled from the original
luminance. Reordering changes overlap behavior and cannot be silently optimized.
Above8 the whole curve is identity; no display clipping occurs. Black remains0.
Whites shapes the upper range rather than promising to move a specific output
white point. Shadows/highlights have no neighborhood support or halo and cannot
recover detail that input clipping already destroyed.

Let Q(x) be the composed nonnegative map. Extend it to all signed Y by
F(Y)=sign(Y)*Q(abs(Y)), F(0)=0. Negative luminance is a numerical extension,
not physical radiance: positive amounts increase its magnitude. For finite
scene-linear RGB use the existing centered native-Y evaluation
Y=G+wr*(R-G)+wb*(B-G), in declared ProPhoto/D50 or Rec.2020/D65.
Use gain=Q(abs(Y))/abs(Y) when Y!=0, and gain1 when Y==0. Multiply every original
channel by the same positive gain in binary64 and round once to binary32.
No epsilon, independent per-channel curve, implicit gamut operation or clamp.

All amounts0 is an exact whole-tile bypass **after** complete finite and tile
validation. Gain1 is an exact pixel bypass. Candidate arithmetic must keep
neutral/channel signs and signed zero bits in these bypasses. Active zero and
subnormal rounding is subject to the pinned target floating-point policy; any
output not representable as finite float32 must reject rather than clip.
The neutral-axis math is monotone; ties from final float32 rounding are allowed.
Ratio/chromaticity preservation is mathematical, with final-rounding error
quantified separately. It is not a perceptual hue or gamut guarantee.

## Mathematical admission

B'(t)=32*t*(1-t)*(1-2*t); its absolute maximum is16*sqrt(3)/9.
Each warp derivative therefore lies in [1-4*sqrt(3)/9,1+4*sqrt(3)/9],
approximately[0.23019964,1.76980036], for every admitted amount. Each warp is
strictly increasing, keeps its endpoints, stays in its own interval and joins
the identity with derivative1. Composition is C1 and strictly increasing;
its real derivative is bounded by the fourth powers of those positive bounds.
The odd extension joins with derivative1 at0. The gain tends to1 at0, so a
division threshold would introduce an unnecessary discontinuity.

For a>=0 and x inside the interval, width*t/x<=1. Thus each stage's ratio
lies in [11/27,43/27], using max(4*t*(1-t)^2)=16/27.
The composite ratio is within their fourth powers, a positive bound below8.
This supports signed RGB ratio preservation and bounds binary64 work for
finite binary32 input. It does not prove finite binary32 output; validate that
output separately. These real bounds do not prove every chosen ordered binary64
evaluation, cancellation case, target libm policy or final float32 rounding.
Independent rational/extreme/near-endpoint tests are required before freezing.

## Alternatives and explicit tradeoffs

An additive luminance offset changes RGB ratios and can create channel sign
changes; it remains a valid different artistic contract, not this candidate.
A directly weighted exposure field can reverse a ramp at strong opposing
settings; a concrete counterexample must be retained rather than relying on
nice-looking default plots. Integrating a positive slope guarantees monotonicity
but changes higher luminances after a lower-range edit; compact endpoint-anchored
warps instead restore the identity outside each range. Ordered overlap and
contrast amplification/compression remain visible behavior, not defects hidden
by a qualification statement. Deep black cannot be lifted from exactzero.

## Exact arithmetic and exported interface

`TonalRangeSettings` copies four binary64 fields in order `blacks`, `shadows`,
`highlights`, `whites`, each finite[-1,1], default0.
`validate_tonal_range_settings(settings)` validates all four even for identity.
`TonalRangeNode(shared_ptr<const Node> input, settings={})` copies immutable
settings and retains the input. Null input and any descriptor other than
RGBFloat32 scene-linear ProPhoto/D50 or Rec.2020/D65 reject. Output preserves
descriptor and requested extent. Input transfer/profile is never inferred.

The ordered native mapping uses separate binary64 operations, without
contraction/reassociation/fast-math or epsilon thresholds:

```
validate level and nonempty rectangle, exclusive x/y endpoints <= 2^32
fetch one upstream tile at that level
validate its bounds, descriptor, exact storage and ALL finite samples
if all four controls == 0: return original tile bits
for each original float32 RGB triple:
    r,g,b = double(original R),double(original G),double(original B)
    Y = (g + wr*(r-g)) + wb*(b-g)
    x = abs(Y)
    mapped = x
    for (amount,a,b) in blacks/shadows/highlights/whites order:
        if amount == 0 or mapped <= a or mapped >= b: continue
        t = (mapped-a)/(b-a)
        u = 1-t
        scale = (amount*(b-a))*4
        bump = (t*t)*(u*u)
        mapped = mapped + scale*bump
        require finite mapped
    gain = 1 if x == 0 else mapped/x
    require finite Y,mapped,gain and gain > 0
    if gain == 1: preserve original triple bits
    for each original channel c:
        value = double(c)*gain
        require finite value and abs(value) <= finite float32 max
        cast ONCE to float32, require finite rounded result
```

Wr/Wb are the existing once-rounded binary64 values:
ProPhoto/D50 `(0.28807112822929337,0.00008565396060525903)` and
Rec.2020/D65 `(0.26270021201126703,0.059301716469861945)`.
The centered form makes neutral Y exactly its input scalar. Do not substitute
a separately rounded green coefficient, resample a LUT, reorder overlapping
warps or use independent channel curves. Amounts nearzero may round away
naturally but must never be discarded heuristically.

Reject invalid inputs/outputs with no returned partial tile. Complete tile
validation precedes any whole-tile/pixel bypass. Underflow follows target
float32 rounding;no explicit flush,clamp or rounding-mode mutation is introduced.
The host's rounding environment can affect active results. The selected MSVC
strict-FP probes cover four modes;portable runtime and flush policy remain
qualification gates,not an assertion of cross-runtime bit identity.

## Support, resource and graph identity

Point support equals the requested output rectangle;no halo,global analysis,
frame statistics or spatial scratch. Support mip0 Final/Preview and mip1/2
Preview iff the upstream supports that level. Fetch/reduce the upstream at the
requested level BEFORE the nonlinear tone map. Native-map-then-average is a
different operation and must not replace this order.

The node owns copied32-byte logical settings,two binary64 weights,identity flag
and one input pointer;mapping uses fixed scalar work. It modifies the validated
returned float32 RGB tile in place and adds no pixel vector or retained frame.
Tile storage/count/bounds use the existing addressability checks;pixel count
is compared using division to avoid a width*height*3 overflow. Node metadata,
source/upstream/cache/caller/scheduler/allocator/runtime storage is not a pixel
scratch cap. Simultaneous calls have distinct local returned tiles. No mutable
processing cache is introduced;settings/space determine the whole mapping.

Saved type `rawengine.tonal_range`,schema1/processing2,has exactly four REQUIRED
parameters:

```json
{"blacks":0.5,"shadows":-0.75,"highlights":0.25,"whites":-0.5}
```

Reject missing/extra keys,booleans/non-numbers,nonfinite/out-of-range values,
schema or process changes even when disabled. Enabled graph execution requires
one image input,matching supported scene-linear input/output domains,normal
blend,opacity1,no masks/extensions. Disabled operations retain existing strictly
validated bypass/domain semantics;they do not activate a new descriptor path.
Source/upstream/type/schema/process/exact binary64 parameters/domain/extent/
rectangle/mip/quality remain in graph/cache identity,including zero controls.
No manifest format bump,existing saved type change or default pipeline insertion.
Python reaches the node through explicit manifests on existing RAW/raster/
multisource synchronous/jobs/latest/history/analysis/source-footprint paths.
This does not add viewer controls or mutable one-shot session configuration.

## Pre-native evidence and acceptance

The original Python scalar study checks83 full-corner/fractional settings,
14193 exact rational scalar comparisons and420 rational RGB triples. All
selected final float32 results match;maximum binary64 scalar error8.8817842e-16.
2,988,000 sampled slopes stay positive,830 endpoint and126 cancellation-limit
probes pass. Analytic bounds above cover real arithmetic;sampled ranges do not
replace those proofs. Transfer/derivative/gain plot was inspected. Naive weighted
exposure maps .375->.5538098 and .38->.5495747,reversing that ramp;the selected
warp does not. At input.7,amounts(1,1,-1,1),forward/reversed control order differs
(.36093843/.44353571),so reordering is materially incorrect.

Standalone MSVC19.40 Release /O2 /fp:strict prototype uses21,580 fixtures,
83 settings,both spaces,neutral/signed/subnormal/extreme/cancellation RGB and
independent integer/rational ties-to-even binary32 rounding. Normal rounding
matches63,246 channels exactly;down/up/toward-zero each stays within1 ULP of
that reference. Every mode rejects498 nonfinite-input cases,passes169,984
positive slope checks and rejects4 direct output-guard overflow/nonfinite
values. The full-curve RGB fixtures produce no actual output-overflow case;
this is not claimed as end-to-end overflow coverage. Runtime guards remain
required because a real finite binary64 result need not be finite float32.
Mip-order counterexamples differ by.15553826/.10323942 at factors2/4.
Current engine rejects the future saved type in both spaces,enabled/disabled.

72 repeatable diagnostic camera prototypes use one Nikon high-ISO file,two
demosaicers,two working spaces and three crops;12 prior calibrated inputs
remain byte-identical. Maximum relative red/green cross-product error5.8079e-8
comes from final float32 rounding. Contact overview and two native crop boards
were inspected. Strong lifting makes existing chromatic grain more visible;
darkening suppresses visible brightness/detail and overlap can strongly
compress contrast. This is no NR/profile/Adobe/representative-quality proof.

Immutable local reports: `build-msvc-release/research/tonal-range-scope-v1/`,
`tonal-cpp-probe-v1/`,`tonal-prefreeze-boundaries-v1/` and
`tests/rawfiles/_librawops_local/high-iso/tonal-range-prototype-v1/`.
Helper/fixture/source/native/plot/board hashes bind these observations.

Native acceptance requires exact replay against these frozen C++/rational
fixtures;independent math/extremes/identity/rounding/input-validation and
rect/descriptor/settings rejection;full/tiled/ROI/native/mip/source-support;
strict saved parameter/type/version/domain admission;RAW/multisource/geometry
composition;cache/analysis/jobs/history/lifetime;camera/artifact parity;fresh
installed C++/Python consumers and all four existing build/test configurations.
Record PERF-034 native API/source/copy/scalar costs with logical/actual resource
distinctions. Representative/profile-aware quality,45MP/concurrency/allocator,
portable rounding and broad Phase4 production signoff remain explicit gates.

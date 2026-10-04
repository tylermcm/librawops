# Highlights, shadows, whites and blacks: original candidate v1

Status: research draft, 2026-10-03. Not frozen, implemented or production-qualified.
This addresses the existing Phase 4 tone-control requirement. No new checklist
row, native node, dependency, UI, default, commit or push accompanies this study.

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

## Candidate curve

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

## Pre-freeze and native acceptance gates

First retain exact polynomial/derivative/ratio proofs, independent rational
scalar and RGB truth, full corners plus fractional controls, dense monotone
ramps, endpoint continuity, neutral/signed/cancellation/subnormal/extreme tests,
counterexamples, and inspect rendered transfer/derivative/ratio plots. Evaluate
fixed diagnostic camera patches for color/detail interactions without calling
them representative profile qualification. Record cost before selecting native
arithmetic. Keep all previous accepted kernel/native/reference artifacts intact.

Before freezing, resolve the interval/strength/interface decision, precise
operation order and intermediate validation, native/API limits and versioned
saved type. Proposed implementation is an immutable point node in the two
scene-linear spaces, point footprint, requested-level input before nonlinear
mapping (distinct from native-map-then-average), exact cache/signature identity,
copied settings and no neighborhood scratch. Both enabled and disabled manifests
must strictly validate schema/process/parameters/domain, with unsupported domains
and nonfinite amounts rejected. This is a proposal, not a registry addition.

Native acceptance also requires independent direct/saved/Python comparison,
ROI/tile/mip/source-footprints, RAW and multisource composition, analysis/cache/
jobs/history and lifetime/rejection gates, frozen camera truth/artifact inspection,
fresh installed consumers and all four existing build/test configurations.
Per-pixel arithmetic, copying/allocator/full-frame/45MP/concurrency costs and
portable rounding remain measured gaps. Broad production quality still requires
the declared representative/profile-aware corpus, not preferred personal edits.

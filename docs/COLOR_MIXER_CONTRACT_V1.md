# Original bounded hue/color mixer contract v1

Status: **functional bounded native foundation; production color qualification open**.
This is the first bounded color-mixer component of the HSL/color-control roadmap.
It defines eight hue bands with hue, chroma and native working-Y adjustments in
scene-linear RGB. The engine executes `rawengine.color_mixer` as an opt-in saved operation.

The domain choice is deliberate. [W3C CSS Color 4](https://www.w3.org/TR/css-color-4/#hsl-to-rgb)
defines its HSL representation and conversions around sRGB. This operation instead
uses the engine's scene-linear working descriptor and pinned native-Y coefficients.
The selector, band layout, control equations and neutral fade below are original
engine policy. Standard display-HSL lightness/saturation, perceptual/skin behavior
and Adobe parameter equivalence remain separate qualification work. No external
implementation, dependency or model is selected by this reference.

## Domain and settings

Exports: `ColorMixerSettings`, `validate_color_mixer_settings` and
`ColorMixerNode` in ToneOps.hpp/.cpp. Copy settings into immutable node state.
Input/output is finite float32 scene-linear ProPhoto/D50 or Rec.2020/D65 RGB
with an unchanged descriptor. Accept negative channels and highlight headroom.
There is no clipping, normalization, transfer conversion or working conversion.

Three arrays each contain exactly eight binary64 numbers, all defaulting to zero:

| Parameter | Per-band range | Meaning |
| --- | --- | --- |
| `hue_shift` | [-60,60] degrees | Add the blended shift to the original selector hue |
| `saturation_delta` | [-1,1] | Scale rotated native-Y chroma by 1 + blended delta |
| `luminance_delta` | [-1,1] | Fractional native-Y change, faded toward neutrals |

All numbers must be finite. Fixed band order and center angles are:

| Index | Label | Center in working RGB |
| --- | --- | --- |
| 0 | Red | 0 degrees |
| 1 | Orange | 30 degrees |
| 2 | Yellow | 60 degrees |
| 3 | Green | 120 degrees |
| 4 | Aqua | 180 degrees |
| 5 | Blue | 240 degrees |
| 6 | Purple | 270 degrees |
| 7 | Magenta | 300 degrees |

These names identify original numeric anchors; they do not certify perceptual
color names across working spaces or duplicate another product's ranges. Bands
and their widths are fixed in v1. There is no master slider, range editor, skin
selector, monochrome toggle or selective-color operation. The optional local
viewer exposes the fixed bands through a small selector/three-slider panel.

## Validation and original hue

Validate tile bounds, storage length, descriptor and all three finite samples
before any bypass. If all 24 settings are numerically zero, copy the original
float32 bits. If `R == G && G == B` numerically, also copy every original bit
regardless of settings, including unequal zero signs and extreme finite neutrals.
These hue-selective controls do not edit achromatic pixels.

For a remaining pixel, promote samples to binary64 and calculate:

```text
M = max(R,G,B)
m = min(R,G,B)
C = M - m
```

C is positive without an epsilon. Resolve ties by testing R first, then G,
then B. The hexagonal selector hue in degrees is:

```text
if M == R: h = 60 * ((G - B) / C)
else if M == G: h = 60 * (((B - R) / C) + 2)
else: h = 60 * (((R - G) / C) + 4)
h = wrap(h)
```

For the bounded angles produced here and after a hue shift, `wrap` adds 360 if
negative, subtracts 360 if >=360, and canonicalizes numeric zero to positive
zero. Its result is in [0,360). No modulus of image values or gamut clamp occurs.
Hue is invariant mathematically under a shared RGB offset and positive RGB
scale; actual binary64/float32 rounding remains part of numerical validation.

## Adjacent band weights

Use the original h once; do not reselect bands after applying hue or chroma.
Find adjacent centers on the ordered list
`[0,30,60,120,180,240,270,300,360]`. The final 360-degree entry refers back to
red/index0. Use upper-bound selection so an exact center selects the segment
starting there. For the selected lower/upper angles a/b:

```text
t = (h - a) / (b - a)
w0 = 1 - t
w1 = t
```

Only these two bands contribute. All intervals have positive width; weights
are nonnegative and sum to one mathematically. This forms piecewise-linear
overlap around the circle, including magenta-to-red wrap. For each parameter
array p, use the following frozen blend helper:

```text
B(p0,p1,t):
    if t == 0: return p0
    if t == 1: return p1
    if p0 == p1: return p0
    return (1 - t) * p0 + t * p1
```

Bypass order is significant. Exact center settings and equal neighboring settings
retain their saved binary64 value. The three blended parameters are H, D and L
for hue shift, saturation delta and luminance delta. There is no approximate
identity tolerance, iterative selection, accumulation over all eight bands or
averaging of absolute hue angles. Blend the bounded signed shifts themselves.

## Native Y, neutral fade and luminance target

Reuse the coefficients already pinned by saturation/vibrance, without changing
their implementations. For any binary64 RGB vector v:

```text
Y(v) = (vG + red_weight * (vR - vG)) + blue_weight * (vB - vG)
```

| Working descriptor | red_weight | blue_weight |
| --- | --- | --- |
| Scene-linear ProPhoto/D50 | 0.28807112822929337 | 0.00008565396060525903 |
| Scene-linear Rec.2020/D65 | 0.26270021201126703 | 0.059301716469861945 |

Compute in strict binary64 without multiply-add contraction:

```text
Y0 = Y(input)
fade = C / (abs(Y0) + C)
S = 1 + D
K = 1 + L * fade
Ytarget = Y0 * K
```

No denominator epsilon is needed for non-neutral finite input. Mathematically
fade is in (0,1], S in [0,2] and K in [0,2]. The fade limits luminance changes
as chroma approaches zero around a nonzero neutral. Indeed
`abs(Ytarget - Y0) <= abs(L) * C` in exact arithmetic. This avoids a finite
luminance jump at the exact achromatic bypass. It is an original stability rule,
not a fitted saturation measure or photographic skin policy.

Negative Y is scaled with the same rule; zero-Y colors retain a zero luminance
target while hue/chroma may still change. `luminance_delta` is not exposure stops
or standard HSL lightness. Rounding can make K numerically one or Ytarget equal
Y0; the exact computed bypass below applies in those cases.

## Hue rotation and chroma mapping

If H is numerically zero, retain the original binary64 chroma directly:

```text
chroma[c] = input[c] - Y0
```

Otherwise set `h2 = wrap(h + H)`, `u = h2 / 60`, `sector = floor(u)` and
`f = u - sector`. Sector is an integer in [0,5]. Construct a zero-minimum RGB
hexagon vector q with the original channel range C:

| Sector | qR | qG | qB |
| --- | --- | --- | --- |
| 0 | C | C*f | 0 |
| 1 | C*(1-f) | C | 0 |
| 2 | 0 | C | C*f |
| 3 | 0 | C*(1-f) | C |
| 4 | C*f | 0 | C |
| 5 | C | 0 | C*(1-f) |

Compute `Yq = Y(q)` with the same working coefficients and order, then use:

```text
chroma[c] = q[c] - Yq
out[c] = Ytarget + S * chroma[c]
```

The final equation applies to both H branches. Recentring q preserves native
working Y mathematically, rather than preserving the RGB midpoint or minimum.
Before saturation, the rotated range is C; mathematically the final range is
S*C. At S=0 the result is neutral Ytarget. Without a luminance edit, target Y
is Y0; hue/chroma edits can still change channel signs or exceed one. Native Y
can differ after the single float32 cast by normal rounding. No exact floating
luminance-preservation, hue-uniformity or gamut-mapping promise is made.

Before mapping, if `H == 0 && S == 1 && Ytarget == Y0`, copy the original
float32 bits. This covers exact inactive ranges, exact cancellation between
neighboring parameters and computed zero-Y/unit-luminance cases. Every prior
finite/tile check still applies. Other edits use the stated equations; their
signed zeros do not have an additional bypass guarantee.

Validate all intermediates for finiteness and each final component for finite
float32 representability before casting once. Finite binary64 intermediates
above float32 range may cancel to a valid final output. Finite float32 source
samples and bounded controls fit binary64 arithmetic; final float32 overflow
can still occur and must reject. No silent clipping or parameter normalization.

## Saved graph and rendering

Saved type `rawengine.color_mixer`, schema1/processing2, has exactly:

```json
{
  "hue_shift": [0,0,0,0,0,0,0,0],
  "saturation_delta": [0,0,0,0,0,0,0,0],
  "luminance_delta": [0,0,0,0,0,0,0,0]
}
```

All arrays have exactly eight finite numeric scalars, excluding booleans. Reject
missing/extra/wrong-shaped/out-of-range values and unsupported versions even
disabled. Enabled execution requires one image input, matching
supported working domains, normal blend, opacity1 and no masks/extensions.
Disabled execution follows existing validated exact passthrough/domain behavior.
Enabled graphs execute the registered original bounded type.

Native final/preview and mip1/2 preview map the requested upstream float32 RGB.
Hue selection and nonlinear fade generally do not commute with native averaging.
There is no halo, geometry, source-reduction or descriptor change. Existing
geometry/conversion/mixed-source footprints, immutable ownership, cache,
jobs/latest/cancellation, saved history and read-only analysis are integration
paths. Arrays, type/version, upstream, actual descriptor and requested level
separate cache identity. Default recipes, earlier point edits and decoder remain
unchanged. Conventional display-HSL and selective-color execution are still open.

## Native implementation and qualification gates

1. Export the copied-settings node/validator and register the strict saved type.
   Pin compiler floating-point behavior and preserve existing working-Y constants.
2. Use independent exact-rational references for selectors, all centers/overlap
   intervals/wrap, asymmetric controls, tie rules, hue rotations and combined
   hue/chroma/luminance results. Include both working spaces, primary/secondary
   and offset/signed/headroom/zero-Y colors, near-neutral continuity, subnormals,
   maximum coefficients and near-boundary fixtures. Distinguish chroma scaling
   and native-Y changes from standard HSL saturation/lightness semantics.
3. Verify exact all-zero/neutral/inactive-range/effective-zero identity bits,
   including extreme finite and signed-zero values; tiny edits must execute.
   Reject malformed/nonfinite/bound/overflow/domain/version parameters even
   disabled. Test actual upstream tile bounds/storage/descriptors and ownership.
4. Verify native/mip/ROI/tile parity, a nonlinear map-after-reduction counterexample,
   no halo, geometry, actual working conversion, mixed-source footprints,
   settings-specific cache edits, jobs/latest/history and histogram/local analysis.
5. Archive current binaries/contract before rebuilding. Run full default/core/
   LittleCMS suites and inspect installed API/binaries/exports/notices. Use the
   existing ISO20000 background/skin ROIs, both demosaicers/spaces/native/mips,
   independent references, partition/identity/historical-input checks and inspected
   display boards. Bind evidence to sources/helper/native/decoded/ROI/runtime.
6. Measure warm-input uncached/cached 256-square API requests, seven repeats and
   64 MiB cache after full tests finish; record cache deltas, initialization and
   Windows process memory. PERF-018 records bounded API measurements and remaining
   hue-selection/blend/rotation/graph/copying profiling hypotheses. No cost,
   regression, kernel/full-frame/concurrent or allocation claim precedes evidence.
7. Update the authoritative handoff/checklist/maturity with actual results. Native
   implementation alone does not qualify UI, skin/perceptual/color quality,
   Adobe behavior, profiles/camera/corpus, optimization or release readiness.

## Contract-only verification â€” 2026-10-02

The ignored independent checker assesses rational selectors/weights/rotation/
native-Y/chroma/fade equations, neutral continuity, exact bypass and layout. Its
pre-native report binds the original contract archived in before-color-mixer/,
helper and archived DLL/pyd. The archived build rejected the then-planned type.
This remains historical contract evaluation; do not rerun its unknown-type check
unmodified against current native support. Native results follow below.

## Native verification — 2026-10-02

Exported copied-settings API and strict saved type implemented without changing
the frozen selector/domain/control equations. Native controls and seven Python
tests cover independent exact-rational mapping,identity/errors and graph/mip/
geometry/cache/jobs/history/analysis. Full rebuilt Release suites **44/44 default,
25/25 core,26/26 LittleCMS** pass. Installed header/binaries/exports/notices
verified;viewer8/8 and adapter10/10 pass with unchanged UI sources.

Two ISO20000 ROIs,both demosaicers/spaces/native/mip1/mip2,four presets:
96 independent float64 palette-vertex/dot-Y references within float32 rounding,
96 exact,max absolute error 0;all partition/identity checks exact,
twelve historical calibrated input hashes unchanged,eight display boards inspected.
Native warm-input uncached/cached API median case medians **3.494800/0.329100 ms**,
16 cases/seven repeats,256-square/64 MiB cache,one edit miss/upstream hits and
zero following cached misses. PERF-018 records scope and profiling points.
This includes graph/cache/copies;kernel/full-frame/concurrency/allocator and
production photographic/skin/perceptual/HSL/Adobe claims remain unqualified.

Ignored evidence `tests/rawfiles/_librawops_local/high-iso/color-mixer-verified-v1/color-mixer-camera-check-v1.json`,
SHA256 `345f0cc5e66771cf722350f6d31dec76c0161784dc2390e89c64905b808fff7f`,binds current native/core-source/helper/decoded/ROI/runtime/boards.
Pre-native contract and3D LUT DLL/pyd preserved in `build-msvc-release/before-color-mixer/`.
Continue from the [authoritative handoff](plan/LIBRAWOPS_PLAN.md).

The optional [viewer panel](RAW_TEST_VIEWER.md) now exposes per-band controls
with retained values,Before/reset and19 exact actual-window/native comparisons.
Production color and conventional HSL/selective/perceptual/skin qualification
remain open. The viewer adds no native numerical or default-recipe change.

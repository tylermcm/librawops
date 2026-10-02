# Original bounded tonal color balance contract v1

Status: **functional bounded native foundation; production color qualification open**.
This is the first bounded color-balance component of the color-balance/grading
roadmap. The engine executes `rawengine.color_balance` as an opt-in saved operation.
The equations below are original engine policy, defined from existing working-Y
coefficients and a quadratic three-band partition. No external implementation,
dependency, profile or model is selected.

## Domain and settings

Exports: `ColorBalanceSettings`, `validate_color_balance_settings` and
`ColorBalanceNode` in ToneOps.hpp/.cpp. Settings are copied into immutable state.
Accept finite float32 scene-linear ProPhoto/D50 or Rec.2020/D65 RGB with the
matching actual descriptor; output retains that descriptor. Negative samples
and highlight headroom are supported without clipping, transfer conversion,
normalization or working-space conversion.

Three fixed RGB arrays, each exactly three finite binary64 numeric scalars in
[-1,1], default to zero:

| Parameter | Meaning |
| --- | --- |
| `shadows` | RGB offsets at selector luminance <=0 |
| `midtones` | RGB offsets weighted most strongly at selector luminance 0.5 |
| `highlights` | RGB offsets at selector luminance >=1 |
| `preserve_luminance` | Boolean, default true; remove the blended offset's native Y |

Offsets use working-linear channel units, where the existing diffuse-white
reference is 1. They are additive color controls, not exposure stops, channel
gains, white balance, hue wheels, temperature/tint, conventional display-HSL,
skin/perceptual ranges or Adobe-equivalent sliders. Midtone weight peaks at
0.5 rather than 1: overlapping shadow/highlight weights still contribute there.
Range thresholds, weights and widths are fixed; global/three-way hue-wheel
grading, contrast/pivot/gamma controls and selective color remain separate scope.

Unlike the hue-selective mixer, this operation intentionally edits neutral
pixels and can tint black. Luminance preservation can produce signed channels;
there is no gamut mapping or skin-protection promise. Equal RGB offsets become
an exact no-op when luminance preservation is enabled. With preservation off,
equal offsets can change brightness and lift black.

## Input validation and selector

Validate bounds, exact storage length, actual descriptor and all finite RGB
samples before any identity bypass. If all nine offsets are numerically zero,
copy the original float32 bits regardless of `preserve_luminance`. There is no
generic neutral-pixel bypass.

Promote samples to binary64. Reuse the existing pinned saturation/vibrance/
color-mixer native-Y coefficients without altering those operations:

| Working space | wr | wb |
| --- | --- | --- |
| ProPhoto/D50 | 0.28807112822929337 | 0.00008565396060525903 |
| Rec.2020/D65 | 0.26270021201126703 | 0.059301716469861945 |

For any triple v, pin the green-anchored arithmetic without contraction:

```text
Y(v) = (vG + wr * (vR - vG)) + wb * (vB - vG)
Y0 = Y(input)
t = 0 if Y0 <= 0 else 1 if Y0 >= 1 else Y0
```

The selector is evaluated once from the original input, before applying offsets.
The clamp applies only to the selector; it never clips image samples. Negative-Y
colors receive the shadow controls, and all Y>=1 colors receive highlight
controls. Chromaticity, shared RGB scaling and brightness offsets can change the
selected tonal weights; no scale-invariant or perceptual tone classification
is claimed. Pinned binary64 Y determines actual selection near thresholds.

## Overlapping tonal weights and strict blend

For the original selector t, evaluate in this order:

```text
a = 1 - t
ws = a * a
wm = (2 * t) * a
wh = t * t
```

These are quadratic Bernstein weights. In exact arithmetic they are nonnegative
and sum to one. At t=0 they select shadows, at t=1 highlights, and at t=0.5
their values are [0.25,0.5,0.25]. They are continuous across the clamped endpoints;
the endpoint derivative changes, so global derivative continuity is not promised.
Actual binary64 weight sums need not equal 1 exactly. Do not renormalize them.

For each RGB component, blend its shadow/midtone/highlight settings s/m/h:

```text
if t == 0: d = s
else if t == 1: d = h
else if s == m && m == h: d = s
else: d = (ws * s + wm * m) + wh * h
```

Endpoint selection and equal-control retention precede arithmetic. This keeps
inactive bands from affecting endpoint inputs and retains uniform offsets
exactly, including numeric-zero signs. No epsilon, tolerance-based identity,
parameter normalization or reselection after editing is used.

## Offset projection and output

If all three blended components are numerically zero, copy the original bits.
Otherwise calculate:

```text
if preserve_luminance:
    Yd = Y(d)
    offset[c] = d[c] - Yd
else:
    offset[c] = d[c]
```

The green-anchored expression retains equal dR/dG/dB exactly, so preservation
projects a common RGB offset to numeric zero. In exact arithmetic:

```text
preserve_luminance: Y(output) = Y0
otherwise:         Y(output) = Y0 + Y(d)
```

This uses native working Y, not display/perceptual lightness. Projection is not
a grayness or chroma-range constraint. No exact Y guarantee follows the final
float32 rounding; bounded cancellation in binary64 is part of the contract.

For each component independently:

```text
if offset[c] == 0: copy that input component's float32 bits
else: output[c] = float32(double(input[c]) + offset[c])
```

The component bypass covers inactive tonal bands, exact weighted cancellation,
projected common offsets and unaffected channels. Thus a nonzero edit to one
component does not erase another unchanged component's signed zero. Tiny
nonzero offsets execute even if a final cast happens to round back to the input.
Nonzero edits have no additional signed-zero preservation guarantee.

Validate finite intermediates and finite float32 representability of every final
binary64 sum before the single output cast. No intermediate float32 conversion,
silent clipping or overflow normalization. With the bounded settings, ideal
blended offsets are in [-1,1] and projected offsets in [-2,2]. These are
mathematical scope bounds, not a parser/process memory cap or an exact rounded
bound. For finite float32 inputs these small offsets fit binary64; near float32
extremes they can be rounded away before the output cast. A final-overflow
guard remains mandatory, but tests must not invent an unreachable overflow
fixture or claim exact-real sums above float32 max are separately represented.

## Saved graph and rendering

Saved type `rawengine.color_balance`, schema1/processing2, has exactly:

```json
{
  "shadows": [0,0,0],
  "midtones": [0,0,0],
  "highlights": [0,0,0],
  "preserve_luminance": true
}
```

All three arrays contain exactly three finite numeric scalars, excluding
booleans. `preserve_luminance` must be a boolean, not 0/1. Reject missing/extra/
wrong-shaped/nonfinite/out-of-range values and unsupported versions even
disabled. Enabled execution requires one image input, matching
supported working domains, normal blend, opacity1 and no masks/extensions.
Disabled execution follows existing validated exact passthrough/domain rules.
Enabled graphs execute the registered original bounded type.

Native final/preview and mip1/2 preview map the requested upstream float32 RGB.
The quadratic tonal weights generally do not commute with native averaging:
for source neutral pixels 0 and 1, only midtone red=1 and preservation off,
mapping the mip1 input 0.5 yields [1,0.5,0.5]; averaging edited native pixels
would yield [0.5,0.5,0.5]. Requested-level mapping is deliberate.
No halo, geometry, reduction, source identity or descriptor change is added.
Existing graph/cache/jobs/latest/history/analysis/geometry/working-conversion/
multi-source footprint APIs apply. All arrays, the boolean, type/version,
upstream, actual descriptor and requested level separate cache identity.
Default recipes, prior kernels, Menon/source policy, decoder and UI are unchanged.
The UI is a local test harness only and will not ship alongside the library.

## Native implementation and qualification gates

1. Export copied settings/validator/node and register the strict saved type.
   Preserve pinned Y/compiler arithmetic and all existing operations/defaults.
2. Independent exact-rational truth for weights/endpoints/overlap/asymmetric
   RGB controls/projection/both modes/spaces; neutral, signed/headroom,
   near-threshold, zero-Y, subnormal and bounded maximum settings. Test neutral
   tinting/black lift explicitly, common-offset preservation and float32 rounding.
3. Exact full/component identity, extreme/signed-zero bits, inactive bands,
   effective cancellation and tiny nonzero edits; copied input/settings;
   malformed tile/domain/finite/parameter/type/version checks even disabled.
   Keep finite/final-overflow guards; distinguish numerical reachable fixtures.
4. Requested native/mip/ROI/tile parity, the nonlinear order counterexample,
   no halo, geometry, actual working conversion, mixed-source footprints,
   array/boolean-specific cache edits, jobs/latest/history and read-only analysis.
5. Archive current binaries/contract before rebuilding; full default/core/LCMS
   builds/tests, installed public API/binaries/exports/notices. Fixed existing
   ISO20000 background/skin ROIs, both demosaicers/spaces/native/mips, independent
   references, exact partition/identity/twelve historical input hashes and
   inspected boards. Bind evidence to native/sources/helper/decoded/ROI/runtime.
6. After full suites finish, measure warm-input uncached/cached 256-square API
   requests, seven repeats and 64 MiB cache; cache deltas, initialization and
   Windows memory. PERF-019 records bounded API cost with remaining tonal
   weighting/projection/graph/copy/memory gaps, without a regression claim.
7. Update the authoritative handoff/checklist/maturity with actual results.
   Broader color grading, hue wheels, production color/profile/skin/perceptual/
   Adobe/release and full-frame/concurrent/allocator qualification stay open.
   UI additions require a concrete testing need and remain outside packaging.

## Contract verification

The ignored `build-msvc-release/research/color-balance-contract-check.py` uses
independent Fraction weighted sums and dot-product Y for equation/weight/
projection truth, alongside a strict ordered binary64 prototype. It also checks
exact bypasses, malformed prototype settings and the reduction-order example.
Its pre-native report binds the original contract archived in before-color-balance/,
helper and archived DLL/pyd, including the then-unknown enabled type rejection.
This remains historical contract evaluation; do not rerun its unknown-type check
unmodified against current native support. Native results follow below.

## Native verification — 2026-10-02

Copied-settings API and strict saved type implemented without changing the frozen
equations or domain. Native controls and seven Python cases cover independent
exact-rational weights/projection, bits/errors and graph/mip/geometry/cache/jobs/
history/analysis. Full rebuilt Release **46/46 default,26/26 core,27/27 LittleCMS**
passes. Installed header/binaries/exports/notices verified; unchanged viewer9/9
and adapter10/10 compatibility tests pass. UI remains an unpackaged test harness.

Two ISO20000 ROIs,both demosaicers/spaces/native/mip1/mip2,four presets:
96 independent float64 Bernstein/dot-Y references exact; all partition/identity
checks exact,twelve historical calibrated input hashes unchanged,eight boards
inspected. Maximum output-Y target residual after float32 rounding 1.35651046e-08.
Native warm-input uncached/cached API median case medians **2.665700/0.288450 ms**,
16 cases/seven repeats,256-square/64 MiB cache,one edit miss/upstream hits and
zero following cached misses. PERF-019 records scope and remaining profiling gaps.
These measurements include graph/cache/copies; kernel/full-frame/concurrency/
allocator and photographic/skin/perceptual/grading/Adobe qualification stay open.

Ignored evidence `tests/rawfiles/_librawops_local/high-iso/color-balance-verified-v1/color-balance-camera-check-v1.json`,
SHA256 `1715a297f9d0899d887999d31ce9544fc0b6877a010aca87a56e8e0a69ffbb9f`, binds current native/core-source/helper/decoded/ROI/runtime/boards.
Pre-native frozen contract and color-mixer DLL/pyd preserved in
`build-msvc-release/before-color-balance/`.
Continue from the [authoritative handoff](plan/LIBRAWOPS_PLAN.md).

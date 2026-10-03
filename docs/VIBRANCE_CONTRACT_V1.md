# Original bounded scene-linear vibrance contract v1

Status: **functional opt-in foundation, implemented and registered**. This document freezes
the bounded operation added after the saturation/viewer milestone. It defines
an original deterministic adaptive-chroma operation, not a reproduced commercial
slider, perceptual model, skin-protection rule, denoiser or photographic look.
Production/visual qualification remains a separate gate.

## Domain, parameters and exact behavior

Input/output: scene-linear ProPhoto/D50 or Rec.2020/D65 float32 RGB, with the
descriptor retained. Use the exact pinned native-Y red/blue coefficients and
green-anchored float64 evaluation from [saturation v1](SATURATION_V1.md).
No white-point adaptation, normalization, transfer conversion or clipping occurs
inside vibrance. Enabled camera-linear, encoded/display and unsupported working
spaces reject. Every input channel must be finite, even for bypasses.

One immutable float64 parameter, `amount` in `[-1,1]`: zero is exact identity;
positive values increase chroma, negative values decrease it. `-1` is not a
promise of complete grayscale; use saturation amount0 for that. Copy settings
at node construction. Reject malformed/nonfinite/out-of-range settings.

For each non-neutral finite pixel, use these frozen equations/order:

```text
Y = (G + red_coefficient * (R - G)) + blue_coefficient * (B - G)
delta = max(R, G, B) - min(R, G, B)
a = abs(Y)
weight = a / (a + delta)
scale = 1 + amount * weight
out[c] = Y + scale * (in[c] - Y)
```

Evaluate in float64 without multiply-add contraction; cast each output once to
float32. All finite float32 inputs fit the intermediate float64 range. `delta`
is positive on non-neutral input, so the denominator is positive without an
epsilon. There is no hidden threshold or separate luminance normalization.

Before evaluating ratios/maps:

1. Validate all three finite samples and tile storage/bounds/descriptor.
2. If `amount == 0` or numerically `R == G == B`, return the original pixel bits.
3. Otherwise compute Y/delta/weight/scale. If `scale == 1`, return the original
   bits, including the exact zero-Y colored-input case.
4. Check all mapped channels for finite float32 representability before storing
   the pixel. Overflow rejects; it never silently clips or changes exposure.

Weight lies in `[0,1]`; scale lies in `[0,2]`. Working Y is preserved
mathematically, allowing output float32 rounding. At a fixed Y, more chroma
range receives less positive boost. Signed luminance uses its absolute magnitude
only for the adaptive weight; the original signed Y anchors the chroma map.
Negative RGB and highlight headroom remain supported. Neutrals and mixed signed
zeros preserve their original bits. A colored pixel whose Y is exactly zero
is unchanged at every amount; this is deliberate and must be visible in tests.

These equations preserve an RGB chroma direction around neutral mathematically.
They do not guarantee perceptual hue/skin preservation, gamut containment or
noise suppression. Chroma grain can change; evaluate it on the existing
development photos without claiming a calibrated/held-out corpus.

## Graph and preview contract

Saved type: `rawengine.vibrance`, schema1/processing2. Parameters are exactly
`{"amount": <finite number>}`. Use the existing complete operation record,
matching supported domains, one `image` input, normal blend, opacity1 and no
masks/extensions. Known parameter/version validation runs even disabled;
disabled execution follows existing exact passthrough rules. The operation is
available through C++ VibranceNode and saved graph APIs; default recipes retain
their prior behavior and do not introduce it.

It is a point edit: no added halo, unchanged descriptor and output bounds.
Propagate the requested native final/preview or supported mip1/2 preview upstream
and map those float32 samples. Adaptivity is nonlinear, so native vibrance then
averaging and vibrance after upstream reduction can differ. Keep the latter
requested-level order explicit and test an example that distinguishes them.
Place it after camera calibration; the simple viewer places it after
levels/curves/saturation and before the existing working-to-sRGB/tone/output chain.

Use existing operation/version/amount/upstream/level cache identity, source
footprints, jobs/latest/cancellation, history and analysis. Validate from the
actual upstream descriptor after working-space conversions. No decoder/model,
dependency, exposure/WB policy or manifest-format changes are needed. Native
code must reuse the pinned Y policy without changing existing saturation math.

## Required implementation evidence

- Independent exact/decimal primary-scaling and affine truth for primaries,
  mixed signed/headroom colors and amounts -1/-.5/0/.5/1 in both working spaces.
- Bit-exact zero amount and neutral/extreme finite/signed-zero bypass, and exact
  zero-Y colored bypass. Explicitly prove no epsilon-induced bypass error.
- Bounds/finite/overflow/settings ownership, malformed tiles/domains/versions/
  parameter errors; known disabled parameter validation and exact passthrough.
- Working-Y residual within a justified float32 rounding bound, nonnegative
  scale, expected chroma ratios and positive common-scale equivariance within
  rounding. Do not assume cross-working-space perceptual equivalence.
- Native/reduced/ROI/tile equality at each level, an adaptive reduction-order
  counterexample, no halo, geometry/conversion/mixed-source footprints, shared
  cache reuse, jobs/history/copy ownership and read-only analysis integration.
- Existing source/default/policy and saturation identity behavior unchanged.
  Run the appropriate full default/core-only/LittleCMS matrix after native work.
- Fixed-camera development crops with independent numerical references, bypass
  and tile checks; inspect signed/highlight/skin/background/chroma-grain views.
  Archive prior binaries before rebuilding and bind source/runtime/report hashes.
- Measure warm-input uncached edits separately from cached output if useful;
  log observed cost under a new PERF ID, with scope, reproduction, memory limits
  and baseline limitations. Current bounded API measurements are recorded below.

## Historical contract audit — 2026-10-02, before implementation

Read-only `build-msvc-release/research/vibrance-contract-probe.py` confirms the
enabled proposed type rejects as unknown in the current native runtime. A seeded
10,000-color signed/headroom float64 equation audit in both spaces checks weight/
scale bounds, working-Y preservation, zero identity and positive power-of-two
common scaling across amounts -1/-.5/0/.5/1. A two-color example demonstrates
native mapping then averaging differs from requested-level adaptive mapping.
This audits the proposed equations only; native finite/bit/overflow, graph,
camera, performance and production qualification evidence is still required.

## Implementation verification — 2026-10-02

`ToneOps.hpp` exports immutable `VibranceSettings`, its validator and `VibranceNode`.
The strict ToneOps translation unit reuses saturation's pinned coefficients;
saturation equations/defaults remain unchanged. Full rebuilt Release CTest passes
**36/36 default, 21/21 core-only, 22/22 LittleCMS**. Full logs are preserved under
ignored `build-msvc-release/research/vibrance-full-matrix-v1/`. Additional exact
zero-Y fixtures were then added to tests only, rebuilt in all three trees, and
focused vibrance checks passed 2/2 default, 1/1 core, 1/1 LittleCMS; native
engine binaries did not change. The Python vibrance suite now has eight cases.

Independent exact-rational primary scaling and weighted affine references cover
signed/headroom colors and both working spaces. Controls cover copied settings/
input ownership, finite identity/neutral/signed-zero/extreme bits, subnormals
without epsilon, computed-unit-scale bypass, overflow/nonfinite/malformed tiles,
strict parameters/versions/domains even disabled, native/mip/ROI/geometry/
conversion/mixed-source footprints, cache/jobs/latest/history and analysis.
Power-of-two common scaling passes; a two-color counterexample distinguishes
native-then-average from requested-level mapping. Exact zero-Y colored float32
fixtures preserve bytes for all five test amounts:

| Working space | RGB fixture |
|---|---|
| ProPhoto/D50 | (1.578406810760498, 2.1256375312805176, -22974) |
| Rec.2020/D65 | (2.3305158615112305, -0.028333187103271484, -10) |

The ignored offline integer-relation helper used an existing SymPy installation
only to find fixtures; no SymPy code or dependency enters the engine or tests.
Early searches found none and one helper configuration hit a library assertion;
the successful scaled search produced the frozen fixtures above.

`build-msvc-release/research/vibrance-camera-check.py` covers two ISO20000
`_DSC1793` background/skin ROIs, both demosaicers/working spaces, native/mip1/mip2,
and amounts -1/-.5/0/.5/1. **120 float32 outputs match independently primary-solved
NumPy adaptive/affine references exactly; all 120 tile-256/64 comparisons and
identities are exact.** Maximum working-Y residual is `1.4342482312912352e-08`;
all twelve historical calibrated input hashes remain unchanged. Eight five-amount
encoded boards were inspected. Negative amounts attenuate chroma without promising
grayscale; positive amounts increase skin/background color and existing chroma grain.
These are development crops through a diagnostic camera/display path, not
qualified skin protection, profile, perceptual hue, photographic look or NR.

Sixteen native API timing cases, seven repeats each, warm input/uncached edit,
256-square ROI/64 MiB cache: median of case medians **1.571200 ms**, cached output
**0.313500 ms**. Each edit adds one miss/upstream hits; cached requests add zero
misses. PERF-014 records API cost and deferred profiling points, not isolated
kernel or controlled regression timing. Ownership initialization 296.677/286.593
ms remains PERF-003. Workflow after input checks 2.729 s; peak process working
set 334,254,080 bytes includes full Bayer/NumPy/cache/views, not node scratch.

Ignored evidence: `tests/rawfiles/_librawops_local/high-iso/vibrance-verified-v1/`;
`vibrance-camera-check-v1.json` SHA-256
`71c02e01b5f398594188a8829f4acf0c074abe93febe7e578f1ba8dbfb44ae68` binds
helper/native/source/decoded/ROI/runtime/board hashes. Previous saturation DLL/pyd
are archived in `build-msvc-release/before-vibrance/` to preserve earlier reports.
Temporary `vibrance-install-v1` contains updated header/binaries/CMake exports/notices.

The [simple viewer](RAW_TEST_VIEWER.md) exposes vibrance -1..1/default0, exact
Before/reset, eight adapter tests and 14 separately owned native/UI comparisons
with six inspected captures. Larger/concurrent profiling remains unmeasured.
Production vibrance,
skin/hue selectors, perceptual/HSL controls, profiles/quality/Adobe qualification
and custom NR remain separate open work.

## Saved amount precision correction - 2026-10-03

Corrected saved saturation/vibrance amount factories to retain the declared float64 controls. Two independent Fraction-based regression cases fail against archived previous DLL and pass current native,including mip/jobs/history;seven saturation/nine vibrance Python cases pass. Saturation near-one formerly became identity;vibrance non-float32 amount changed ProPhoto channel bits. Rec.2020 can quantize selected nearby amounts to the same float32 output;tests do not require all parameter differences to be visible. Native equations/control versions/defaults unchanged. Current full68/36/37 and fresh installed C++ graph-versus-direct/Python/cubic consumers pass. Camera96 saturation/120 vibrance cases exact;all216 output hashes,16 inspected board hashes and24 historical input records unchanged. Before-point-manifest-precision preserves current cubic/full logs and original point reports. Graph3270/7389/156;142 checked/138 open;no UI/commit/push,camera NR deferred.

Normative float64 control/equation body above unchanged. See the current point-manifest-precision final audit for preserved previous/current evidence bindings.

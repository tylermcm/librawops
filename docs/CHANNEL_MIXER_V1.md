# Bounded scene-linear channel mixer v1

Status: **functional opt-in engine foundation**. This original operation mixes RGB channels with an immutable row-major 3×3
float64 matrix, default identity. Every coefficient must be finite with magnitude
at most 64. Input/output is scene-linear ProPhoto/D50 or Rec.2020/D65 float32 RGB;
the descriptor stays unchanged. It is a creative operation in that declared
space, not camera calibration or working-space conversion.

Each non-unit output row uses strict float64, without multiply-add contraction:

```text
out[row] = (m[row,0] * R + m[row,1] * G) + m[row,2] * B
```

Cast once to float32 after checking finite representability. There are no offsets,
clipping, row-sum normalization, luminance preservation, transfer conversions,
monochrome toggle or photographic/Adobe-equivalence policy. Negative values and
highlight headroom remain supported; output overflow rejects. Every input channel
must be finite even if the matrix is identity, zero or ignores that channel.
Validate upstream tile storage, bounds and descriptor before mapping.

An exact unit row (one coefficient1, the other two numerically zero) copies the
selected input float32 bits. Thus identity, permutations, repeated channels and
unmodified rows preserve signed zeros and extreme finite values. Signed-zero
coefficients count as numeric zero for this rule. Other rows follow the equation;
their zero sign and float32 rounding are not bypass guarantees. A neutral RGB
pixel need not stay neutral. Settings are copied at construction.

## Graph and rendering

`ChannelMixerSettings`, `validate_channel_mixer_settings` and `ChannelMixerNode`
are exported from ToneOps.hpp. Saved type `rawengine.channel_mixer`, schema1 /
processing2, has exactly `{"matrix":[nine row-major finite numbers]}`. Reject
missing/extra/wrong-shaped/boolean/nonfinite/out-of-range values and unsupported
versions even disabled. Enabled execution requires supported matching working
domains, one image input, normal blend, opacity1 and no masks/extensions.
Disabled operations use existing exact passthrough/domain behavior.

Native final/preview and mip1/2 preview propagate the requested level upstream
and map those float32 samples. A linear matrix commutes with averaging
mathematically; rounding/reduction orders need not be bit-identical. The point
node adds no halo or geometry. Use existing source footprints, cache signatures,
jobs/latest/cancellation, history, ownership and read-only analysis APIs. Matrix,
type/version, descriptor, upstream and requested-level identities separate caches.
Default recipes, viewer controls, decoder, dependencies and prior math stay intact.

## Qualification requirements

- Independent exact-rational dot-product references on primaries, asymmetric
  mixes, signed/headroom/subnormal samples, both spaces and coefficient boundaries.
- Exact identity/unit-row/permutation bits; validate every input including ignored
  nonfinite channels. Malformed tiles, settings, domains and output overflow reject.
- Native/mip/ROI/tile equality at each level, no halo, geometry/conversion/mixed
  source footprints, matrix-specific cache reuse, jobs/history/copied ownership
  and histogram/local-statistics integration.
- Full default/core/LittleCMS suites, installed header/binaries/notices, prior
  archived binaries and unchanged historical camera inputs/defaults/policies.
- Real-camera development crops with independent references, partitions and
  identities, plus inspected encoded outputs and bounded API-cost measurements.
  Performance observations do not establish a regression without a controlled
  baseline. Production controls, representative color/skin/quality, profiles,
  Adobe compatibility and full-frame/concurrent profiling remain separate gates.

## Verification — 2026-10-02

Full rebuilt Release CTest passes **38/38 default, 22/22 core-only, 23/23
LittleCMS**. Native controls and seven Python cases verify exact-rational matrix
truth, asymmetric mixes, signed/headroom values, coefficient boundaries, exact
identity/unit-row/permutation bits including extreme/subnormal/signed-zero values,
copied settings/input ownership, malformed/nonfinite/overflow/domain/version/
parameter errors even disabled, requested-level mapping, native/mip/tile/ROI
geometry, conversion/mixed-source footprints, cache/jobs/latest/history and
histogram/local-statistics integration. Previous saturation/vibrance/tone tests
still pass. Eight optional viewer and ten adapter tests pass on the rebuilt engine;
viewer controls/layout are unchanged. The temporary `channel-mixer-install-v1`
contains the public header, binaries, CMake exports and notices.

Ignored `build-msvc-release/research/channel-mixer-camera-check.py` covers two
ISO20000 `_DSC1793` background/skin ROIs, both demosaicers/working spaces,
native/mip1/mip2 and identity/channel-cycle/asymmetric-mix/monochrome matrices.
**All 96 outputs match an independent float64 NumPy matrix dot-product reference
exactly; all 96 tile-256/64 comparisons and identity renders are exact.** Twelve
historical calibrated input hashes remain unchanged. Eight four-preset encoded
boards were inspected: identity retains the original, the channel cycle changes
color, the asymmetric matrix changes contrast/color, and repeated weighted rows
produce grayscale. Luminance can change (maximum working-Y change0.2048084641 in
these cases); it is not a luminance-error metric. These development crops use a
diagnostic camera/display chain, not qualified profiles, looks or Adobe behavior.

Sixteen native API timing cases, seven repeats each, warm calibrated input/
uncached mixer at256-square output and64 MiB cache: median of case medians
**1.435700 ms**, cached output **0.270300 ms**. Each new matrix edit adds one miss
and upstream hits; subsequent cached output adds zero misses. PERF-015 records
measured API cost and deferred profiling hypotheses, not isolated dot-product
kernel time or a controlled regression. Initialization295.030/282.439 ms remains
PERF-003. Workflow after input checks2.223 s; peak process working set330,944,512
bytes includes full Bayer/NumPy/cache/views, not node scratch or a global memory cap.
Larger/concurrent requests and allocation traces remain unmeasured.

Ignored evidence:
`tests/rawfiles/_librawops_local/high-iso/channel-mixer-verified-v1/channel-mixer-camera-check-v1.json`;
SHA-256 `99a342e98989bf9ac62f06e98d8e457a4aacb68a2280df0266e5acb0faa9e4c4` binds
helper/native/source/decoded/ROI/runtime/board hashes. Pre-mixer vibrance DLL/pyd
remain in `build-msvc-release/before-channel-mixer/` for earlier report bindings.
Original defaults, Menon math/frozen policy hashes and viewer sources stay unchanged.

Production control design, offsets/monochrome modes, representative color/skin
quality, profiles/Adobe qualification and 1D/3D LUT operations remain open.

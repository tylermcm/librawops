# Original bounded dehaze contract v1

Status: **functional bounded native foundation; automatic/production qualification open**.

This foundation uses explicit uniform transmission and user-supplied atmospheric
light. It implements an original bounded control policy, rather than estimating
depth, transmission or atmospheric light from a photograph. Automatic spatial
dehaze and production photographic qualification remain separate plan gates.
No third-party implementation or runtime dependency is selected.

## Controls and domain

Exported `DehazeSettings` has finite binary64 `amount` in [-1,1], default0,
and `atmospheric_light`, exactly three finite binary64 RGB values in [0,4],
default [1,1,1]. Atmospheric light is in the actual scene-linear working RGB
units; zero and headroom values are permitted. Validate all controls even at
amount0. `DehazeNode(input, settings)` owns immutable input and copies settings.

Input/output are RGBFloat32 scene-linear ProPhoto/D50 or Rec.2020/D65 only.
Descriptor and extent are preserved. No implicit conversion, normalization,
clipping, transfer, gamut mapping, luminance selector or profile inference.
Signed RGB and headroom are processed without limiting samples to [0,1].

## Formation model and original policy

The atmospheric formation equation `I = t*J + (1-t)*A` relates observed RGB I,
scene radiance J, atmospheric light A and transmission t. See equation1 of
[He, Sun and Tang, CVPR 2009](https://people.csail.mit.edu/kaiming/publications/cvpr09.pdf).
We use only this model as background; no dark-channel prior, spatial estimator,
matting or automatic atmospheric-light method from that work is implemented.

Our original amount mapping sets `h = (7/8)*abs(amount)` and `t = 1-h`.
Thus t is bounded to [1/8,1]. Positive amount removes a declared uniform haze;
negative amount adds it. The 7/8 bound and slider policy are our choices,
not claims about the paper or another editor. Opaque haze cannot be inverted.

The normative arithmetic is ordered binary64, with separate operations and no
contraction, reassociation, fast-math or epsilon thresholds:

```text
validate complete returned input tile: bounds, storage, descriptor, finite RGB
if amount == 0: return original float32 bits
h = 0.875 * abs(amount)
if amount > 0:
    t = 1 - h
    coefficient = h / t
else:
    coefficient = h
for every channel c:
    x = double(original_float32[c])
    difference = x - atmospheric_light[c]       # positive amount
                 atmospheric_light[c] - x       # negative amount
    if difference == 0: preserve original bits
    offset = coefficient * difference
    if offset == 0: preserve original bits
    mapped = x + offset
    check all evaluated values finite and abs(mapped) <= finite float32 max
    cast mapped once to float32
```

Settings-dependent coefficient may be cached at construction with this exact
order. Complete input validation precedes all bypasses/mapping. A failure returns
no partial tile. Genuine float32 underflow is allowed; output overflow rejects
rather than clips. Tiny nonzero amounts may round away naturally but are never
discarded heuristically. Exact zero amount, fixed atmospheric-light components
and computed zero offsets preserve input bits, including signed zero.

In ideal arithmetic, positive amount produces `(I-h*A)/t` and negative amount
produces `t*I+h*A`. Equal-magnitude positive/negative operations are inverses
for the same A; float32 rounding prevents a general bit-exact round trip.
At amount+.5, A1 and input.5 the ideal result is1/9. At amount+1 the slope
is8; at amount-1 the slope is1/8. Perturbations in each channel scale by these
slopes. Positive dehaze can amplify noise up to8: this is not noise reduction.

Neutral numeric RGB stays neutral with neutral A. Tinted A can tint neutrals.
There is no general hue, native-Y, gamut or chroma conservation claim. Constant
fields can change. Uniform haze has no depth discrimination; spatially varying
real haze, incorrect A and non-atmospheric veiling require wider qualification.

## Support, integration and identity

This is a point operation: input support equals output rectangle, no halo or
global analysis, no depth/atmosphere cache or extra image-sized scratch.
Support mip0 Final/Preview and mip1/2 Preview when upstream supports them.
Apply after requested-level upstream reduction. Ideal affine mapping commutes
with exact averaging; floating reduction and single-cast arithmetic need not.
Geometry, analysis halos, multiple sources and RAW support compose through
existing graph infrastructure; dehaze adds no footprint of its own.

Saved type `rawengine.dehaze`, schema1/processing2, has exactly:

```json
{"amount":0.5,"atmospheric_light":[1,1,1]}
```

Reject missing/extra fields, booleans/non-numbers, wrong array length, nonfinite
or out-of-range values and wrong versions even disabled. Enabled execution
requires one image input, normal blend, opacity1, no mask/extensions. Disabled
execution follows existing bypass. Type, versions, exact amount/all A components
(even amount0), upstream identity/domain/extent/rectangle/mip/quality participate
in existing cache identity. Settings and source ownership must survive caller
mutation, history replay and source replacement. Reuse sync/jobs/latest/cancel,
analysis and history APIs. No Python convenience wrapper, default recipe,
GPU support, row-loop preemption or UI panel is required.

## Resources and performance

O(output pixels), complete finite-input validation plus one channel map pass.
Map into the returned upstream tile. A256-square float32 RGB tile is786,432
bytes; no separate RGB output allocation or image-sized scratch is required by
this layout. This excludes upstream/cache/signatures/allocator/Python/history
and is not a process-memory cap. Caller-sized tiles retain checked products.

PERF-026 tracks native validation/map/graph/signature/API costs. Measure identity,
positive/negative and tinted-A fixed camera ROIs at native/mips, both spaces and
demosaicers, seven repeats. Separate owned initialization, upstream-prewarmed
uncached edit and retained-output API/cache counters. Record process memory and
full-frame/concurrency/allocator/stage gaps. SIMD and pass fusion are hypotheses;
strict order/bypass/validation cannot change incidentally. Prototype timings
describe research tooling only.

## Acceptance gates

1. Before native code, ordered binary64 maps agree with independent rational
   formation/inverse formulas for signed/headroom, neutral/tinted/constant,
   step/impulse, tiny/extreme/subnormal/signed-zero samples and endpoints.
   Ordinary finite outputs tolerate `4e-7*max(1,abs(reference))`; identity and
   partition comparisons require exact bits. Validate known uniform-haze truth,
   round-trip tolerance, fixed A and slopes. Preserve a plot and frozen contract,
   current native/source/prior evidence. Planned enabled type must reject now.
2. Native validator/node and strict saved registration follow the frozen order.
   Check null/domain/descriptor/bounds/storage/level errors, all-channel nonfinite
   rejection before bypass, finite overflow, copied settings, concurrent rendering
   and nonzero/uint32-limit origins. Point support must be exact.
3. Independent Python/native tests cover both spaces, tile/ROI/mips, geometry,
   convolution/analysis support, cache controls, disabled validation, jobs/history/
   ownership/RAW/demosaicers/source replacement. Full default/core/LittleCMS tests,
   installed public C++ and Python consumers and unchanged policy/UI gates pass.
4. Freeze diagnostic camera settings before evaluation: amount0 A[1,1,1],
   amounts-.5/+.5 A[1,1,1], and amount+.5 A[.8,1,1.2]; fixed background/skin/
   true-border crops, both spaces/demosaicers/mips. Compare independent arithmetic,
   exact tile/identity and historical input hashes; inspect synthetic/camera boards
   without retuning. Record predicted noise/tint/negative/headroom behavior and
   PERF-026; photographic/profile/corpus/Adobe/release gates remain open.

This closes only a bounded original contract. Follow the
[authoritative build plan](plan/LIBRAWOPS_PLAN.md). The UI remains an unpackaged
local test harness.

## Historical pre-native verification - 2026-10-02

Frozen [original bounded dehaze](DEHAZE_CONTRACT_V1.md): explicit amount[-1,1]/atmospheric RGB[0,4], uniform transmission floor1/8, ordered signed/headroom affine add/remove haze, complete finite validation/exact bypass/no halo/global estimator. Independent prototype29,316 rational maps/4,188 exact identities/63 fixed components/seven slopes/19 invalid controls and overflow checks pass; synthetic recovery max9.536743e-7. Planned native type rejects both spaces; plot inspected; contract/current sources/binaries/prior texture audit archived before code. PERF-026 records unmeasured native costs;checker0.421181s is tool time. Prior full58/31/32 remains prior evidence,not rerun for docs-only work. Checklist133 checked/138 open;HEAD85c989d,local changes,no commit/push.

Report `build-msvc-release/research/dehaze-contract-check-v2.json` SHA256 `549e39c07415c4f79c93cdb4abc72498edc78d15e8b3342acf3222ae6fc541c5`. Plotv2 SHA256 `69e46fb68d99bed4810816639537fb24effcc82a6780f1fd481917011f6b0b64` was inspected; it shows fixed A, negative/headroom outputs and declared gain1/8..8. Maximum rational error1.907349e-6 meets scaled float32 tolerance. Archives `before-dehaze-contract` and `before-dehaze-native` preserve their own sources/helper/normative contract/native hashes; later evidence must retain those versions.

## Native implementation and verification - 2026-10-02

Implemented [bounded original uniform-transmission dehaze](../DEHAZE_CONTRACT_V1.md): exported immutable DehazeSettings/validator/DehazeNode, strict rawengine.dehaze schema1/process2 amount/atmospheric_light, ordered no-halo signed/headroom map and complete finite/exact-bit bypass. Native/seven Python integration cases pass;58,824 exact frozen channel maps/936 exact tile requests,scaled rational error below4e-7. Full Release60/60 default,32/32 core,33/33 LittleCMS;viewer9/9,adapter10/10,installed independent C++/Python consumers pass.144 camera references and tile/identity checks exact,twelve historical inputs unchanged,twelve boards inspected. Fixed A=1 over-corrects dark non-hazy crops;negative RGB display black and tinted A creates predicted color changes. This manual foundation does not qualify automatic/production dehaze. PERF-026 uncached/cached API1.693350/0.273750ms aggregate;stage/full-frame/concurrency/allocator gaps open. Graph2969/6652/153;checklist134 checked/138 open;HEAD85c989d,local changes,no commit/push.

Native report SHA256 `073a948dcf1acf180cfe611c006225de27a03bbd67cf45406ff35a233dbf1f7b`;camera report SHA256 `31ab821a4ebb350ae1eaad9eb3d4b6c1c8342122390a9e0cd169424efc0355e5`. Installed consumer and Python smoke pass;full suites and unchanged UI/policy evidence are recorded in the canonical plan. Source/settings ownership,finite validation,exact support and strict arithmetic follow the frozen normative body. Camera presets were fixed before evaluation. Dark non-hazy crops over-correct visibly at A1;this remains a declared manual uniform control,not photographic/automatic dehaze qualification. PERF-026 carries measured API scopes and gaps.

## Rectangle endpoint verification - 2026-10-02

Corrected DehazeNode rectangle validation to accept the final uint32 pixel/exclusive end2^32 and reject genuine overflow;native edge tests pass. Full Release60/60 default,32/32 core,33/33 LittleCMS pass again;58,824 frozen channel maps/936 tiled requests exact. Fresh144 camera references exact,twelve inputs unchanged,all output and twelve board hashes equal previous dehaze run;no repeat visual inspection needed. Installed C++/Python v2 consumers pass. Current graph2971/6655/152;checklist134/138;HEAD85c989d,local changes,no commit/push. Before-dehaze-rectangle-fix preserves prior sources/native/reports/full test logs. Bounded manual dehaze only;automatic/photo/profile/corpus/Adobe/full-frame/concurrency/allocator gates open. PERF-026 fresh aggregate uncached/cached1.734650/0.292450ms;PERF-028 logs second full-test slowdown without a regression claim.

A nonempty rectangle may have exclusive x/y end2^32,consistent with existing spatial bounds. The final uint32 pixel is valid;end overflow rejects. This corrects constructor-independent direct rendering support without changing controls or map arithmetic. Current native/camera report SHA256 `ebef3d0ae3f6efb9b62ff5f144af32f437e2c7c22c9d623c9c02734ffbe902e2` / `69bee79591d8d21fd65c4edf956967057a4d1931d700e3ce02e8df8b1c802fe3`;v1 boards and outputs exactly retained. Final audit and current full logs are bound separately from archivedv1 evidence.

# Original bounded texture contract v1

Status: **functional bounded native foundation; production qualification open**.

The original pre-native contract body below remains frozen. Its planned API is
now implemented; historical pre-native evidence and current native verification
are recorded separately below.

Texture adjusts one explicitly selected spatial band of scene-linear native Y.
Its detail signal is the difference between two smoothed images, rather than
clarity's original-minus-local-mean signal. This is an original bounded engine
policy, with independent mathematical acceptance criteria. Personal preferred
edits, editor matching, a noise model and production photographic quality are
separate requirements. No external algorithm implementation or runtime is added.

## Controls and domain

Planned exported `TextureSettings` has finite binary64 `amount` in [-1,1],
default0, and integer `scale` in 1..4, default2. Scale is a count of unit blur
passes, not a radius, sigma, wavelength or exposure-normalized physical size.
Scale0 is invalid even for amount0. Planned
`TextureNode(input, native_bounds, settings)` copies settings, owns its immutable
input and validates nonempty, addressable current-stage native bounds.

Input and output are RGBFloat32 scene-linear ProPhoto/D50 or Rec.2020/D65 only.
Extent, descriptor and working space are unchanged. No implicit transfer,
clipping, gamut mapping, profile inference or RGB/chroma smoothing occurs.
The requested upstream float32 RGB samples form binary64 native Y:

| Working space | wr | wb |
| --- | --- | --- |
| ProPhoto/D50 | 0.28807112822929337 | 0.00008565396060525903 |
| Rec.2020/D65 | 0.26270021201126703 | 0.059301716469861945 |

Evaluate `Y = (G + wr*(R-G)) + wb*(B-G)` with separate binary64 operations.
No rounded float32 Y or intermediate RGB blur is allowed.

## The two-scale detail signal

One unit blur B is a horizontal three-tap [1,2,1] pass followed by the same
vertical pass. Each pass uses only existing neighbors at true current-stage
image boundaries. Include the center with weight2, each real adjacent neighbor
with weight1, and normalize by the actual weight sum:4 in the interior,3 at a
one-sided boundary,2 on a singleton axis. No replicated, reflected or wrapped
samples, and no clipping at viewport/internal tile edges.

At every pass, apply this rule again using the same true image bounds. Direct
convolution with a once-renormalized wide binomial kernel differs near borders
and is not equivalent. Horizontal-before-vertical order is fixed even though
exact arithmetic has a separable interpretation.

To preserve constant computed-Y fields exactly, the normative ordered axis pass
uses center-anchored differences:

```text
center = source[p]
sum_difference = +0.0
weight_sum = 2
if the negative-axis neighbor exists in the true image:
    difference = source[p-1] - center
    sum_difference = sum_difference + difference
    weight_sum = weight_sum + 1
if the positive-axis neighbor exists in the true image:
    difference = source[p+1] - center
    sum_difference = sum_difference + difference
    weight_sum = weight_sum + 1
delta = sum_difference / weight_sum
if delta == 0: copy the original binary64 center
else: destination[p] = center + delta
```

Run exactly `2*scale` unit B passes. After pass `scale`, retain fine Y for output
coordinates; after pass `2*scale`, retain coarse Y. Do not round either plane
to float32. Then map each original center RGB in this order:

```text
center_y = Y(original_center_rgb)
if center_y <= 0 or center_y >= 1: copy original RGB bits
detail = fine_y - coarse_y
if detail == 0: copy original RGB bits
weight = (4 * center_y) * (1 - center_y)
strength = amount * weight
offset = strength * detail
if offset == 0: copy original RGB bits
for R, G, B:
    mapped = double(original_channel) + offset
    check finite and within finite float32 range
    cast once to float32
```

Use binary64 without contraction, reassociation, fast-math or epsilon thresholds
for all evaluated Y, subtraction, addition, division and mapping. Reject
nonfinite intermediates and outputs; widen checked coordinate/allocation
arithmetic before additions/products. Genuine float32 underflow on an edited
channel is allowed. Tiny nonzero amounts must not be erased heuristically.
The double-plane delta0 copy preserves constant fields and signed-zero bypasses.

For amount0, validate controls/domain/level/rectangle and the complete returned
output tile (bounds, storage, descriptor, all finite RGB), then copy original
float32 bits; request no halo or blur planes. Otherwise request the full
`2*scale` halo and validate every returned RGB sample before any center bypass.
Endpoint centers do not allow nonfinite halo samples to escape validation.

## Declared response and limitations

In exact arithmetic, `detail = B^scale(Y) - B^(2*scale)(Y)`. For an infinite
interior grid, one B has frequency response
`H(wx,wy) = cos(wx/2)^2 * cos(wy/2)^2`; the detail response is
`H^scale * (1-H^scale)`. This is a nonnegative band-pass response, zero at DC
and at an axis Nyquist frequency, with maximum1/4 where `H^scale=1/2`.
These characterize the linear detail plane, not the adaptive, once-rounded RGB
output. Finite borders, the center-Y selector and float32 rounding alter output
frequency behavior. Scale changes bandwidth through repeated unit passes;
there is no hard frequency cutoff or guarantee that noise falls outside it.

Constant computed-Y fields bypass exactly. Interior affine native-Y ramps and
interior one-pixel alternating stripes/checkers have zero ideal detail;
ordinary floating fixtures use the numeric tolerance below. Borders can adjust
ramps and alternating patterns because missing taps are renormalized.
Positive amount adds this band; negative amount subtracts it. Negative texture
is not a convex interpolation toward a mean, a denoiser, or guaranteed
pointwise monotone smoothing. Step/impulse lobes and overshoot/undershoot are
expected within the declared support and must be recorded rather than hidden.

Equal RGB addition preserves channel differences before float32 rounding and
changes ideal working Y by offset. No perceptual hue/saturation, luminance
conservation, gamut or energy claim follows. Neutral numeric neighborhoods
stay numerically neutral. Signed/headroom RGB remain unbounded by0..1 and
may be edited when center Y is inside0..1. Center Y outside that interval
bypasses; neighboring Y is never clipped. This selector is shared with clarity
as an explicit policy; the two-scale detail signal, scale and borders are distinct.

## Halos, staging and levels

The exact conservative support radius is `2*scale` in both axes (maximum8),
including all successive blur passes. For output rectangle O and true current
stage bounds I, stage0 requires `R0 = expand(O,2*scale) intersect I`.
After unit pass j, the valid plane rectangle is
`Rj = expand(O,2*scale-j) intersect I`. To produce Rj, compute horizontal
temporary samples over Rj expanded by one pixel only vertically and clipped
to I, then vertical samples over Rj. The previous R(j-1) contains every needed
input. Neighbor existence and denominator always use I, never Rj/R(j-1).
An implementation may retain larger correct regions, but must not invent edge
samples or let a temporary buffer boundary become a true boundary.
Save the fine output samples after pass scale before recycling planes.

Native origins may be nonzero. Support mip0 Final/Preview and mip1/2 Preview
only when the upstream node supports them. Reduced stage bounds are zero-based
ceil(native width/2^mip) by ceil(native height/2^mip), following SpatialOps.
Crop/orientation/resize update current-stage bounds before texture.
Texture follows upstream reduction; scale counts passes on requested-level
pixels. Texture-before-reduction is different and needs a counterexample test.
Compose support through geometry, spatial/RAW/multi-source nodes; analysis adds
its own halo before composing texture support. Partition identity assumes
identical upstream samples; reuse compatible extent and validation helpers.
ConvolutionNode's replicated borders and clarity's square mean cannot replace B.

## Saved identity and integration

Planned type `rawengine.texture`, schema1/processing2, has exactly:

```json
{"amount":0.5,"scale":2}
```

Reject missing/extra fields, bool-as-number, fractional or out-of-range scale,
nonfinite/out-of-range amount and wrong versions even disabled. Enabled
execution requires one supported image input, normal blend, opacity1, no masks
or operation extensions. Disabled execution follows existing graph bypass.
No existing operation/default/recipe or processing2 behavior changes.

Cache identity includes exact amount/scale even for amount0, type/schema/process,
extent/domain, upstream identity and rectangle/mip/quality via existing graph
signatures. Planned native/Python saved graph integration reuses sync/jobs/latest,
tile-boundary cancellation, history/save/restore/source pinning and replacement,
analysis and immutable ownership. No row-loop preemption, GPU support, Python
convenience wrapper, default recipe entry or viewer panel is required.

## Resources and performance

The staged reference does `2*scale` two-axis unit passes, O(scale*halo pixels),
not a per-pixel recursive tree. Two reusable double planes plus a saved double
fine-output plane suffice; recompute center Y from original RGB when mapping.
Every reused plane reads only its valid region. For output tile256 and scale4,
the interior halo is272-square:

| Logical storage | Bytes |
| --- | ---: |
| Returned upstream float32 RGB halo | 887,808 |
| Output float32 RGB | 786,432 |
| Two full-halo double scratch planes | 1,183,744 |
| Saved double fine-output plane | 524,288 |
| Total for this layout | 3,382,272 |

This excludes cache/upstream/signatures/allocator/Python/history and is not a
process-memory cap. Caller-sized frame tiles can grow storage; retain checked
allocation products and existing budgets. Sixteen axis passes at scale4 visit
at most1,183,744 halo-plane sites and2,367,488 neighbor differences for this
layout; shrinking valid regions/true borders reduce those conservative bounds.
Amount0 needs no double scratch and only output support.

PERF-025 tracks unmeasured native blur/Y/validation/plane-allocation/copy/API costs.
Measure scale1/2/4, identity and positive/negative amounts on fixed interior and
true-border camera ROIs at native/mip, at least seven repeats, fixed build,
cache/tile/thread/input settings. Separate owned initialization, exact upstream
halo prewarming with uncached edit, and retained-output API time/cache counters.
Report process memory with stage/full-frame/concurrency/allocator gaps explicit.
Buffer reuse, SIMD and larger kernels are improvement hypotheses; changed pass
order/borders/rounding needs a separate version, not an incidental optimization.
Prototype/checker time is tooling cost, never native render performance.

## Acceptance gates

1. Before native implementation, compare ordered binary64 staged math with
   independent exact-rational weighted-sum blur planes (including per-pass true
   borders) and mapping. Cover both spaces, all four scales, signed/headroom,
   neutral/chromatic/flat/affine/impulse/step/alternating/near-flat fixtures,
   singleton/thin/nonzero origins and amount0/±1/tiny amounts. Ordinary finite
   final float32 outputs use absolute tolerance
   `4e-7 * max(1, abs(reference channel))`; bypass and partition identity are
   separate exact-bit gates. Characterize extreme-cancellation selector branches
   against ordered Y rather than demanding rational branch identity.
2. Prove complete-halo staged ROI equality, support, shrink regions, true-boundary
   repeated normalization, response zero/max bounds, and explicit counterexamples
   for tile-edge clipping, once-renormalized wide kernel, clarity substitution
   and texture-before-reduction. Nonfinite halo before bypass, settings and
   finite-cast checks must fail; negative amounts get no denoise qualification.
3. Preserve frozen pre-native contract/source/binaries and prior evidence; confirm
   planned enabled type rejects before implementation. Then export immutable
   settings/validator/node and strict saved type with strict SpatialOps FP policy.
   Native tests cover descriptor/storage/bounds/null/control/domain/level/errors,
   complete halo, exact bypasses, extreme coordinates and concurrent ownership.
4. Independent Python/native integration covers ROI/tile/mip/order, composed
   geometry/spatial/RAW/multi-source/analysis support, identity/invalidation/cache,
   jobs/latest/cancel/history/source replacement and copied input/settings.
   Full default/core/LittleCMS builds/tests, public installed C++/Python smoke,
   license/dependency boundaries and unchanged default/UI/policy gates pass.
5. Before camera evaluation freeze diagnostic inputs/display/settings: amount0,
   amount-.5/scale1 and amount+.5/scales2/4, both spaces/demosaicers/native/mips,
   fixed interior/skin/true-border ROIs. Compare independent complete-halo math,
   tile/identity and historical input hashes; inspect original step/impulse/
   frequency and camera boards. Accept no added partition seams or support/color
   residual beyond declared numeric truth; record expected lobes and noise
   response without retuning the frozen equation. Log PERF-025 and retain broader
   corpus/profile/photographic/release/full-frame qualification as open.

This closes a contract milestone only. TextureNode, dehaze, production texture
quality, denoise and editor compatibility remain open in the
[authoritative build plan](plan/LIBRAWOPS_PLAN.md). The UI remains an unpackaged
local test harness.


## Historical pre-native verification — 2026-10-02

The independent checker passed **29,568 rational mappings**, **11,428 exact
bypasses**, **6,328 exact staged ROI/partition comparisons**, 25,344 neutral
mappings and 1,152 constant bit-preserving mappings. Counts overlap; these are
synthetic prototype checks, not native or production camera cases. Thirty-five
original fields cover flat/near-flat/affine/step/impulse/checker/signed-chromatic
singleton, thin and small images at nonzero origins, both spaces, all four
scales, amounts-1/-.5/0/.5/1/2^-40. Maximum final float32 rational error
5.96028739697e-08 is below the declared tolerance.

Fifteen malformed control cases,27 every-channel nonfinite halo failures before
endpoint bypass, four cast-validator failures,36 translated/uint32-limit halo
and staging proofs,72 independent FIR/analytic frequency checks,564 exact
interior affine/1,128 alternating detail-zero checks and164 impulse-support
checks pass. Extreme constant finite/subnormal/signed-zero RGB retain exact
bits; general extreme cancellation/native branch agreement remains a native
implementation gate. Cast-validator tests do not demonstrate an overflowing
conforming image fixture.

Counterexamples produce max errors0.0292787849903 for internal tile-edge
clipping,0.00883838383838 in detail for once-normalized wide boundary filtering,
and0.0234375 for texture-before/after reduction. Interior alternating texture
detail is exactly0 while radius1 clarity detail is1/3. These distinguish the
chosen operator from substituting clarity or a differently normalized kernel.
The current native engine rejects enabled planned rawengine.texture in both
spaces; native node/registration/port/camera performance remain pending.

Report `build-msvc-release/research/texture-contract-check-v2.json` SHA-256
`4ae612bf4508089cc38ae9cc71d3fa72499fdf09e553e0c0fd04124bd2b92d2e`; checker2.655528s is research-tool time.
The original step/frequency plot `build-msvc-release/research/texture-contract-v2.png`
SHA-256 `a099d8c8eb3d12215f6af7a95cba9e703d07f4517119729a2954f3fb96e01b84` was inspected. It shows radius-limited
step lobes for ±amount and scale-dependent detail passbands, with maximum1/4,
zero DC/Nyquist. Positive amount adds overshoot/undershoot, negative subtracts
the band; this is not a photographic, artifact-free or noise-reduction claim.
Plotv1 has the same hash; report/toolv1 and v2 are preserved separately.

The normative prefix is frozen as
`build-msvc-release/before-texture-contract/TEXTURE_CONTRACT_V1.before-native.md`,
LF SHA-256 `4a0abed4eab1345545bd41324c1086d0025bb0013e93414da05ddfccce5439b8`. The archive also preserves
nine current implementation sources, DLL/cp39 extension, both reports/helpers,
previous clarity verification and a hash manifest. Reruns must use new report
versions and preserve historical helper bindings. The independent helper lives
at `build-msvc-release/research/texture-contract-check.py`; run from the repo
with the restored Windows environment and Anaconda3.9, selecting fresh output
report/plot names before a repeat. No native code/dependency/default/UI changes.

Before this documentation update, the previous clarity final audit was rechecked:
56/56 default,30/30 core,31/31 LittleCMS test-log bindings,144 exact camera
references/twelve boards,input/install/archive/policy/UI and graph2861/6391
remain unchanged. Full suites were not rerun for this contract-only step.
PERF-025 records unmeasured native costs/work-set hypotheses; PERF-020 records
the repeated PowerShell startup/provider delay and working restored-env command
helper. Next implement the frozen settings/node/saved type and acceptance gates
under the authoritative handoff. No commit or push.


## Native implementation and verification — 2026-10-02

Exported TextureSettings/validator/TextureNode with owned immutable input,
copied amount/scale and current-stage bounds. Strict schema1/process2
rawengine.texture validates controls even disabled. The node follows the frozen
native-Y/center-anchored unit pass ordering, actual-weight true borders and
shrinking intermediate regions. Two full-halo double planes are reused; fine
output doubles are saved after scale passes. Center Y is recomputed from original
RGB at mapping, retaining its pinned expression. Complete input bounds/storage/
descriptor/finite validation precedes bypass; amount0 has no halo or planes.
SpatialOps strict FP/no contraction remains enabled. No Python wrapper,
dependency,default recipe,source/demosaic policy or viewer change.

Native tests include independent neutral-step per-pass boundary means, nonzero
origins, positive/negative/tiny edits at scales1/2/4 with exact partitions,
concurrent local scratch, supported/malformed levels and bounds, uint32-limit
staged rendering, copied settings, exact constant/subnormal/signed-zero/extreme
bits and Nyquist differentiation from clarity. Every-channel NaN/±Inf halo,
storage/descriptor/null/camera-domain/control failures reject before endpoint
bypass. Seven Python cases use independent Fraction weighted planes/mapping,
all scales/both spaces/signed/headroom, mip-order counterexample, composed
crop/orientation/resize/convolution/RAW/multi-source/analysis support, exact
tile/ROI/disabled identity, cache amount/scale invalidation including amount0,
sync/jobs/latest/cooperative cancellation/history undo/redo/replay, source
replacement/pinning and copied inputs. An explicit larger active-area footprint
case separates texture radius2 from bilinear/Menon sensor support.

Focused texture CTest2/2 and seven direct Python cases pass. Full rebuilt
Release CTest **58/58 default,31/31 core,32/32 LittleCMS** pass in
166.15/155.33/155.46s concurrently; these are tool times, not render benchmarks.
Viewer9/9(.220s) and adapter10/10(.028s) pass with unchanged UI. A local viewer
test invocation initially omitted its required module path; passing
build-msvc-release corrected the command, with no code change.

Frozen-prototype report `build-msvc-release/research/texture-native-contract-check-v1.json`
SHA-256 `f5345948c0320d5a3023499dd8f83db07b4b5af6a9ee459e16f164f78d143896` binds29,568 exact native mappings,
5,040 exact native tiled requests and1,152 constant bit-preserving mappings.
Six variable extreme/subnormal/near-endpoint fixtures add1,944 exact mappings;
rounded-Y/rational selector branches agree on these tested cases. Historical
independent rational truth retains maximum5.96028739697e-08 error. The original
frozen helper/contract/report is used, not retuned after implementation;
checker0.659220s is tool cost.

Camera report `tests/rawfiles/_librawops_local/high-iso/texture-verified-v1/texture-camera-check-v1.json`
SHA-256 `e974b285c77a0836206133e85c4b34a370b16b16d404a79749ae2d9c7d9ab43e` binds144 exact complete-halo
references over fixed background/skin/true-active-edge ROIs, both demosaicers/
spaces/native/mip1/mip2 and amount0/-.5-scale1/+.5-scale2/+.5-scale4. Independent
direct weighted-sum planes use separately rendered maximum halo inputs, rather
than native anchored differences/shrinking buffers. Max RGB/rounded-RGB-Y target
error0; all tile64 and identity outputs exact; twelve historical calibrated
inputs unchanged. Max intentional Y change0.00899541807364.
All twelve four-column diagnostic boards were inspected: band edits are subtle
on these high-ISO crops, existing chromatic grain persists, and no new partition
seams were observed. This verifies the equation, not denoise, a qualified camera
profile, production texture appearance or an artifact-free claim. The original
step/frequency plot remains unchanged; declared edge lobes/support remain policy.

PERF-025:48 camera timing cases,seven repeats each,256-square native output,
64 MiB cache,exact upstream halo prewarmed as one tile for each nonzero edit.
Median of case medians uncached/cached API5.746400/0.294750ms;
identity0.596800/0.265950,
scale1 subtract4.251500/0.295400,
scale2 boost6.580350/0.303550,
scale4 boost11.520200/0.316100ms.
Each uncached edit adds one miss/upstream hits; cached request adds no misses.
This includes validation/graph/signatures/cache/copies, not isolated kernel or
controlled-regression evidence. Initialization291.820/286.315ms remains PERF-003;
workflow7.131929s includes reference/display/timing work.
Process peak working set357,965,824/peak commit
1,060,225,024 bytes includes owned Bayer/NumPy/cache/boards,
not node scratch. Full-frame/concurrency/allocator/stage measurements remain open.

Installed prefix `build-msvc-release/texture-install-v1` has seven public headers,
current DLL/import library/cp39 extension/CMake exports and two notices, excluding
viewer/Tk/Pillow/decoder/sample LUT assets. Independent installed C++ consumer
compiles/links exported TextureSettings/validator/node and checks the true-border
step; report SHA-256 `4ad598aad78fe60583aad222abb1a87d7c7b0e2146dd19f5a588e16167ad3bf1`. Installed Python module path,
step and exact amount0 smoke passes; report SHA-256 `a4a6fe7818d24f6a332411941b5b15be4489bc9ed7b5b21e361655e683d1a6fc`.
These use the existing Windows/MSVC runtime; clean-machine redistribution remains
a release gate. Current DLL SHA-256 `557988a132440f46efa6346b7279a19b1712f9ace8b0a892efb11b290469e0f1`,
cp39 extension `973e21c867fe6312ff9bce0fa31d446a9fc2be1744c13d979b1672a683b6b0a9`.

Pre-native source/docs/binaries/helpers/report/test/graph bindings are archived
under `build-msvc-release/before-texture-native/` with a26-entry hash manifest;
the earlier frozen normative archive and historical reports remain unchanged.
The previous clarity evidence retains its archived native/source identity,
while rebuilt full suites cover clarity on the current engine. The final audit
is `build-msvc-release/research/texture-native-final-verification-v1.json`.
Reproduce with restored Windows environment, explicit Anaconda3.9,
build-pinned-camera.cmd/build-verify-configs.cmd and full CTest. Local helpers
texture-native-contract-check.py/texture-camera-check.py/texture-install-smoke.py
and build-texture-consumer.cmd bind reports; use fresh output versions for repeats.
Graph2922/6543/147;canonical checklist132/138. No commit or push.

Next: Freeze a bounded original dehaze contract next: choose explicit domain/controls, atmosphere/transmission policy, signed/headroom/identity/color/numerical behavior, any global-analysis/source/cache/halo/resource requirements and objective acceptance gates before native code. Keep wider clarity/texture photographic/profile/corpus/Adobe and full-frame/concurrency/allocator/release qualification open and separate. UI stays an unpackaged local test harness; no panel is needed for this milestone.

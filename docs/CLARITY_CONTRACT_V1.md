# Original bounded clarity contract v1

Status: **functional bounded native foundation; production qualification open**.

The original pre-native contract body below remains frozen. Its planned API is
now implemented; historical pre-native evidence and current native verification
are recorded separately below.

This defines a small scene-linear local midtone-contrast control. Positive amount
increases local luminance contrast; negative amount moves luminance toward its
neighborhood mean. The neighborhood and selector are explicit engine policy.
Photographic quality, multiscale texture/dehaze, denoise and another editor's
clarity semantics are separate gates. Personal preferred edits are not required.
No external implementation, model, algorithm dependency or UI is selected.

## Controls and domain

Planned C++ `ClaritySettings` owns `amount` (finite binary64 in [-1,1], default0)
and `radius` (integer1..8, default3). Planned
`ClarityNode(input, native_bounds, settings)` copies settings, owns its immutable
input and validates a nonempty, addressable native output extent. Only actual
RGBFloat32 scene-linear ProPhoto/D50 or Rec.2020/D65 input is supported. Output
descriptor, working space and extent are unchanged. No implicit transfer,
normalization, clipping, gamut mapping or luminance/chroma conversion is added.

Radius is measured in requested-level pixels. A radius8 square has at most289
real samples, including its center. This narrow limit matches the existing
local-statistics support envelope; wider/multiscale controls need a new contract.
Radius0 is rejected even when amount0, avoiding a redundant identity encoding.

## Neighborhood, boundaries and arithmetic

For output coordinate p, use every existing upstream pixel q within the inclusive
square [p.x-radius,p.x+radius] × [p.y-radius,p.y+radius], clipped to the true
current-stage image bounds. Divide by its actual count N. A viewport or internal
tile edge never clips the window. No replicated, reflected or wrapped samples.
Singleton and thin images use their actual available samples.

Use the same once-rounded binary64 native-Y coefficients as existing color nodes:

| Working space | wr | wb |
| --- | --- | --- |
| ProPhoto/D50 | 0.28807112822929337 | 0.00008565396060525903 |
| Rec.2020/D65 | 0.26270021201126703 | 0.059301716469861945 |

Y(rgb) is evaluated in binary64 as `(G + wr*(R-G)) + wb*(B-G)`. It is a
green-anchored expression with implicit coefficient `1-wr-wb`, using the actual
requested upstream float32 samples. No separate rounded float32 luminance plane
or rounded RGB-blur stage is inserted.

Validate the complete returned upstream tile's exact bounds/storage/descriptor
and every RGB sample before any pixel bypass. Reject nonfinite values rather
than using local statistics' nonfinite exclusion policy. With amount0, request
only the output rectangle, validate it and copy all original float32 bits.
For nonzero amount, request and validate the full radius halo, including samples
outside the viewport, even if some centers later bypass.

For each pixel, evaluate the following in exactly this order, in binary64 with
no fused contraction, reassociation, fast-math or epsilon thresholds:

```text
center_y = Y(center_rgb)
if center_y <= 0 or center_y >= 1: copy original center RGB bits

sum_difference = +0.0
for each real q, ordered by ascending y, then ascending x:
    difference = Y(q) - center_y
    sum_difference = sum_difference + difference
detail = -(sum_difference / N)
if detail == 0: copy original center RGB bits

weight = (4 * center_y) * (1 - center_y)
strength = amount * weight
offset = strength * detail
if offset == 0: copy original center RGB bits

for channel R, G, B:
    mapped = double(center_channel) + offset
    check mapped is finite and within finite float32 range
    cast mapped once to float32
```

The amount0 branch precedes neighborhood math but follows settings/domain/level/
rectangle/tile/finite validation. Check intermediate Y, differences, accumulation,
detail, weight, strength and offset for finiteness on evaluated paths. Reject a
failed output before returning a result tile. Genuine nonidentity underflow on
float32 casting is allowed. Radius and rectangle arithmetic must widen before
addition/subtraction/product and reject unaddressable allocations.

Center-anchored differences define the arithmetic, rather than subtracting a
rounded mean. A constant computed-Y neighborhood has exact zero detail,
preserving original bits even at extreme finite values. Cached double Y values
are permitted if calculated by the same expression; summation order stays fixed.
Existing local RGB mean/variance or ConvolutionNode cannot substitute directly:
their accumulation, nonfinite handling and/or replicated borders differ.
Reuse extent/halo/validation infrastructure where compatible. Implementing in
SpatialOps requires the same strict floating-point compilation policy as ToneOps.

## Declared behavior and limits

In exact arithmetic, detail is `Y(center)-mean(Y(neighborhood))` and weight is
`4Y(1-Y)` for 0<Y<1, otherwise0. Equal addition to RGB changes working Y by
offset and preserves R-G/R-B/G-B differences before float32 rounding. It does
not promise preserved hue angle, saturation, gamut or perceived brightness.
Equal-channel input neighborhoods remain numerically neutral; mixed signed-zero
bits survive a bypass. There is no color-preservation mode or global energy/
luminance conservation claim.

Positive amount produces edge/impulse overshoot and undershoot within the radius;
these follow the equation and must be measured, not described as artifact-free.
Negative amount is a convex movement toward the mean in exact scalar-Y math,
with strength at most1. This is not a noise model or denoise qualification.
Flat computed-Y regions and zero/endpoint weights bypass exactly. Affine interior
ramps have zero ideal detail; general floating ramps use the numerical tolerance.
No monotonicity of the entire adaptive spatial map is promised. The selector is
continuous at Y0/1 in real arithmetic; derivative changes and float32 steps are
allowed. Very small nonzero amounts must not be erased by a heuristic threshold.

Negative/headroom RGB channels are retained and can be edited when their computed
center Y lies inside0..1. Center Y outside that range bypasses, including all
neutral negative/over-white centers; neighborhood Y itself remains unclipped and
can influence an interior center. This is a specified scene-linear selector,
not a photographic exposure-normalized or perceptual midtone claim.

## Spatial levels, replay and ownership

Support native mip0 Final/Preview and mip1/2 Preview only when the upstream node
supports them. Other requests reject. Reduced current-stage bounds use existing
SpatialOps extent rules: zero-based ceil(native width/2^mip) × ceil(height/2^mip).
Native coordinates may have nonzero origins. Crop/orientation/resize change the
current-stage bounds before this node; never assume original sensor dimensions.

Expand the requested output by radius and clip to current-stage bounds; amount0
uses radius0 support. Compose this input extent through upstream geometry,
convolution, RAW calibration/demosaic and multi-source footprints using existing
graph APIs. Clarity is applied after the upstream requested-level reduction.
Radius stays in reduced pixels, so native-scale support grows with mip. This
differs from clarity-before-reduction; test an explicit counterexample. Analysis
adds its own halo around the clarity output before composing clarity's halo.
Partition equivalence requires upstream samples themselves to be consistent.

Planned saved type `rawengine.clarity`, schema1/processing2, has exactly:

```json
{"amount":0.5,"radius":3}
```

Reject missing/extra parameters, bool-as-number, noninteger radius, nonfinite or
out-of-range controls and wrong versions even disabled. Enabled execution uses
one image input, matching supported domains, normal blend, opacity1, no masks or
operation extensions. Disabled execution follows existing graph bypass without
evaluating the clarity neighborhood. No default recipe adds this operation and
existing processing2 types/defaults remain unchanged.

Cache identity includes type/schema/process, exact amount/radius, native extent,
actual domain, upstream identity, rectangle/mip/quality via existing signatures.
Changing radius must affect identity even for amount0. Reuse existing saved
manifest rendering, sync/jobs/latest/cancellation, history undo/redo/save/restore,
source replacement/pinning, RGB histogram/local analysis and Python ownership.
There is no new standalone Python convenience renderer or viewer panel required.
Node cancellation remains at existing renderer tile boundaries; do not claim
row-loop preemption or GPU support.

## Resources and performance evidence

The reference neighborhood traversal is O(output pixels × (2r+1)^2), at most289
sample differences per output pixel. A tile256/radius8 interior halo is272²:
887,808 float32 RGB bytes, output786,432 bytes and, if cached,591,872 bytes for a
double-Y halo, totaling2,266,112 logical bytes. Without the optional Y cache,
the two tile buffers total1,674,240 bytes. These exclude upstream/cache/signature/
allocator/container/Python/history storage and do not cap process memory.
At tile256/radius8 there are18,939,904 reference neighbor visits. Singleton,
border, small tiles and amount0 need fewer. Caller-chosen frame-size tiles may
allocate frame-size intermediates; retain existing resource limits and checked
products, not an invented global memory cap.

PERF-024 in the plan records unmeasured Y formation, neighborhood traversal,
border handling, graph/signatures/cache/copies and allocation/work-set costs.
Measure radius1/3/8, identity and ±amount on native/mip, true-border and interior
camera ROIs with fixed tile/cache/thread/build/source settings. Separate owned
session initialization, warm-input uncached edit and retained-output API costs;
report at least seven repeats and process memory. Keep full-frame/concurrency/
stage/allocator gaps explicit. Separable/prefix-sum/SIMD/fusion/compiled-window
reuse are hypotheses only: changed summation must not silently change this
contract or exact tile/replay/bypass guarantees. Contract-checker time is tool
cost, never a native render benchmark.

## Implementation acceptance gates

1. Before native code, check independent exact-rational neighborhood/selector
   truth against the ordered binary64 prototype on original synthetic samples:
   both spaces, radii1/3/8, bounds/defaults/±amount/tiny amounts, uniform/neutral/
   chromatic/affine/impulse/step, signed/headroom/subnormal/extreme values,
   singleton/thin/nonzero origins, true borders and varied partitions. For ordinary
   finite cases, compare final float32 against independent rational truth with
   absolute tolerance `4e-7 * max(1, abs(reference channel))`; exact bit gates
   separately govern bypass and tile identity. Near extreme cancellation, do not
   mistake the rational selector for rounded binary64 Y: characterize branch
   differences explicitly, then require native/prototype order agreement.
2. Export immutable C++ settings/validator/node and strict saved registration.
   Native tests must cover null/domain/level/rectangle/descriptor/storage failures,
   every-channel nonfinite halo failures before bypass, ownership, finite/overflow
   checks, exact bypasses and concurrency. Preserve previous source/native/frozen
   contract evidence before rebuilding; enabled planned type must reject before
   implementation.
3. Independent Python math and integration tests must cover graph/mip/order,
   tile/ROI/true-border parity, composed geometry/spatial/RAW/multi-source support,
   cache radius/amount invalidation/upstream reuse, sync/jobs/latest/cancel/history/
   analysis/source ownership, disabled schema and malformed controls. Compare
   actual luminance changes to rounded-RGB reference, not assumed conservation.
4. Full default/core/LittleCMS builds/CTest, installed public API/native module
   smoke, notices/dependency boundaries and source/hash/whitespace checks pass.
   Viewer remains unchanged unless testing requires otherwise and stays unpackaged.
5. On fixed admitted development ROIs, freeze original settings before evaluation,
   retain diagnostic source/calibration/display identities, compare independent
   equations/native/mip/tile/identity and unchanged historical input hashes, inspect
   true-border/step/noise/detail crop boards, and log PERF-024 measurements. Step
   polarity and support must match truth; no additional partition seams, color
   residual beyond float32 tolerance or broadened halo is accepted. Ringing/noise
   response is recorded without tuning the frozen contract after seeing it.

These close a bounded implementation milestone. Representative profile/camera/
lighting/skin/highlight quality, wider clarity scales, texture/dehaze, production
performance and Adobe compatibility remain distinct open gates in the
[authoritative plan](plan/LIBRAWOPS_PLAN.md).


## Historical pre-native contract verification — 2026-10-02

The ignored independent checker passed **16,920 rational mappings, 10,090 exact
bypass checks, 24,840 partition regions and 13,536 neutral mappings**. These are
overlapping check counts, not independent production camera cases. Maximum
rational-to-ordered final float32 error5.87661688e-08 is inside the declared
tolerance. Twenty-five original synthetic fields cover singleton/thin/true-edge,
neutral/chromatic ramps, impulse/step and near-flat inputs at radii1/3/8 and
amounts-1/-.5/0/.5/1/2^-40 in both spaces. Constant signed-zero/subnormal/extreme/
chromatic fields preserve bits. Fourteen invalid setting cases,27 every-channel
nonfinite-halo cases before per-pixel bypass, four output-cast bounds checks and
36 translated/nonzero-origin footprint cases pass. Output-cast tests validate
the check itself; they do not establish an overflowing conforming image fixture.

Explicit counterexamples distinguish correct true-image support from internal
tile-edge clipping, and clarity-before-reduction from the specified
clarity-after-reduction. Enabled planned rawengine.clarity rejects in both spaces
on the unchanged current engine. No native clarity node/registration, runtime
feature, dependency or UI was added. Native/package/camera performance and
photographic qualification are still implementation gates.

Checker `build-msvc-release/research/clarity-contract-check-v1.json` SHA-256
`a7c62978a230bfb420eba4d51f3811decab0eb7a10f2adf518e693aa58102019`; elapsed1.702375s is research-tool cost, not native performance.
Frozen pre-native contract `build-msvc-release/before-clarity-contract/CLARITY_CONTRACT_V1.before-native.md` LF SHA-256
`a519c7a9e1af1854902116bf1d085c8c02661126f21c1b9e8d12765fc7bc5009`.
The frozen document and current DLL/cp39 extension/nine implementation sources
are archived together in before-clarity-contract. Current contract body is
unchanged from that frozen prefix; this section adds verification evidence only.
The checker binds helper/source/native hashes. Reruns must choose fresh report
versions and preserve old evidence rather than overwrite it.

The inspected original step plot
`build-msvc-release/research/clarity-step-contract-v2.png` has SHA-256
`488deacb22efe2875e1620576257b8fd22eef60f53af07683fcf5307a7250f9b`.
At radius3, a neutral .25/.75 step with amount+1 reaches about.089286/.910714;
amount-1 approaches the local mean. Both spaces have the same neutral response.
The adjustment spans only the three pixels on each side of the step; flat regions
stay fixed. This is the declared overshoot/undershoot, not a halo-free or
photographic-quality claim. Plotv1 and its helper/metadata are preserved;
inspectedv2 expands the vertical axis to show the complete response.

Latest full54/54 default,29/29 core,30/30 LittleCMS and192 camera comparisons are
the preceding grading/LUT evidence, not rerun or clarity evidence. Their current
native/source/input/board/install/policy/UI bindings remain unchanged. Next
implement the settings/validator/node and saved type, then run this contract's
native/Python/integration/build/install/camera/performance gates. PERF-024 records
the unmeasured neighborhood/Y/graph/copy/resource costs and possible profiling
points. The UI remains an unpackaged local test harness. No commit or push.


## Native implementation and verification — 2026-10-02

Exported ClaritySettings/validator/ClarityNode in SpatialOps, with copied controls,
native extent and shared immutable input. Strict schema1/process2
rawengine.clarity registration accepts only amount/radius, validates even
disabled, and uses existing graph/Python/session integration. Complete halo
validation precedes bypass; amount0 requests only output. An optional double-Y
halo cache reuses the same expression without changing row-major differences.
SpatialOps now has the same strict FP/no-contraction compile policy as ToneOps;
existing spatial regression tests still pass. No production Python wrapper,
dependency, default recipe, source/demosaic policy or viewer change.

Native tests cover exact constant/signed-zero/subnormal/extreme bits, independent
neutral step/true-border math, radius8 partitions, singleton/thin/nonzero origins,
descriptor/storage/complete halo/domain/null/control/level/extent/address-boundary
failures and concurrent requests. Seven independent Python cases cover rational
truth, ±/tiny amounts/radii, signed/headroom/neutral/identity, disabled schema,
tile/ROI/geometry/convolution/analysis halos, requested-level order, RAW both
demosaicers/spaces, working conversion/multi-source support, cache amount/radius
invalidation, sync/jobs/latest/cooperative cancellation/history/replay, source
ownership/replacement and pinned old-source history.

Initial test fixtures incorrectly expected sensor-origin0 support despite the
active-area boundary, omitted the required multi-source export ID and reused
an old source fingerprint after replacement. Corrected fixtures derive pinned
bilinear/Menon active-boundary support and test stale-recipe rejection versus
updated bindings. No production/contract change was needed. Focused clarity/
spatial4/4 pass. Full Release CTest **56/56 default,30/30 core,31/31 LittleCMS**
passed166.35/155.86/155.94s concurrently; these are tool times, not render costs.
Viewer9/9(.223s) and optional adapter10/10(.031s) pass with unchanged UI.

Frozen-prototype native report `build-msvc-release/research/clarity-native-contract-check-v1.json` SHA-256
`62ae49abf15e379a8c0a073a597987520b5112a1c54c5f07ed08f09294e436cc`:16,920 native outputs match the pre-native ordered prototype
exactly and2,700 native tile partitions match;independent rational checks retain
maximum5.87661688e-08 error within the frozen tolerance. Tool elapsed1.834146s
is not a render benchmark. Prototype-only check counts remain labeled as such.

Fixed-camera report `tests/rawfiles/_librawops_local/high-iso/clarity-verified-v1/clarity-camera-check-v1.json` SHA-256
`fce00c8df2a7a7345579c592243a7d5e3c1df0db9ad2aab55232458b263ce2f0`.
144 independent complete-halo mathematical references across background,skin
and true active-edge ROIs,both spaces/demosaicers,mip0/1/2/four frozen presets:
all exact,including tile64 and identity;12 unique historical calibrated inputs
unchanged. Camera reference uses independently rendered radius8 halos and direct
mean-Y math, not the native center-difference loop. Inspected12 crop boards show
soften1 reduces local luminance contrast;boost3/8 emphasize structure and existing
high-ISO grain. Strong chromatic noise remains, as equal RGB offsets do not
denoise chroma. No new partition seams were observed. The diagnostic calibration/
display chain is not production skin/profile/corpus/Adobe qualification.

Forty-eight native timing cases,seven repeats,256-square output/64 MiB cache,
exact required upstream halos prewarmed as single tiles,after all tests:
median of case medians uncached/cached8.110300/.299950 ms. Identity.580400/.256800,
soften radius1 3.810550/.295700,boost radius3 10.940650/.323400,boost radius8
46.563500/.326250 ms. Each edit adds one miss/upstream hits;cached output adds
zero misses. These include graph parsing/signatures/cache/validation/returned
copies;no isolated kernel or controlled regression claim. Different radii/amounts
are declared workloads. PERF-024 records the measured cost and unmeasured stage/
allocator/full-frame/concurrent gaps. Initialization300.161/282.222ms is owned
RAW session cost(PERF-003). Workflow22.926s includes references/displays/timings;
peak working set365,670,400/peak commit1,068,855,296 bytes includes full owned Bayer,
NumPy/cache/boards,not node scratch/global accounting. Profile neighborhood
traversal/Y cache/finite checks versus graph/copies before optimization;fixed sum
order,identity,halos,replay and bounded ownership must remain.

Installed smoke `build-msvc-release/research/clarity-install-smoke-v1.json` SHA-256
`2d9370b3df798e7a1a3fefa06018432fdb7393078534bb8eca8baee205298102` verifies staged module location,clipped true-border step
and exact identity using explicit stage/host runtime DLL loading.
Install prefix clarity-install-v1 contains matching SpatialOps/header set,
DLL/import library/cp39 extension,CMake exports and two existing notices;
no UI/Tk/Pillow/decoder/unused cp312 is installed. Host runtime is used;standalone
loader/runtime redistribution and clean-machine release remain open.

Frozen contract/current-before-implementation DLL/cp39 and nine source files in
before-clarity-contract preserve prior grading/LUT and pre-native evidence.
Reproduce with restored Windows profile/temp/SystemRoot and explicit Anaconda3.9:
build-pinned-camera.cmd,build-verify-configs.cmd,full CTest in three trees;then
clarity-native-contract-check.py,clarity-camera-check.py and clarity-install-smoke.py.
Camera timing must follow tests and use a fresh versioned output directory.
Final ignored verify-clarity-native-final.py binds source/native/frozen contract/
camera/helper/board/package/test/policy/UI/checklist evidence. Graphify refreshed
2861 nodes/6391 links/140 communities,18 expected data-only JSON zero-node notices.
All changes local at HEAD85c989d;no commit or push. Next freeze a bounded texture
contract;dehaze,wider clarity/photographic/profile/corpus/Adobe/release gates remain
separate. UI remains an unpackaged local test harness.

## Equivalent intermediate-finiteness verification - 2026-10-03

The frozen arithmetic and admitted input domain remain unchanged. The requirement
to check neighborhood differences and every ordered partial sum for finiteness
can be established by the following bounds plus a final runtime sum guard on
IEEE binary32/binary64 representations. Other representations retain the
per-neighbor runtime checks. Complete input and per-Y validation, zero-amount
and pixel bypasses, detail/weight/strength/offset checks and final float32
overflow rejection remain in place. No arithmetic operation or iteration order
is removed, reassociated or contracted. This is a clarification of equivalent
verification, not permission to omit input or output validation.

Let M be the largest finite binary32 value, u=2^-52 and eta=2^-1074.
Conservatively bound one binary64 rounded result by |z|(1+u)+eta, allowing
one ulp and a subnormal allowance. Both fixed working-space coefficients are
in [0,.5]. Applying this bound to the exact green-anchored expression gives
|Y|<2^130. An evaluated center has 0<Y<1, so every neighborhood difference
has magnitude <2^131. By induction, every ordered partial sum with k<=289
samples is bounded by k(D+eta)(1+u)^k<2^140. This is far below the finite
binary64 range. Neither a difference nor an intermediate sum can be nonfinite
under these admitted invariants. This proof does not extend to arbitrary
double sources, user-specified coefficients, larger radii or fast-math.

Exact rational bound derivation is preserved in the ignored
clarity-finite-bound-proof.py and clarity-finite-bound-proof-v1.json. A strict-FP
standalone original/hoisted-check prototype agrees exactly with the baseline
DLL on 1,344 timing comparisons and 4,320 selected extreme/nonfinite cases,
including 576 matched rejection cases. Extreme checks cover all radii, signed
zero/subnormal/max-range fields, mixed fields with evaluated neutral centers,
positive/negative/tiny/zero amounts, nonzero origins and true-border/interior
ROIs. They supplement the proof; testing alone cannot prove the bounds.

Native selection remains pending controlled paired timing and independent
math/ROI/mip/RAW/full-build/installed-consumer verification. Prototype timings
are not native performance gains. before-clarity-guards preserves the previous
source/native/contracts/full logs before this candidate change.

## Native finite-check performance decision - 2026-10-03

Accepted the bounded clarity finite-check optimization after proof and native gates. Complete finite binary32 halo/fixed weights/radius<=8 bound all ordered differences and partial sums below2^140;IEEE binary32/binary64 uses a final neighborhood sum guard,other representations retain per-neighbor guards. Arithmetic/order,per-Y/input/output validation,bypasses,API/schema/process/defaults/support/memory are unchanged. Native paired radius1/3/8 medians1.5915x/4.1222x/5.5050x on selected128-square synthetic ROIs;all4032 repeats/144 fixtures exact with unchanged source work. Independent16920 output maps/2700 tiles,rational tolerance,4320 extreme/nonfinite cases(576 matched rejections),1152 rounding-mode cases and144 camera outputs pass. Twelve historical inputs/twelve previously inspected board hashes unchanged. Full69/37/38 pass;fresh installed C++/Python consumers pass. Fixed checklist144 complete/136 open/280 total;HEADd3c4617,no UI/commit/push,camera NR deferred.

Selected controlled ABBA native timing includes consumer clocks/source copying and applies to two spaces/native/mip1/2/interior/edge requests,not universal camera/full-frame performance. Radius1/3/8 minimum paired speedups1.4166x/3.4891x/5.1926x;amount0 path unchanged. Real-camera API aggregate uncached/cached2.714700/0.280150ms after tests;workflow16.927127s,whole-process peaks{"available": true, "peak_commit_bytes": 1069166592, "peak_working_set_bytes": 365957120, "private_bytes": 981594112, "working_set_bytes": 278904832} include owned Bayer/NumPy/cache/displays,not allocator or node scratch attribution. All camera output/board hashes are unchanged,so prior visual inspection remains applicable. Non-IEEE fallback retained but not exercised on this MSVC host. General photographic/profile/corpus/full-frame/allocator/cancellation/cross-runtime/release gates remain open. Prototype-only timing is not used as native acceptance evidence.

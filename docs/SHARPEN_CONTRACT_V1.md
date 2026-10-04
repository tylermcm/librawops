# Original bounded RGB sharpening contract v1

Status: **functional bounded native foundation; capture/creative production qualification open**.

This is a small-radius RGB unsharp-mask foundation for Phase4 sharpening.
It adds a declared high-frequency residual independently to each linear RGB
channel. Capture/profile-specific sharpening, creative photographic qualification,
edge-aware ringing suppression, output-size sharpening and raster denoise remain
separate gates. No external algorithm implementation or dependency is selected.

## Controls and domain

Planned exported `SharpenSettings` has finite binary64 `amount` in [0,2],
default0, and integer `radius` in1..3, default1, in requested-level pixels.
Validate radius even at amount0; no threshold, negative strength, gamma or
adaptive/noise/skin selector. `SharpenNode(input, native_bounds, settings)`
copies controls, owns immutable input and validates nonempty addressable current
stage bounds. Coordinate arithmetic supports exclusive ends up to2^32.

RGBFloat32 scene-linear ProPhoto/D50 or Rec.2020/D65 only; descriptor/extent
unchanged. Signed values and headroom are edited, not bypassed or clipped.
No working conversion, transfer, normalization or gamut mapping.

## Normative local residual

For each output center and RGB channel, W contains the existing pixels within
the radius square at true current-stage image borders, including the center.
Use actual sample count n; no replication/reflection/wrap, no internal tile or
viewport edges. Compute ordered binary64 center-anchored differences:

```text
validate complete returned RGB halo: bounds, storage, descriptor, all finite
if amount == 0: return original output tile bits, no halo/scratch
for output coordinates in row-major order:
    choose W using true current-stage bounds
    for R,G,B independently:
        center = double(original_center_channel)
        sum_difference = +0.0
        for neighbors in increasing global y, then x, including center:
            difference = double(neighbor_channel) - center
            sum_difference = sum_difference + difference
        detail = -(sum_difference / n)
        if detail == 0: copy original channel bits
        offset = amount * detail
        if offset == 0: copy original channel bits
        mapped = center + offset
        check every evaluated value finite and mapped within finite float32 range
        cast once to float32
```

No contraction/reassociation/fast-math/epsilon thresholds. Do not form a rounded
mean then subtract it: direct centered residual preserves small differences.
Complete halo validation precedes flat-channel bypasses. Reject overflow, return
no partial tile, allow genuine final float32 underflow. Zero amount, constant
channel neighborhoods and computed-zero offsets preserve original bits,
including signed zero and extreme finite constants. Other channels can still edit.

Ideal equation is `output = center + amount*(center - mean(W))`. The sum is
centered only to freeze arithmetic and constant identity. This is distinct from
clarity's midtone-selected native-Y/equal RGB offset and texture's two-blur band.
It changes RGB differences and can amplify chromatic noise. Neutral numeric
neighborhoods stay neutral; no general hue/saturation/native-Y conservation.
Constant fields are fixed. Interior affine fields have zero ideal residual;
true-border truncation can edit ramps. Step/impulse overshoot/undershoot are
expected within radius support. This is neither a denoiser nor a deconvolution.

For an infinite interior grid, square-mean response is H=K_r(wx)*K_r(wy),
where `K_r(w)=sum(exp(-i*k*w),k=-r..r)/(2*r+1)` is real. Sharpen response
is `1+amount*(1-H)`. H lies in[-1,1], so gain lies in[1,1+2*amount],
at most5 as a conservative bound; DC gain is1. Finite borders/rounding affect
this ideal response. Independent white-noise variance gain for a full n-pixel
window is `(1+amount*(1-1/n))^2 + (n-1)*(amount/n)^2`; this assumption does
not describe correlated camera/demosaic noise. No noise-suppression claim.

## Halos, levels and saved integration

Amount0 requests exactly O; otherwise `expand(O,radius) intersect true I`.
Support composes through preceding geometry/spatial/RAW/multiple-source nodes
and following analysis halos. Nonzero native origins work; reduced bounds are
zero-based ceil(native dimensions/2^mip), following SpatialOps. Native Final/
Preview and mip1/2 Preview require upstream support. Apply after upstream
reduction: sharpening-before-reduction differs and needs a counterexample.

Planned `rawengine.sharpen`,schema1/processing2, has exactly
`{"amount":0.5,"radius":1}`. Reject missing/extra fields, bool-as-number,
fractional/out-of-range radius, nonfinite/out-of-range amount and wrong versions
even disabled. Enabled requires one image input, normal blend,opacity1,no masks
or extensions; disabled follows existing bypass. Do not change defaults/recipes
or old operations. Include both controls even amount0, type/versions/domain/
extent/upstream/rectangle/mip/quality in existing cache identity. Reuse native/
Python manifests,sync/jobs/latest/tile cancellation/history/analysis/source
pinning/replacement. No UI panel or convenience wrapper is required.

## Resources and performance

Reference visits at most49 samples per channel per pixel (radius3), O(radius^2
*output pixels). Direct returned float32 halo plus separate float32 output;
three scalar binary64 sums, no image-sized double scratch. At tile256/radius3,
halo262-square823,728 bytes plus output786,432 =1,610,160 logical bytes;
9,633,792 channel-neighbor visits bound. Cache/upstream/allocator/Python/history
are excluded; this is not a process cap. Checked allocation products remain.

PERF-027 tracks neighborhood/validation/map/allocation/graph/API work. Measure
identity and radii1/2/3, fixed positive strengths, both spaces/demosaicers/native/
mips/interior/true-border camera crops, seven repeats. Separate owned source
initialization, exact halo-prewarmed uncached edit and cached output/API counters;
record process memory and stage/full-frame/concurrency/allocator gaps. Prefix/
separable sums,SIMD,reuse are hypotheses; changed summation/rounding needs an
explicit version rather than incidental optimization. Prototype time is tooling.

## Acceptance gates

1. Before native code, compare ordered residuals with independent exact-rational
   direct means/maps for all radii,amount0/.5/1/2/tiny, neutral/chromatic/constant/
   affine/step/impulse/checker/signed/headroom/singleton/thin/nonzero-origin fields.
   Ordinary final outputs tolerate `4e-7*max(1,abs(reference))`; identity and
   full-halo ROI/partition equality require exact bits. Check constant components,
   actual borders,support,frequency/noise gain,small-amount continuity and explicit
   tile-edge/reduction-order/clarity-substitution counterexamples. Preserve a
   synthetic plot/frozen contract/current source/native/prior report/full logs;
   planned enabled type rejects before code.
2. Native settings/validator/node and strict graph registration follow the frozen
   arithmetic/support/ownership/domain/level/rectangle/storage/finite/overflow
   rules, including constant extremes,nonfinite halo before bypass and concurrency.
   Independent Python integration covers ROI/tile/mips/geometry/spatial/analysis/
   RAW/both spaces/demosaicers/multisource/cache/jobs/history/replacement.
3. Full default/core/LittleCMS builds/tests,installed public C++/Python consumers,
   unchanged default/UI/dependency/policy and preserved evidence pass. Freeze camera
   presets before evaluation:amount0/radius1,.5/radius1,1/radius2,2/radius3;
   fixed background/skin/active-border crops,both spaces/demosaicers/native/mips.
   Compare independent direct-mean references,tile/identity/historical hashes;
   inspect step/frequency/camera boards,record ringing/chromatic-noise response
   without retuning the equation. Log PERF-027 and keep production capture/creative/
   profile/corpus/Adobe/denoise/release qualification open.

Follow the [authoritative build plan](plan/LIBRAWOPS_PLAN.md). The UI remains
an unpackaged local test harness.

## Historical pre-native verification - 2026-10-02

Frozen [original bounded RGB sharpening](../SHARPEN_CONTRACT_V1.md): amount[0,2]/radius1..3,per-channel center-minus-true-border square mean,ordered centered double residual/exact component bypass,signed/headroom/overflow rules,radius support/requested-level integration.16,605 rational channel maps/3,321 identities/1,320 exact halo partitions/4,725 constant bits/1,161 axis-frequency and noise checks pass,max2.384186e-7;additional1,506 impulse-support/915 affine/867 two-axis-frequency/36 translated uint32 support/nine tiny continuity checks pass. Tile-edge/reduction-order/chroma-substitution counterexamples retained;planned native type rejects both spaces. Step/frequency plot inspected,frozen contract/current sources/native/prior dehaze reports/full logs archived. PERF-027 unmeasured native cost;prototype0.727258s/extra0.039366s are tool times. Prior current60/32/33 suites remain prior evidence,not rerun for this contract step. Checklist135 checked/138 open,graph2971/6655/152,HEAD85c989d,local/no commit/push.

Normative body frozen in `build-msvc-release/before-sharpen-native/docs/SHARPEN_CONTRACT_V1.md`. Math report SHA256 `2255df0583954716dd2014a3510d9e938e84405748ddc416288df0eabb3e7c90`;extra support/frequency/continuity report SHA256 `0515f02d69868f2be8d67ecb845acd6111b22849bdbc92ec7d7816823a752c2e`. Original step/frequency plot SHA256 `20e521f1b4596818ec6839f18997f893e75e2bcc3c161d60642934b7909ab05b` inspected;expected overshoot/undershoot and non-flat high-frequency response retained. Tile clipping error.166666687 and requested-level order error.0833333284 distinguish the required operator. Normative policy is unchanged by those observations;native implementation and photographic qualification remain open.

## Native implementation and verification - 2026-10-02

Implemented [original bounded RGB sharpening](../SHARPEN_CONTRACT_V1.md): exported SharpenSettings/validator/SharpenNode,strict rawengine.sharpen schema1/process2 amount/radius,per-channel centered square-mean residual,true clipped borders/complete halos/exact component bypass/signed-headroom mapping. Native/seven Python integration cases pass;38,880 exact frozen channel maps/1,080 tile requests/9,450 constant channels,max rational error2.384186e-7. Full Release62/62 default,33/33 core,34/34 LittleCMS;viewer9/9,adapter10/10 and installed C++/Python consumers pass.144 independent direct-mean camera references/tile/identity checks pass,twelve historical inputs unchanged,twelve boards inspected. Strong edits amplify existing chromatic grain and introduce declared step lobes;production capture/creative/noise/profile/corpus/Adobe qualification stays open. PERF-027 uncached/cached API11.934350/0.304900ms aggregate;stage/full-frame/concurrency/allocator gaps retained. Graph3030/6802/163;checklist136 checked/138 open;HEAD85c989d,local changes,no commit/push.

Native report SHA256 `0c39bf70c61c81b1dd957b97490b2b4da6bcd4b88ba4696ca232d534607e8bc9`;camera report SHA256 `a6a1a270e13a67ff7ee5f2c149b69782db87bd05b151f4719cda8e55f689047a`. Frozen normative body unchanged;installed C++/Python APIs,strict SpatialOps FP and preserved source/default/UI/dependency/policy boundaries verified separately in the canonical plan/final audit. Camera settings fixed before evaluation;step/frequency and twelve camera boards inspected. Stronger chromatic grain and radius-local ringing are recorded expected responses,not a denoise/artifact-free/capture-quality claim. PERF-027 retains stage/full-frame/concurrency/allocator gaps.

## Traversal verification pending final timing - 2026-10-03

Paused at the user request on2026-10-03. Current native includes validated cubic identity/scalar row reuse and sharpen shared RGB traversal. Latest full Release69/69 default(177.60s),37/37 core(162.39s),38/38 LittleCMS(162.59s) pass;immutable full logs saved in sharpen-traversal-paused-full-logs-v1. Current sharpening38880 frozen channel maps/1080 tiles/9450 constant channels pass exactly;all144 camera outputs,12 historical inputs and12 already inspected board hashes match previous native. Fresh installed sharpening/point/cubic C++ and sharpening Python consumers passed. Sharpen prototype48 fixtures/1344 comparisons showed radius1/2/3 median1.0600x/1.0861x/1.1626x speedups;these are prototype results only. Final native paired timing and final sharpen audit have NOT been run.142 checked/138 open,HEAD85c989d,no UI/commit/push,camera NR deferred.

Resume by completing the pending sharpen native performance checkpoint:run research/run-sharpen-stage-paired.py sequentially after competing tests finish,inspect the144-case/4032-repeat old/new/new/old output/source equality and measured speedups,then run research/record-sharpen-traversal.py for final audit/document bindings. The latest full69/37/38 pass and fresh install are already complete;do not rerun them solely to collect timing or overwrite immutable paused logs. If timing does not justify the optimization,assess/revert only the sharpen traversal against before-sharpen-traversal and reverify;preserve cubic/other prior work. Then continue the documented bounded clarity finite-check proof/prototype under Phase4 performance gates. Broad profile/corpus/allocator/cancellation/release gates open,camera NR deferred,no UI/commit/push.

## Native traversal performance decision - 2026-10-03

Finished the pending sharpen native timing gate and rejected both traversal candidates. Universal fused RGB median paired speedups(radii1/2/3)0.9601x/0.9860x/1.0368x;selective original1/2 plus fused3 gave1.0055x/1.0065x/0.9532x. Native/compiler context did not retain prototype gains without regressions. Restored original SpatialOps.cpp exactly from before-sharpen-traversal,preserving current cubic/other work. Restored paired medians1.0021x/0.9944x/1.0033x are near baseline,not improvement claims. All three144-case paired studies/12096 repeats have exact identical outputs/source work. Current38880 frozen channel maps/1080 tiles/9450 constants and144 camera outputs pass;12 inputs/12 inspected boards unchanged. Fresh installed C++/Python sharpening/point/cubic consumers and full69/37/38 pass. Current fixed checklist144 complete/136 open/280 total;HEADd3c4617,no commit/push,UI unchanged/unpackaged,camera NR deferred.

PERF-027 retains original per-channel traversal. Pure prototype speedups are insufficient to select native optimizations;the observed native radius1 regression was consistent across the selected fixtures,and selective dispatch introduced a radius3 regression. Keep rejected source/native/paired-report artifacts before another traversal/layout/compiler experiment. Restored native camera API13.954350/0.331800ms under overlapping tests,workflow12.0647015s,peak working set360345600/commit1063129088 bytes are diagnostics,not controlled kernel or process-memory regressions. Paired tests used sequential old/new/new/old after competing checks completed,seven repeats,two spaces/interior-edge/native-mips,selected synthetic128-square requests;no confidence interval or universal/camera/full-frame claim. Current full logs preserved immutably before any future focused CTest. All top-level cpp/hpp/CMake now match the verified pre-traversal baseline,including the retained cubic optimization. No quality/API/default/version/resource change is accepted.

## Equivalent neighborhood-intermediate finiteness verification - 2026-10-03

The frozen per-channel arithmetic and admitted domain remain unchanged.
On IEEE binary32/binary64, the conservative bounds below and a final sum
runtime guard establish that every neighbor difference and every ordered
partial sum is finite. Other representations retain per-neighbor checks.
Complete halo validation,detail checks,component/amount0 bypasses,offset/mapped
finiteness and float32 overflow rejection remain in place. The original
channel-major neighborhood traversal is retained. Previously rejected RGB
traversal optimizations are not part of this candidate. No arithmetic,ordering,
clipped sample count,support,versions,defaults or buffer ownership changes.

Let M be the largest finite binary32 value,u=2^-52 and eta=2^-1074.
The conservative one-ulp/subnormal bound round_bound(z)=|z|(1+u)+eta gives
D=round_bound(2M)<2^129 for every per-channel centered difference. Induction
bounds every ordered partial sum after k additions by
k(D+eta)(1+u)^k. For every k<=49(radius<=3),this is <2^135,far below
the finite binary64 range. Thus neither a difference nor an intermediate
sum can be nonfinite for the admitted complete finite float32 halo.
Arbitrary double sources,larger neighborhoods,fast-math,reassociation or
changed representations are outside this proof.

Ignored sharpen-finite-bound-proof.py/v1.json retain exact rational bounds
for all49 partial sums. Strict-FP standalone original/candidate versus
current baseline DLL agrees exactly on1,344 timing comparisons,1,350
extreme/nonfinite cases(444 matched rejections),and360 rounding-mode cases.
Cases cover radius1..3,amount0/.5/1/2/tiny,signed-zero/subnormal/max-range/
mixed fields,nonzero origins,true-edge/interior complete halos and all four
host rounding modes. Matched rejections include nonfinite inputs and finite
input fields that overflow the sharpened float32 output. Testing supplements
the proof;standalone timing is not native performance acceptance.

Before-sharpen-guards preserves previous source/native/contracts/immutable
full logs/proof/prototype. Controlled native timing and independent frozen
math/ROI/mip/RAW/full-test/install/final audit gates remain pending.

## Native finite-check performance decision - 2026-10-03

Accepted sharpening finite-check optimization after proof and native gates,retaining the original channel-major traversal. Complete finite binary32 halos/radius<=3 bound every difference<2^129 and all49 ordered partial sums<2^135;IEEE binary32/binary64 uses one final sum guard per channel,other representations keep per-neighbor checks. Input/detail/component bypass/offset/mapped/output-overflow checks,arithmetic/order,count/support/buffers/API/schema/process/defaults unchanged. Native paired radius1/2/3 medians2.2420x/4.2051x/5.5138x on selected128-square synthetic ROIs;all4032 repeats/144 fixtures exact with unchanged source work. Independent38880 frozen channel maps/1080 tiles/9450 constant channels plus1350 extreme/nonfinite cases(444 matched rejections,including real output overflow)/360 rounding cases pass.144 camera outputs/12 historical inputs/12 inspected boards unchanged. Current full-frame144 fixtures/1008 exact repeats/72 cooperative cancellation checks preserve36 full outputs/support/logical buffers. Full69/37/38 and fresh installed C++/Python consumers pass. Earlier RGB traversal candidates remain rejected. Fixed checklist144 complete/136 open/280 total,HEADd3c4617,no UI/commit/push,camera NR deferred.

Selected controlled ABBA native timing is not universal/camera/full-frame throughput. Radius1/2/3 minimum paired speedups2.0654x/3.8117x/5.0600x;amount0 path unchanged. Camera API aggregate uncached/cached4.068150/0.264700ms after tests,workflow5.921572s,whole-process peaks{"available": true, "peak_commit_bytes": 1064665088, "peak_working_set_bytes": 361914368, "private_bytes": 976822272, "working_set_bytes": 274198528} include Bayer/NumPy/cache/display/runtime and do not isolate allocator/node memory. Current full-frame rerun has exact outputs/request signatures/source bytes/logical buffers/cancellation against preserved baseline;one timing run is not controlled before/after performance. Existing board inspection remains valid because all output/board hashes are identical. Non-IEEE fallback is retained but unexercised on this MSVC host. Representative45MP/concurrency/scheduler/cache/cancellation-latency/allocator/cross-runtime/profile/corpus/photographic/release gates remain open. Strong sharpening still amplifies chromatic grain and produces declared step lobes;this is not NR or a quality/default change.

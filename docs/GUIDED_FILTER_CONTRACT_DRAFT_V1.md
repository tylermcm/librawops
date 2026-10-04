# Working-Y guided filter — contract draft v1

Status: superseded research proposal, retained for history. The
[frozen contract](GUIDED_FILTER_CONTRACT_V1.md) now governs new implementation.
Native implementation and production qualification remain absent. This is a substep of the existing Phase4 local-statistics
and bilateral/guided-filter requirements in [the build plan](plan/LIBRAWOPS_PLAN.md).
It does not change any existing clarity, texture, dehaze, sharpening, noise
reduction or default rendering operation.

## Selected use and provenance

Select a reusable scene-linear RGB base/detail primitive whose scalar guidance is
the input's declared working-space Y. All RGB channels share that guidance, while
each fits its own local regression. This addresses edge-aware base construction
for global tone/detail experiments under Phase4; it does not deliver camera noise
reduction or prescribe a new production control. Same guidance avoids three
unrelated spatial edge decisions. It does not retain all isoluminant chromatic
edges: that behavior must be tested explicitly. Full RGB guidance, external guide
inputs, masks, confidence weights, adaptive radii and joint upsampling require
separate justified work and are not silently provided by this primitive.

The local regression/filter idea is the published guided filter of He, Sun and
Tang, [ECCV 2010](https://people.csail.mit.edu/kaiming/publications/eccv10guidedfilter.pdf),
[author's page](https://people.csail.mit.edu/kaiming/eccv10/index.html).
This draft selects its own bounded domains, true-edge counting, stable arithmetic,
graph behavior and acceptance requirements. It is not a new algorithm claim or
the paper's radius-independent integral-image implementation. No author demo code,
third-party implementation, dependency, trained model or reference image is
incorporated. Independent rational kernel construction is the mathematical oracle.

The existing plan's algorithm/patent release gate remains open. A targeted initial
search found, among other application-specific records, confidence-weighted
[US9286663B2](https://patents.google.com/patent/US9286663B2/en), adaptive-radius
chroma-upsample [US9478017B2](https://patents.google.com/patent/US9478017B2/en), and
video-unit [WO2022218285A1](https://patents.google.com/patent/WO2022218285A1/en).
Their cited claim text is an engineering scope inventory, not a clearance
conclusion; no legal status or freedom-to-operate certification follows from this
search. The selected primitive contains no confidence map, per-pixel radius,
chroma upsampling or video coding path. Preserve this gate for affected release
decisions rather than treating mathematical research as release qualification.

## Proposed domain and parameters

Input/output: finite signed scene-linear interleaved RGBFloat32 in exactly one
declared supported working space: ProPhoto/D50 or Rec.2020/D65. No encoded RGB,
camera-native data, implicit conversion, exposure normalization, range clamp,
black floor, noise/ISO inference or gamut mapping. Output descriptor and extent
match input. Negative values and headroom are ordinary data.

Proposed immutable copied settings:

- `radius`: integer 0..8, default 3, measured in requested-level pixels.
- `epsilon`: finite binary64 in [2^-24, 65536], default 2^-12. Units are squared
  working-Y units. It is regularization, not an estimated sensor variance.

Validate epsilon even at radius zero and when a saved node is disabled. Radius
zero is exact input-bit identity after complete tile/descriptor/storage/finite
validation. There is no amount/strength parameter in this mathematical primitive.

## Mathematical target and true borders

Let `I[j]` be scalar working-Y guidance and `p[j,c]` a channel. At each coefficient
center `k`, let `W_k` contain all image pixels within the square radius `r`, clipped
at the true image boundary. Every included pixel has equal weight; `n_k=|W_k|`.
Define exact real means `mu_k=mean(I)`, `m_k,c=mean(p_c)`, population guide variance
`v_k=mean((I-mu_k)^2)` and covariance `t_k,c=mean((I-mu_k)*(p_c-m_k,c))`.
Set `a_k,c=t_k,c/(v_k+epsilon)`.

For output pixel `i`, `C_i` contains the coefficient centers within radius `r`,
also clipped at the true image boundary, with count `N_i`. Output target:

```
q[i,c] = mean over k in C_i of (m[k,c] + a[k,c]*(I[i]-mu[k]))
```

The exact independent expanded kernel is:

```
W[i,j] = (1/N_i) * sum over k in C_i with j in W_k of
         (1/n_k) * (1 + (I[i]-mu[k])*(I[j]-mu[k])/(v[k]+epsilon))
q[i,c] = sum over j of W[i,j]*p[j,c]
```

The row sums are one in exact arithmetic. Individual weights can be negative;
there is no convex-hull or universal no-overshoot guarantee. With constant
guidance, the target is two actual-count clipped box averages. Clipped boundary
counts differ across windows; the interior `(2r+1)^4` normalizer must not be used
at image edges. Do not replicate, reflect or pad with zeros. A constant RGB field
is preserved; changing regularization does not make the operation an identity on
arbitrary varying input.

Needed coefficient region is `expand(output,r)` clipped to true bounds. Needed
input is `expand(coefficient_region,r)`, equivalently `expand(output,2r)` under
these rectangular bounds. Maximum radius8 therefore needs16 input pixels each
side. Tile boundaries are never image boundaries. Window and coefficient-center
iteration uses absolute global y then x order, so tile/ROI partitioning cannot
change any pixel's arithmetic. Return only requested output. A radius-r halo is
insufficient, including with flat guidance.

## Proposed ordered numeric policy — compiled evidence, not frozen

Promote each input float32 channel to binary64. The mathematical guidance is the
existing working-space Y. Evaluate its differences directly from RGB differences,
with no absolute guide/mean buffer and no FMA/reassociation:

```
dr=double(p[j,R])-double(p[k,R])
dg=double(p[j,G])-double(p[k,G])
db=double(p[j,B])-double(p[k,B])
D(j,k)=(dg + wr*(dr-dg)) + wb*(db-dg)
```

This is `I[j]-I[k]` in exact arithmetic. The ordered finite implementation must
be tested against exact weighted-RGB differences, rather than claiming equality
to subtracting two separately rounded absolute Y values.

| Space | wr | wb |
|---|---:|---:|
| ProPhoto/D50 | 0.28807112822929337 | 0.00008565396060525903 |
| Rec.2020/D65 | 0.26270021201126703 | 0.059301716469861945 |

For each coefficient center k, first sum `D(j,k)` and the three RGB differences
from k in binary64 with positive-zero initial accumulators, global row-major
order. Divide by actual count to obtain mean offsets `mg[k]` and `mp[k,c]`.
Keep these offsets; do not round an absolute mean by adding back a large anchor.
Make a second ordered pass using `dY=D(j,k)-mg[k]` and
`dP=(double(p[j,c])-double(p[k,c]))-mp[k,c]` for squared guide differences and
three cross-products, each with its own ordered positive-zero accumulator. Divide
by actual count to get population variance and covariances; denominator is the
ordered `variance + epsilon`. Slope is covariance divided by denominator. Do not
use `mean(I*I)-mean(I)^2`, subtract huge raw moments, exclude nonfinite samples,
floor variance or apply a hidden epsilon.

For each output/channel, anchor at its input channel value. For each coefficient
center in global y/x order compute the ordered
`((double(p[k,c])-double(p[i,c]))+mp[k,c]) + a[k,c]*(D(i,k)-mg[k])`, skipping the
product when `a==0`; sum these corrections, then divide by actual center count.
Exact zero correction returns the center input bits, including signed
zero. Otherwise add the correction once to the input center in binary64 and cast
once to float32. Reject a nonfinite mapped value or finite value outside finite
float32 range before conversion, and a nonfinite converted result. No intermediate
float32 means, slopes, guide or correction buffers. No rounding-mode/FTZ changes.

Every needed RGB sample is validated before any identity result escapes. Full
finite binary32 extremes,
all four rounding modes, subnormal/signed-zero behavior, near-flat guidance at
large offset, covariance cancellation and output overflow need explicit proof.
Rational kernel truth uses exact binary32 RGB inputs and the declared binary64
weights as exact rational values; characterize ordered difference-expression
rounding separately. A universal ULP or absolute error bound has not
been established. Do not freeze numeric policy based solely on small ramps.

The strict prototype now has selected four-mode exact-kernel comparisons and a
conservative finite-intermediate proof for every admitted finite binary32 input.
[Numeric evidence](GUIDED_FILTER_NUMERIC_EVIDENCE_V1.md) bounds binary64
intermediates by2^467 and documents cancellation/subnormal and actual ordered
float32-range decisions. This is not a universal ULP or relative-output accuracy
guarantee. No near-zero snap is applied. Rounding/FTZ/DAZ controls remain unchanged;
ordinary arithmetic exception-status flags may accrue. The numeric policy remains
a proposal until camera/reduce-first and node identity gates are complete.

## Proposed API, saved graph and ownership

Proposed names: copied `WorkingYGuidedFilterSettings`, explicit validator,
`working_y_guided_filter_region`, reusable finite-tile filtering helper and
immutable unary `WorkingYGuidedFilterNode` with fixed native bounds.

Node quality follows existing spatial contracts: native Final/Preview, mip1/2
Preview only when upstream supports the level. Reduction occurs upstream before
guidance/moments/nonlinear filtering. Radius stays in requested-level pixels.
No default graph insertion or replacement of old arithmetic. Graph footprint
includes the complete composed2r halo and source IDs. Tile caches, jobs and
histories use immutable validated settings and source/operation identity.

Proposed new saved type `rawengine.guided_filter_working_y`, schema1/process2,
exact required parameters `radius`,`epsilon`, unary `image` input, same explicit
supported input/output domains, normal blend, opacity1, no masks. Reject omitted
and extra keys, bool/non-numeric epsilon, noninteger/bool radius, range/finite
errors, wrong versions/domains/input shape and unsupported levels even if saved
node is disabled. Enabled=false may bypass only a fully validated descriptor.
Registration, typed Python integration and revision/digest policy are future
implementation work, not demonstrated by this document.

### Selected identity and descriptor policy before freeze

`WorkingYGuidedFilterNode(input,native_bounds,settings)` will copy its settings
and validated current-stage native extent and retain the immutable input node.
Construction rejects null input,empty/unaddressable bounds,invalid parameters
and any descriptor other than the exact supported scene-linear RGBFloat32
descriptors. Store the declared descriptor/working-Y coefficients at construction;
every returned upstream tile must match that descriptor,exact needed rectangle
and storage before any radius-zero bypass. The finite-tile helper derives its
working-space coefficients from its validated tile descriptor. No silent domain
conversion or profile inference occurs.

The region helper validates radius and nonempty contained rectangles. The node
validates all settings before delegating to it. Native bounds are the current
stage extent,including geometry changes;reduced bounds are zero-based
ceil(native width/2^mip) by ceil(native height/2^mip),as in existing SpatialOps.
`input_region_level` rejects a supplied source extent that differs from that
level extent or an unsupported level/upstream combination. Full2r support is
composed through existing geometry,RAW and multi-source footprint mechanisms;
analysis adds its own halo first. Radius zero requests only the output rectangle.

Saved radius is a numeric integral value0..8;fractional values,booleans,strings
and collections reject. Epsilon is a non-boolean numeric finite binary64 value
within the admitted bounds. Exactly two parameter keys are required;there is no
parameter defaulting in saved records. Type/schema/process,ports,domains,normal
blend,opacity1,mask/extension and parameter validation also apply when disabled.
Disabled execution then follows the existing validated upstream bypass:it does
not evaluate filter neighborhoods or claim an enabled filter's finite-halo check.
Requested render levels still obey the existing renderer/upstream level guards.

Enabled graph cache identity uses the existing canonical complete operation
record and upstream source/operation signature;the tile key adds rectangle,mip
and quality. Both radius and exact serialized epsilon remain in that identity
even at radius zero. Changing either must distinguish enabled saved revisions
despite identical radius-zero pixels. Actual source/domain/current extent are
retained through existing source identity and geometry chains. Disabled nodes
retain upstream executable/cache identity after validating their saved record;
their configuration still survives serialization and history restoration.
No new cache namespace,digest format,global processing version or mutation of old
operation identities is proposed.

Use existing manifest/session/Python APIs,jobs/latest/history,source replacement
and ownership,RGB histogram/local analysis and optional ICC composition. No
standalone Python convenience renderer,viewer panel or automatic graph insertion
is needed. Cancellation remains at existing renderer tile boundaries;this
contract does not promise row-loop preemption or a GPU path. These policies are
selected from current code behavior;new-type execution and integration tests
remain future implementation gates.

## Proposed resource envelope and performance observations

One finite float32 source tile over the needed2r halo; one coefficient table over
the output+r region holding guide mean offset,three RGB mean offsets and three
slopes(seven binary64 values/center); one
RGBFloat32 output tile; fixed-size local accumulators. For unclipped output W×H:

```
N=(W+4r)*(H+4r), A=(W+2r)*(H+2r), O=W*H
logical numeric payload <= 12*N + 56*A + 12*O bytes
```

This includes source and output numeric storage but excludes upstream graphs,
cache, allocator capacity/metadata, retained Python results and concurrent jobs.
At256-square/r8 this bound is5,924,864 bytes. Guard rectangular exclusive endpoints
with uint64 and all products/vector capacities before allocation; reject empty
or unaddressable bounds and malformed/missing/extra source tiles. No global full
image analysis, integral-image storage, histogram quantization, LUT or model.

The proposed ordered two-pass per-center moments and per-output coefficient loop
have O(A*(2r+1)^2 + O*(2r+1)^2) work with bounded r. The reference is not evidence
for the paper's O(N) radius-independent speed. Radius8, source-copy, coefficient
allocation, final correction loops and future stable separable covariance are
profiling points only; optimization must preserve a frozen contract or use a new
processing version. Kernel/API/45MP/allocator/concurrency costs remain unmeasured.

## Gates before freeze, implementation and acceptance

Before freeze: compare centered regression against an independent exact rational
expanded kernel; prove boundary row sums/constant results/support; retain near-flat
raw-moment cancellation, missing2r halo and negative-weight/overshoot examples;
prove finite-intermediate envelope and strict-FP rounding/rejection policy; resolve
actual compiled table layout/revision/identity/resource rules. Inspect synthetic
base/detail/edge/color results with stated limitations. Prototype camera crops
must include both low/high ISO without implying a noise model or production NR.

Before acceptance: native independent casts/rejections, strict saved schema and
all-parameter identities;full-input/geometry/nonfinite/bounds/halo/copy/descriptor
validation;all true borders,offset image bounds,ROI/tile equality,upstream mip order,
thread safety;RAW/raster/multisource/Python/analysis/cache/jobs/history/ownership;
optional ICC composition;measured bounded API/resource evidence;all configured
full suites and fresh installed C++/Python consumers. Broader production/profile,
full RGB/external guidance where justified, portable numeric behavior and the
existing release gates stay separately open. No new headline checkboxes.


## Initial independent scope evidence — 2026-10-03

Scoped an original bounded numeric/graph proposal around the published scalar-guided regression target for a WorkingYGuidedFilterNode under existing Phase4 rows. RGB channels share working-Y guidance,radius0..8,epsilon2^-24..65536;true-edge actual counts,complete2r halo,centered RGB-derived guidance differences and mean offsets,one final float32 cast,explicit identity/finite/overflow rules. Independent exact rational expanded kernels and integer ties-to-even rounding compare256 cases/6720 channels in both working spaces:6672 exact,48 differ by one smallest float32 subnormal(2^-149),all deviations in subnormal-neutral halfway cases.2240 exact row-sum/support checks;fixed halo,raw-moment cancellation and negative-weight/overshoot examples preserved. The first absolute-mean prototype lost small channel detail at large offsets(maxerror0.000115712);centered arithmetic removes that error in selected cases. Synthetic base/residual plot inspected,including near-isoluminant color-edge smoothing and signed/headroom ramps. Research only,not frozen/native/production or camera NR. Current native/source/full74/39/40/75/install/480camera curves acceptance is unchanged. Fixed145 checked/135 open/280 total,HEADd3c4617,no UI/commit/push,camera NR deferred.

Reference `build-msvc-release/research/guided-filter-scope-v3/report.json` SHA256 `cd5c4f5d2ba24abb0f590ea00b680d7b33550893d87385450343cf52ccd50558`;visual report `research/guided-filter-visual-v1/report.json` SHA256 `cd0ff3b319616318d9bd66a7d586030f69e6db714564711f80923d4380346ded`,plot SHA256 `4acf5585239faf2c9160c63c03cd7c0bb994040c3a7e8f96d701d881f12d3f7e`. The reference binds the proposal snapshot `guided-filter-scope-v3/draft-at-reference-v3.md`. Earlier first-scope draft/prototype/report are retained in `guided-filter-scope-v1/`;v2 preserves the centered candidate before exact integer float32 rounding exposed subnormal tie differences. Successful reports are immutable. The selected absolute mean candidate is superseded;neither it nor the centered candidate is frozen/native. No universal rounding/error,artifact,performance or radius-independent complexity claim follows. Next: Complete Working-Y guided-filter pre-freeze gates: compile an isolated strict-FP prototype;verify independent exact-rational casts/conditioning and guards across four rounding modes,full finite binary32 extremes,subnormal/zero and real output overflow. Prove bounds and actual coefficient-storage/resource/rectangle rules;check true-border/ROI/tile/reduction semantics and clean/high-ISO camera prototypes before freezing. Preserve current curves/levels accepted source/native/full74/39/40/75/install evidence. Then implement the frozen primitive/node/saved bindings and full Python/camera/API/installed acceptance. Production/profile/corpus and release gates remain open. No UI/commit/push,camera NR deferred,no new headline rows.


## Strict numeric/resource checkpoint — 2026-10-03

Completed guided-filter strict-MSVC pre-freeze numeric/resource gates.590 selected fixtures/2360 records cover both spaces/four modes,finite float32 extremes/subnormal/zero/offset bounds and guards;independent exact expanded kernels/direct float32 rounding check69,285 channels,68,101 reference-equal.152 invalid rejections,213 output overflow rejections,36,878 exact tile/5,985 ROI/96 thread comparisons. Retained extreme cancellation near-zero residuals and one actual ordered overflow decision with an exact target just inside FLT_MAX;no universal ULP/relative-output guarantee. Conservative ordered-intermediate proof bounds all binary64 values by2^467,with7,803 representable summation endpoint checks. Separate unchanged-prototype harness passes35,709 assertions/566 rejects/8,721 rectangles/48 actual payload checks,four modes,56-byte coefficient layout and preserved FP controls. Camera/reduce-first/node identity gates still open;draft unfrozen,engine unchanged. PERF-036 distinguishes research-tool timings from unmeasured native kernel/API costs. Fixed145 checked/135 open/280 total,HEADd3c4617,no UI/commit/push,camera NR deferred.

[Numeric evidence and finite-intermediate proof](GUIDED_FILTER_NUMERIC_EVIDENCE_V1.md). Immutable strict precision report SHA256 `995d211f64d485bb7ef90e4cef25183ae8484a24586a1fc71f5181ef2c6a4f01`;channel/boundary analysis `391574e1467764633b8924e2c3944f33836362c449d943df32cddc1790710ec8`;bound/compiled guards `ed8f19160c161fc2d149a7e9f7139c1d4f8af8607d5e2a84ba163c8756fbe0b3`. The original draft bound by the precision report is archived at `research/guided-filter-numeric-bound-v1/draft-at-compiled-prototype-v1.md`;successful sources/executables/reports are retained unchanged. Guard tests cover the standalone helper,not engine descriptors or saved graphs.

Next: Complete the remaining Working-Y guided-filter pre-freeze gates:prototype clean/high-ISO camera crops and requested-level reduce-first behavior with true-border/ROI/tile evidence;inspect base/detail/color/overshoot behavior with stated limits. Settle node descriptor,revision/digest,strict saved schema/identity and native level boundaries,then freeze and archive before native implementation. Preserve the completed strict four-mode exact-kernel,finite-intermediate,capacity/rectangle/payload evidence and current curves/levels full74/39/40/75/install acceptance. After freeze implement native/Python/graph/RAW/cache/jobs/history/ICC/camera/API/installed gates. Production/profile/corpus and release remain open,no UI/commit/push,camera NR deferred,no new headline rows.

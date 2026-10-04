# Working-Y guided filter — contract v1

Status: frozen specification, 2026-10-03. Native implementation, registration and
production qualification are absent. Freeze evidence is recorded below. This is a substep of the existing Phase4 local-statistics
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
This contract selects its own bounded domains, true-edge counting, stable arithmetic,
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

## Domain and parameters

Input/output: finite signed scene-linear interleaved RGBFloat32 in exactly one
declared supported working space: ProPhoto/D50 or Rec.2020/D65. No encoded RGB,
camera-native data, implicit conversion, exposure normalization, range clamp,
black floor, noise/ISO inference or gamut mapping. Output descriptor and extent
match input. Negative values and headroom are ordinary data.

Immutable copied settings:

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

## Frozen ordered numeric policy

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
been established. The pre-freeze proof and compiled evidence now establish the
policy below; native acceptance still must replay it.

The strict prototype now has selected four-mode exact-kernel comparisons and a
conservative finite-intermediate proof for every admitted finite binary32 input.
[Numeric evidence](GUIDED_FILTER_NUMERIC_EVIDENCE_V1.md) bounds binary64
intermediates by2^467 and documents cancellation/subnormal and actual ordered
float32-range decisions. This is not a universal ULP or relative-output accuracy
guarantee. No near-zero snap is applied. Rounding/FTZ/DAZ controls remain unchanged;
ordinary arithmetic exception-status flags may accrue. The policy is frozen with the camera/reduce-first and node identity evidence
below. Engine integration remains required.

## API, saved graph and ownership

Exported names: copied `WorkingYGuidedFilterSettings`, explicit validator,
`working_y_guided_filter_region`, `working_y_guided_filter_rgb` finite-tile filtering helper and
immutable unary `WorkingYGuidedFilterNode` with fixed native bounds.

Node quality follows existing spatial contracts: native Final/Preview, mip1/2
Preview only when upstream supports the level. Reduction occurs upstream before
guidance/moments/nonlinear filtering. Radius stays in requested-level pixels.
No default graph insertion or replacement of old arithmetic. Graph footprint
includes the complete composed2r halo and source IDs. Tile caches, jobs and
histories use immutable validated settings and source/operation identity.

New saved type `rawengine.guided_filter_working_y`, schema1/process2,
exact required parameters `radius`,`epsilon`, unary `image` input, same explicit
supported input/output domains, normal blend, opacity1, no masks. Reject omitted
and extra keys, bool/non-numeric epsilon, noninteger/bool radius, range/finite
errors, wrong versions/domains/input shape and unsupported levels even if saved
node is disabled. Enabled=false may bypass only a fully validated descriptor.
Registration and typed Python integration remain native acceptance work. The
identity policy below is frozen; this document does not demonstrate execution.

### Identity and descriptor policy

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

## Resource envelope and performance observations

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

The ordered two-pass per-center moments and per-output coefficient loop
have O(A*(2r+1)^2 + O*(2r+1)^2) work with bounded r. The reference is not evidence
for the paper's O(N) radius-independent speed. Radius8, source-copy, coefficient
allocation, final correction loops and future stable separable covariance are
profiling points only; optimization must preserve a frozen contract or use a new
processing version. Kernel/API/45MP/allocator/concurrency costs remain unmeasured.

## Implementation and acceptance gates

The pre-freeze mathematical,numeric,resource,camera and requested-level gates
are complete for this bounded contract. Broader qualification remains separate.

Before acceptance: native independent casts/rejections, strict saved schema and
all-parameter identities;full-input/geometry/nonfinite/bounds/halo/copy/descriptor
validation;all true borders,offset image bounds,ROI/tile equality,upstream mip order,
thread safety;RAW/raster/multisource/Python/analysis/cache/jobs/history/ownership;
optional ICC composition;measured bounded API/resource evidence;all configured
full suites and fresh installed C++/Python consumers. Broader production/profile,
full RGB/external guidance where justified, portable numeric behavior and the
existing release gates stay separately open. No new headline checkboxes.

## Pre-freeze evidence and limitations — 2026-10-03

The independent exact rational expanded-kernel scope verifies256 cases/6720
channels and2240 exact row-sum/support checks,with48 one-minimum-subnormal
differences retained. Strict-MSVC four-mode precision covers590 selected
fixtures/2360 records,69,285 numeric channels,152 input rejections,213 actual
output-overflow rejections and exact tile/ROI/thread arithmetic comparisons.
[Finite-intermediate proof and numeric evidence](GUIDED_FILTER_NUMERIC_EVIDENCE_V1.md)
provide the conservative2^467 binary64 envelope,56-byte coefficient layout,
35,709 helper guard assertions and explicit extreme-cancellation limits.
No universal ULP,relative-output,convex-hull,no-halo or camera-NR guarantee follows.

Camera prototype:900 cases across all six supplied ISO100/250 captures and one
ISO20000 capture,content and actual top-left borders,two working spaces/two
demosaicers/native/mip1/2/five settings. Thirty odd-extent synthetic cases add
actual RasterSession upstream-reduction evidence. All930 outputs equal the
independent absolute-guide regression float32 reference;180 exact rational
radius1 camera pixels/540 channels also equal. All180 upstream source tile32
comparisons and1830 prototype tile/2790 ROI comparisons are exact. Thirty-two
historical calibrated input hashes are unchanged. Twenty reduction-order cases
include four exact identity pairs and16 noncommuting active pairs,max channel
difference0.0328333378..0.246451393. Filtering is applied after upstream reduction.
This is prototype evidence,not guided-filter engine/graph/Python acceptance.

Seven camera contacts and four selected native96-square boards were inspected.
The boards magnify96 pixels2x nearest and use explicit srgb-preview,
shoulder0.25/gamma1. Residual display is clipped .5+20*working-Y(input-default base)
and never replaces float32 truth. Strong regularization smooths hair,facade,
color boundaries and grain;low regularization retains substantial high-ISO
grain/chromatic structure. Shared Y is not full RGB guidance. Negative signed
RGB is retained(56,100 channels across camera output records);226 channels lie
outside their supplied source-channel ranges. These observations agree with
the admitted negative-weight/signed-output behavior;no preferred-edit,profile,
noise-free,Adobe or production photographic qualification is asserted.

PERF-036:camera prototype replay146.9893324s and independent reference51.6568835s
are workflow costs. Instrumented full-filter medians for native96-square
requests are0.05395ms identity,11.0428ms r1,55.98575ms r3,364.495/365.183ms r8
tight/flat. These include intermediate guards,Peak instrumentation and table/
output allocation;they exclude input cropping,tile/ROI repeats and graph/API.
Radius/window traversal,duplicate guidance evaluation,finite/Peak checks,
coefficient allocation and overlapping tile halos are profiling targets.
Native kernel/API/45MP/allocator/concurrency qualification remains open.
Any optimization must preserve this ordered arithmetic/guards' observable
behavior and frozen replay evidence;proof-backed guard placement is separately
verified rather than assumed. No radius-independent speed claim is made.

Successful immutable reports:

- exact scope: `build-msvc-release/research/guided-filter-scope-v3/report.json`,SHA256 `cd5c4f5d2ba24abb0f590ea00b680d7b33550893d87385450343cf52ccd50558`.

- strict precision: `build-msvc-release/research/guided-filter-precision-v1/pass-v1/report.json`,SHA256 `995d211f64d485bb7ef90e4cef25183ae8484a24586a1fc71f5181ef2c6a4f01`.

- finite bound/guards: `build-msvc-release/research/guided-filter-numeric-bound-v1/report.json`,SHA256 `ed8f19160c161fc2d149a7e9f7139c1d4f8af8607d5e2a84ba163c8756fbe0b3`.

- camera/reduction: `tests/rawfiles/_librawops_local/guided-filter-camera-prototype-v1/report.json`,SHA256 `ba3566b467f845d150d1e89d5b7cf23dd32c2dc84b13b9cf7237a65c12e1f4de`.

- pre-native archive: `build-msvc-release/research/guided-filter-pre-native-archive-v3/report.json`,SHA256 `fbed8b0123707f32ea6bdb3fa5e4e021a881d3ef26ea27cd9c9267fac55bf9ae`.

The archive preserves36 current source/native/prototype/evidence artifacts and
exact draft versions. Actual current engine rejects the new type enabled and
disabled in both spaces. Existing curves/levels source/native/full74/39/40/75 and
installed acceptance is unchanged and rechecked. Native implementation must
replay frozen prototype/camera references and then satisfy all acceptance gates.
Camera NR remains deferred;no UI/commit/push or new headline checklist rows.

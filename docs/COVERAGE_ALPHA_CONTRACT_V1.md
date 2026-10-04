# Coverage and premultiplied alpha foundation v1

Frozen numeric scope, 2026-10-03. This is the first bounded implementation under
the original Phase 5 coverage/alpha row. Graph ports, owned mask sources, painting,
layers, Python, history and document assets remain subsequent integration work;
the original checkbox stays open until those required foundations are accepted.

## Representation and admission

Coverage is one finite binary32 scalar per pixel in [0,1], without primaries,
transfer function or RGB encoding. Premultiplied composite pixels are four finite
binary32 scalars in R,G,B,A order. A is coverage; RGB is scene-linear ProPhoto/D50
or explicit Rec.2020/D65, signed and with headroom. No condition RGB <= A or
RGB >= 0 is imposed. At A == 0, all three stored color components must equal
zero. A nonzero hidden color in a premultiplied transparent input is rejected.
Negative zero is accepted as numerical zero; produced transparent pixels use
four positive zeros. Straight-color inputs must be finite even when alpha is
zero; their hidden finite color is discarded by premultiplication.

Dedicated `CoverageTile` and `PremultipliedRgbaTile` own row-major scalar and
four-channel vectors. They do not change the RGB-only `Tile`, `Node`, renderer,
descriptor enums, manifest formats or current recipes. Bounds must be nonempty,
end coordinates must fit uint32, and storage must match the exact visible pixel
count. The RGBA working-space tag must be one of the two admitted working spaces.
No padding, borrowed data or implicit conversion is admitted by these helpers.
Inputs are const, outputs own fresh storage, and a failure returns no partial
output. Validations inspect the complete input, including endpoint bypasses.

## Numeric operations

Binary32 storage and binary64 intermediates are required. Ordered expressions
use no FMA or reassociation. The implementation does not change the caller's
rounding mode or subnormal policy. Exact fixture acceptance is for round-to-nearest
ties-to-even with gradual underflow; other environments are not an exact-replay
claim. A future portable admission policy must be explicit before wider release.

* Premultiply: validate straight RGB and alpha; zero alpha gives transparent
  black, one alpha copies RGB, otherwise each channel is `float(double(C)*A)`.
* Safe straight access: validate a premultiplied pixel; zero alpha gives three
  positive binary64 zeros, otherwise return each `double(P)/double(A)`.
  This accessor deliberately returns binary64, avoiding a float32 overflow for
  tiny positive alpha. An explicit float32 accessor checks each result against
  +/-FLT_MAX before casting and throws `overflow_error` rather than clipping.
* Apply coverage: validate the pixel and finite [0,1] coverage k; zero k gives
  transparent black, one k copies the valid nontransparent pixel. Otherwise
  compute `float(double(A)*k)`. If that stored alpha rounds to zero, emit
  transparent black. Otherwise scale each stored color by the same k in double,
  then cast. A tiny coverage can lose stored color precision or erase a pixel;
  no epsilon floor, artificial alpha or hidden-color recovery is applied.
* Normal source-over, with source S over backdrop B: zero source alpha copies B,
  opaque S or transparent B copies S, canonicalizing transparent results. For
  the general case compute `t = 1.0-double(S.a)`, then
  `A = float(double(S.a)+double(B.a)*t)` and
  `P[c] = float(double(S[c])+double(B[c])*t)` in that order. Check each double
  color before its cast. Out-of-range color throws `overflow_error`; never clamp
  signed/headroom RGB. Both inputs are validated before an endpoint return.
  Validate stored alpha before returning; if it rounds to zero, emit transparent
  black. Unsupported floating environments may reject an out-of-range computed
  alpha instead of returning malformed coverage.

Bulk premultiplication accepts an RGB Tile and matching coverage bounds, and
requires its exact scene-linear descriptor to match the chosen RGBA working
space. Bulk source-over requires equal bounds and working spaces. Bulk coverage
application requires equal bounds. All are point operations: no halo, spatial
border, mip generation or resampling is performed. Callers must supply already
aligned pixels in one level; no operation assumes that compositing before and
after source reduction is equivalent. Later mask-source reduction will need its
own frozen native-cell footprint and actual-count rule.

For any admitted scalar inputs, premultiplication and coverage scaling cannot
overflow binary32: |factor| <= 1. For division, |P/A| < 2^278 since
|P| < 2^128 and the smallest positive binary32 alpha is 2^-149; binary64 can
represent this range. Source-over can exceed binary32 despite finite inputs,
because stored signed/headroom premultiplied color is not bounded by alpha;
the explicit result guard is necessary. Validation uses `invalid_argument`
for malformed storage, bounds, working-space tags or values. Size arithmetic is
checked before allocation. Allocation failures propagate normally.

## Source basis and deliberate domain decisions

The [W3C Compositing and Blending Level 1 draft, 21 March 2024,
sections 5.1 and 10](https://www.w3.org/TR/2024/CRD-compositing-1-20240321/)
describes source-over, premultiplied storage, division by alpha, and straight
inputs to blend functions. We independently implement the equations. Its bounded
color examples and blend-result clamping are not the signed scene-linear domain
selected by this engine. Only normal source-over is admitted here; encoded-domain
Photoshop blend behavior and other blend modes require separate versions and
evidence. No third-party implementation or new runtime dependency is used.

## Evidence and integration gate

An independent Python Fraction oracle produces binary32 input/output fixtures
using explicit nearest-even binary64 stage rounding and binary32 storage rounding.
Native tests must compare exact bits, including subnormal/tie/overflow inputs;
also test complete validation, transparent hidden colors, endpoint identities,
signed/headroom values, descriptor/bounds/storage mismatches and bulk partition
parity. Generated fixtures are checked against their oracle in CTest.

Subsequent graph integration must use typed scalar mask ports and RGBA image
ports, immutable owned source snapshots with canonical fingerprints, explicit
RGB/RGBA adapters, complete named-edge dependency/signature/ROI traversal and
strict saved schema/domain/extent validation. RGB operations must continue to
reject masks or RGBA unless explicitly extended. Cache accounting must charge
actual scalar/four-channel payloads. Shared jobs/history/Python must retain source
ownership and replay frozen arithmetic. This document does not freeze a graph
schema or claim those paths already execute. Observe routine development costs
in [the performance log](PERFORMANCE_ISSUES.md); targeted performance work remains
deferred until the functional build is complete.

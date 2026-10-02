# Scene-linear saturation foundation

`ToneOps.hpp` exports `SaturationSettings`, `validate_saturation_settings` and
`SaturationNode`. This original point edit supports scene-linear ProPhoto/D50
and Rec.2020/D65 RGB. It has no clipping, transfer conversion, normalization,
adaptive chroma or skin-selective vibrance. It does not change existing recipes,
defaults or older operations.

## Numerical contract

`amount` is a finite float64 number in `[0,4]`: 0 maps to working-space grayscale,
1 is exact identity, values above 1 scale chroma outward. Caller settings copy
into the immutable node. All input RGB samples must be finite, including at
identity and on neutral pixels; nonfinite inputs reject. Finite float32 output
overflow rejects rather than clamping.

Native working-space Y weights derive from the same primary/white chromaticities
as `RawEngine.cpp`; saturation pins the following once-rounded binary64 red/blue
coefficients to its processing identity:

| Working space | Red coefficient | Blue coefficient |
|---|---:|---:|
| ProPhoto/D50 | 0.28807112822929337 | 0.00008565396060525903 |
| Rec.2020/D65 | 0.26270021201126703 | 0.059301716469861945 |

Green has the implicit mathematical coefficient `1 - red - blue`. Evaluate
float64 operations in this order, without multiply-add contraction:

```text
Y = (G + red_coefficient * (R - G)) + blue_coefficient * (B - G)
out[c] = Y + amount * (in[c] - Y)
```

Each mapped channel gets one final float32 cast. `amount == 1` bypasses all
mapping arithmetic after input validation. Numerically equal R/G/B also bypass
mapping at every amount. These bypasses preserve all input bits, including mixed
signed-zero neutrals and extreme finite headroom. Nonidentity colored input is
not clipped, so saturation can create negative or above-one channels. Working Y
is preserved mathematically; output float32 rounding permits a small residual.

The new code uses the existing strict ToneOps translation unit (`/fp:strict` on
MSVC, `-ffp-contract=off` elsewhere). Other translation units/math are unchanged.
Cross-platform release qualification remains open.

## Saved operation and integration

```json
{"type":"rawengine.saturation","schema_version":1,"processing_version":2,
 "parameters":{"amount":1.75}}
```

Use the existing full operation record with UUID, enabled, matching supported
input/output domains, one `image` input, normal blend, opacity1 and no masks or
extensions. Parameters are exactly `amount`; malformed/missing/extra fields,
nonfinite values, booleans and unsupported versions reject even when disabled.
Disabled operations follow existing passthrough/domain rules. Enabled camera,
display, encoded sRGB and unsupported working-space descriptors reject.

Explicit manifests use existing C++ execution and Python session sync/jobs/latest,
history, histogram/local analysis, cache and source-footprint APIs. Parameters,
type/version and upstream identity enter the cache signature. Editing saturation
reuses unchanged upstream tiles. No new recipe/request keyword is supplied.

The point edit retains the descriptor and adds no halo. Native final/preview and
supported mip1/2 previews render upstream at the requested level, then map those
float32 samples. This linear map commutes with averaging mathematically, but
float32 reduction/rounding means different orders need not be bit-identical.
Mixed-source branches, conversion, crop/orientation/resize and RAW calibration
use existing graph rules; place enabled saturation after camera calibration.

## Verification and limits — 2026-10-02

Full rebuilt Release CTest passes **34/34 default, 20/20 core-only, 21/21
LittleCMS**. Native controls and six Python cases verify independent affine/Y
truth in both working spaces, exact finite identity/neutral bits, signed values,
headroom, grayscale, luminance within rounding, malformed tiles/nonfinite input,
strict parameters/version/domain/overflow errors, geometry/ROI/mip partition and
footprints, working conversion/mixed sources, copied ownership, cache/jobs/history
and existing analysis APIs. Temporary `saturation-install-v1` contains updated
ToneOps.hpp, binaries, CMake exports and notices. Earlier native binaries remain
in ignored `build-msvc-release/before-saturation/` for old report bindings.

The ignored `build-msvc-release/research/saturation-camera-check.py` covers two
ISO20000 `_DSC1793` background/skin ROIs, both demosaicers/working spaces,
native/mip1/mip2, amounts 0/.5/1/1.75: **96 float32 buffers match independent
NumPy primary-scaling/Y/affine references exactly**, all 96 tile-256/64 comparisons
and identities exact. Maximum working-Y residual is `1.4804309667049154e-08`.
All 12 historical calibrated native/reduced ProPhoto buffers retain previous NR
stage hashes. Eight four-amount encoded crop boards were inspected: grayscale
and reduced/increased chroma behave as expected; increasing saturation also
amplifies the existing ISO20000 chroma grain. These views use the diagnostic
camera matrix/prototype display chain, not a qualified profile or photographic
look. No NR or Adobe equivalence is claimed.

Sixteen native timing cases, seven runs each, warm-input/uncached edit at
256-square output and 64 MiB cache: median of case medians **1.442900 ms**;
cached output **0.349350 ms**. Each new edit adds one miss plus upstream hits;
each following cached request adds zero misses. PERF-013 records API cost and
deferred profiling points, not isolated kernel time or a regression baseline.
Initialization 307.052/290.595 ms is PERF-003. Workflow after input checks is
2.394 s; peak process working set 332,189,696 bytes includes owned full Bayer,
NumPy/cache/copied views, not isolated node scratch. Larger/concurrent requests,
allocation traces and production quality remain unmeasured/open.

Ignored evidence directory:
`tests/rawfiles/_librawops_local/high-iso/saturation-verified-v1/`;
`saturation-camera-check-v1.json` SHA-256
`80d3eaabb0df9093a06c92c079f9bc429fb3eda05579bef9d36d1191b666878d`.
The report binds helper, native binaries, source, decoded/ROI inputs and runtime.
The earlier failed board-helper attempt remains in a separate directory; its
operation-order assumption was corrected before producing verified evidence.

Saturation is an opt-in engine foundation and is now available in the
[simple viewer](RAW_TEST_VIEWER.md), including exact Before/reset and separately
owned native-render checks. The [original bounded vibrance contract](VIBRANCE_CONTRACT_V1.md)
is implemented as a bounded opt-in graph operation and viewer control. Perceptual/HSL controls,
representative visual quality, profiles and release gates remain open.

# Scene-linear curves and affine levels foundation

This checkpoint adds original per-channel piecewise-linear curves and affine
black/white levels. It does not replace the existing reference `ToneCurveNode`
or change recipes/defaults. Midtone gamma, master-plus-channel composition,
spline interpolation, GUI controls, automatic settings and Adobe matching remain
separate work.

## Numerical contract

`ToneOps.hpp` exports `CurvesSettings`, `LevelsSettings`, `CurvesNode`, `LevelsNode`
and setting validators. Each curve has 2..256 float64 `(x,y)` knots in increasing
x order. Coordinates and segment slopes must be finite with magnitude <=65536;
y can decrease or be nonmonotonic. RGB settings apply independently in the
declared scene-linear ProPhoto/D50 or Rec.2020/D65 working space.

Between knots, use the containing line segment. Exact matching x emits the
saved y directly, rounded once to float32. Below/above the first/last knot,
extrapolate that endpoint segment, anchored at the corresponding endpoint.
Signed values and highlight headroom are not implicitly clipped or normalized.
A flat endpoint segment can intentionally map outliers to a constant; this
comes from the chosen curve. Arbitrary curves can change tone/color; they do
not encode a new transfer function or change the declared working primaries.

An identity channel has `x==y` at every knot and bypasses arithmetic exactly
for all finite float32 samples, including signed zero and values beyond its
knot interval. Other channels in the same node can still change. This avoids
identity drift from subtraction/interpolation. Nonfinite inputs reject, including
identity requests. Malformed input tile storage/bounds/descriptors also reject.
Nonidentity outputs outside finite float32 range throw rather than saturate.

Each levels channel defines input_black < input_white and output_black <=
output_white, with the same coordinate/slope limits as a two-knot curve.
Its affine mapping sends the two input endpoints to the two output endpoints
and extrapolates outside them. Equal output endpoints make a constant map.
Matched input/output endpoints are exact identity. Inverted creative curves are
allowed through CurvesNode; reversed levels endpoints reject. No gamma is
implemented in this affine levels foundation.

Slope calculation/interpolation use float64 and one final float32 cast. The
new translation unit disables multiply-add contraction (`/fp:strict` on MSVC,
`-ffp-contract=off` elsewhere); this does not alter older tone or demosaic math.
Numerical policy is fixed for these saved operation identities. Cross-platform
release qualification remains open.

## Saved operations and rendering

Both nodes are explicit opt-in point edits, schema1/processing2:

```json
{"type":"rawengine.curves","parameters":{
  "red":[[0,0],[0.25,0.5],[1,1]],
  "green":[[0,0],[0.5,0.25],[1,1]],
  "blue":[[0,0],[1,1]]
}}
```

```json
{"type":"rawengine.levels","parameters":{
  "input_black":[0.125,0.25,-0.25],"input_white":[0.875,0.75,1.25],
  "output_black":[0,-0.5,0.25],"output_white":[1,1.5,1.25]
}}
```

These snippets specify type/parameters only; use the existing complete operation
record with UUID, schema_version1, processing_version2, enabled, one `image`
input, matching input/output domains, normal blend, opacity1, no masks/extensions.
Enabled nodes require a supported scene-linear working domain. A RAW graph
places them after camera calibration; encoded display samples reject. Disabled
nodes follow the existing exact passthrough/domain validation rules. Missing,
extra, malformed or unsupported parameters/versions reject even when disabled.

No recipe inserts either node. Python uses existing explicit manifests through
`render_manifest`, synchronous/asynchronous/latest jobs, history, histograms and
streamed local analysis; no new recipe/request keywords are added. Parameters,
node/version and upstream identity enter existing cache signatures. A changed
curve reuses unchanged upstream data. Saved old graphs remain unchanged.

Point edits require no additional halo and retain the upstream output descriptor.
Native final/preview and supported mip1/2 previews propagate their requested level
upstream, then apply the curve/levels mapping to those rendered float32 values.
For nonlinear curves this differs from applying a native curve before reduction;
the order is deliberate and tested. Coordinate/source-footprint composition,
mixed-image branches, geometry and upstream convolution use existing graph rules.

## Bounds and verification

Settings copy into immutable nodes. At the maximum 256 knots/channel, knot
storage is 12 KiB and precomputed float64 slopes <6 KiB, excluding small vector
overheads. The node edits its owned input tile result in place; source/cache
storage remains immutable. Per nonidentity sample, binary-search the sorted
knots. No LUT approximation, GPU or SIMD path is introduced.

Native tests use explicit knot/interior/extrapolation tables, per-channel and
nonuniform-knot identities, signed zero/extreme finite samples, affine truth,
reduced mappings, descriptor/partition behavior and malformed/domain/overflow
rejection. Six Python cases use independent linear-scan interpolation and affine
oracles over both working spaces, geometry/ROI/mips, bilinear/Menon calibrated
RAW taps, source-only rejection, branches/convolution, cache edits, jobs/history,
analysis APIs, copied ownership and max-knot/version/parameter/closed errors.

This is a functional bounded foundation. Production tone-control behavior,
representative visual/quality qualification and release/performance gates remain
open; no universal curve, photographic look or Adobe equivalence is claimed.

## Camera evidence and measured cost — 2026-10-01

Full rebuilt Release CTest passes 32/32 default, 19/19 core-only and 20/20
LittleCMS. The temporary `build-msvc-release/tone-install-v1` installation
includes ToneOps.hpp, native binaries, CMake exports and existing notices.

Run the ignored `build-msvc-release/research/tone-camera-check.py` with the
existing Anaconda research runtime against the local extraction and ROI plan.
Twenty-four cases cover both ISO20000 `_DSC1793` background/skin ROIs, both
demosaicers, native/mip1/mip2 and both operations. Independent NumPy interior
interpolation with explicit endpoint extrapolation and an affine oracle match
every float32 output exactly (maximum error zero). All 24 tile-256/64 comparisons
and 24 identity renders are exact; the 12 unique calibrated input buffers match
the earlier NR stage report. These are numerical controls, not selected looks.

Eight native timing cases use seven runs each on the i9-14900K, Release MSVC,
64 MiB cache and 256-square outputs. Median of case medians for warm-input,
uncached edits is 1.694700 ms curves / 1.832250 ms levels; cached-output requests
take 0.441000 / 0.473300 ms. Every uncached edit adds exactly one miss and reuses
upstream cache data; subsequent cached requests add zero misses. PERF-011 in the
plan records measured API cost and deferred profiling points. It includes graph
parsing, signatures, cache insertion/copying and Python bytes, with no comparable
previous primitive baseline. Full-frame, maximum-knot and concurrent costs and
allocator traces remain unmeasured.

Source initialization takes 296.986 / 293.847 ms for bilinear/Menon (PERF-003).
Workflow after input checks is 1.210 s; peak process working set 319,139,840 bytes
includes full owned Bayer data, NumPy and cache, not isolated node scratch.
The ignored report `tests/rawfiles/_librawops_local/high-iso/curves-levels-v1/`
`tone-camera-check-v1.json` binds helper/native/source/decoded/ROI/runtime inputs;
SHA-256 `9dac510952bb7f4eb07e6483bbc474255a2c5a296b0a3b353e88f2119fa1e6cb`.
Earlier binaries were preserved under ignored `build-msvc-release/before-tone/`
before rebuilding; old reports retain their original binary bindings.

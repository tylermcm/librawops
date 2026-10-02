# Bounded scene-linear 1D LUT v1

Status: functional opt-in engine foundation. `Lut1DSettings`, its validator and
`Lut1DNode` in ToneOps.hpp describe three caller-supplied RGB sample arrays with
equal counts of 2..256, and shared `input_min`/`input_max` (default 0/1).
Default arrays are `[0,1]`. Endpoints must increase strictly. Endpoints, samples
and segment slopes must be finite with magnitude <=65536. Generated grid points
must increase strictly; ranges whose floating-point grid collapses reject.

Input/output is scene-linear ProPhoto/D50 or Rec.2020/D65 float32 RGB with the
same descriptor. This is a table of output channel values, not a strength control,
color-space transform or display-encoded look. No cross-channel dependence,
clipping, normalization, gamma or implicit transfer conversion. File loading,
larger tables and 3D LUTs remain follow-ups.

The first and last sample coordinates are the exact supplied endpoints. For an
interior sample i of N, strict binary64 without fused multiply-add generates:

```text
x[i] = input_min + (input_max - input_min) * (double(i) / double(N - 1))
```

The immutable table delegates to the existing curve mapper. Between coordinates,
use `y[left] + (value - x[left]) * slope[left]`, where slope is the once-generated
binary64 `(y[next]-y[left])/(x[next]-x[left])`. Exact coordinates emit saved sample
values. Below/above the range, extrapolate the first/last segment, anchored at the
first/last coordinate. Signed values and highlight headroom survive; outputs must
fit finite float32 and cast once. A constant table stays constant outside its
range; descending/nonmonotonic tables are permitted within the slope bounds.

A channel is exact identity only when every supplied sample numerically equals
its generated binary64 coordinate. That channel copies input float32 bits at all
finite values, including signed zero, subnormals and extreme out-of-range values.
There is no approximate identity tolerance. Nonidentity channels follow the
mapping, with no signed-zero promise. All input samples, storage, bounds and
descriptor are validated even when all channels are identity.

Saved operation `rawengine.lut1d`, schema1/processing2, requires exactly:

```json
{"input_min":0,"input_max":1,"channels":[[0,0.25,1],[0,0.5,1],[0,0.75,1]]}
```

Reject missing/extra/wrong-shaped/boolean/nonfinite/bound violations and unsupported
versions even disabled. Enabled execution requires matching supported working
domains, one image input, normal blending, opacity1 and no masks/extensions.
Disabled operations follow existing exact passthrough/domain behavior.

Native final/preview and mip1/2 preview map the requested upstream float32 level.
Nonlinear table mapping generally differs from mapping native pixels before
averaging; no alternative reduction is introduced. No halo or geometry changes.
Existing footprints/cache identities/jobs/latest/history/read-only analysis apply.
Ranges, samples, upstream, descriptor, type/version and requested level separate
cache signatures. Default recipes and prior curve math stay intact.

Qualification: independent rational interpolation/extrapolation truth, exact
knots/identity, finite/overflow and strict manifest errors, requested-level/ROI/
partition/geometry/conversion/mixed-source behavior, cache/history/jobs/ownership,
full build configurations and bounded real-camera/performance evidence. Camera
profiles, chosen looks, Adobe equivalence, UI controls, full-frame/concurrent cost
and production quality remain separate gates.

## Verification — 2026-10-02

Full rebuilt Release CTest passes **40/40 default, 23/23 core-only and 24/24
LittleCMS**. All 14 focused suites, native controls, seven Python cases with
independent rational references, eight viewer tests and ten adapter tests pass.
Installed headers, binaries, CMake exports and notices were verified in ignored
`build-msvc-release/lut1d-install-v1`. Viewer controls are unchanged.

Ignored `build-msvc-release/research/lut1d-camera-check.py` checks two ISO20000
background/skin ROIs, both demosaicers and working spaces, native/mip1/mip2 and four
tables: identity, 5-sample contrast/color and 256-sample quadratic. All 96 outputs
match independent binary64 barycentric references exactly (maximum absolute error
zero). Tile-256/64 comparisons and identities are exact; twelve historical
calibrated inputs are unchanged. Eight encoded comparison boards were inspected:
the contrast/color/quadratic tables produce the expected tonal/channel changes
through the diagnostic camera/display chain. No selected photographic look or
profile/Adobe qualification follows from these development crops.

Sixteen native timing cases, seven repeats each, use warm input with uncached
edits and cached outputs. The median of case medians is 3.703900/0.654250 ms
(PERF-016), at 256-square output with a 64 MiB cache. Each new edit adds one miss
and upstream hits; cached requests add zero misses. Timing includes graph/table
validation, signatures, caching and copying. It does not isolate kernel cost or
establish a regression. Initialization of 293.610/278.761 ms remains PERF-003.
The workflow took 2.899 s. Process memory records include full Bayer storage,
NumPy, cache and boards; they do not isolate scratch or establish a global memory
budget. Full-frame, concurrent and allocator costs are unmeasured. Repeated grid
expansion, validation, signatures, copying and maximum-table lookup remain
profiling points, with numerical and version semantics preserved.

Bound report: ignored `tests/rawfiles/_librawops_local/high-iso/lut1d-verified-v1/lut1d-camera-check-v1.json`,
SHA256 `2ee5804c2e7544644c8ac5973b0be7f6e0c5eeb27c3c38cbb959cf90da11ffd1`. It records helper/core sources/native/decoded/ROI/runtime/board
hashes and Windows memory counters. DLL `402a07ce7fcfad8b49ab2f38b65a58b6bf79797f6ff2969383ffdc0b49ed710d`;
pyd `178fa652823a9e757cca5ae5cebecfcabf36feaef4b0930dccedcb3ec50bb320`. Earlier mixer binaries are preserved in
`before-lut1d/`; their report remains historical. Menon, frozen policies, defaults
and viewer sources are unchanged. Graphify has 2,463 nodes, 5,445 links and 127
communities, with 18 expected warnings for data-only JSON files.
Larger tables, file parsing, 3D LUTs, UI and production qualification remain open.

# Bounded scene-linear 3D LUT contract v1

Status: **functional opt-in native foundation; production qualification open**.
This defines an original caller-supplied RGB lookup table with cross-channel
dependence. The engine executes explicit `rawengine.lut3d` saved graphs. Existing 1D
LUT, channel mixer, default recipes and viewer behavior remain unchanged.

## Settings and domain

The exported public interface is `Lut3DSettings`,
`validate_lut3d_settings` and `Lut3DNode` in ToneOps.hpp/.cpp. Settings are copied
into immutable node state. Input and output must have the same supported
scene-linear ProPhoto/D50 or Rec.2020/D65 descriptor and float32 RGB storage.
The table is interpreted in that declared working space.

- `size`: one integer N in [2,17], used on all three axes. The default is 2.
- `input_min`, `input_max`: three binary64 RGB endpoints each, default [0,0,0]
  and [1,1,1]. Every endpoint is finite with magnitude <=65536. Each axis has
  strictly increasing endpoints and generated coordinates.
- `values`: exactly `3*N*N*N` finite binary64 output-channel values, each with
  magnitude <=65536. Negative, over-one, descending and nonmonotonic values
  are permitted within the bounds below. Default values are the size-2 identity
  cube over the default input box.

Generate each axis independently. The first and last coordinates are the exact
supplied endpoints. Interior coordinate i uses strict binary64, without
multiply-add contraction:

```text
x[c,i] = input_min[c] + (input_max[c] - input_min[c])
                           * (double(i) / double(N - 1))
```

Reject a collapsed floating-point grid. For every adjacent lattice pair along
each input axis and each output component, compute the binary64 edge slope
`(next_value - previous_value) / (next_coordinate - previous_coordinate)`.
Every slope must be finite with magnitude <=65536. This bounds lattice edges;
it is not a global derivative bound under extrapolation outside the input box.

Validate N before products, table allocation or traversal, and validate exact
storage length before indexing. At N=17 the table contains 4,913 RGB triples or
14,739 binary64 scalars: **117,912 bytes of value payload**, plus 408 bytes of
axis coordinates. This is a per-node data bound, not a global graph/cache,
JSON-parser, session or process-memory limit. Existing manifest byte/depth
limits also apply. Table parsing and validation can need additional copies.

There is no strength parameter, clipping, row normalization, automatic working
conversion, gamma or implicit display transfer. File formats, external assets,
shaper LUTs, tetrahedral interpolation, larger grids, GPU support, UI controls,
chosen photographic looks and Adobe equivalence are separate follow-ups.

## Storage layout

Input-axis indices are r, g and b. Red changes fastest, then green, then blue.
Each lattice entry contains three adjacent output values in RGB order:

```text
values[3 * ((b * N + g) * N + r) + c]
```

Here c is the output component, independently of the three input-axis indices.
This layout is part of the saved processing contract. It does not promise a
particular file-format layout. Use asymmetric tables to test all axis and
component permutations; a neutral cube alone cannot expose ordering mistakes.

## Cell selection and interpolation

Validate the upstream tile's bounds, descriptor, storage length and **all three
finite input samples** before bypasses or table lookup. Identity or a component
that ignores an input axis does not permit nonfinite input.

For each input component v, locate its cell on the generated axis with
`upper_bound`. Select `left = clamp(upper_bound(v)-1, 0, N-2)` with an explicit
below-first branch, so no unsigned subtraction underflows. Select the first
cell below the box and last cell above it. Clamp only the cell index; retain
the original v. Compute the binary64 fraction:

```text
t = (double(v) - x[left]) / (x[left+1] - x[left])
```

The three fractions can be below zero or above one. Trilinear evaluation extends
the selected boundary cell in every out-of-range axis. A point beyond an edge
or corner therefore uses a multilinear continuation; it is not nearest-color
clamping, separate per-channel extrapolation or a photographic highlight policy.
Cross terms can grow rapidly outside the box. Reject nonfinite arithmetic or
outputs exceeding finite float32 range; do not saturate the result.

Pin the following binary64 scalar interpolation helper, without contraction:

```text
L(a,b,t):
    if t == 0: return a
    if t == 1: return b
    if a == b: return a
    if t > 1: return b + (t - 1) * (b - a)
    return a + t * (b - a)
```

The bypass order is significant. Exact endpoints select their saved value;
equal numeric endpoints select a elsewhere, including when zero signs differ.
No approximate-equality or near-endpoint tolerance is permitted. All intermediate
values must remain finite; intermediate values may exceed float32 range if the
final mapped component fits. Fractions must be finite. Validate final components
before casting each once to float32.

For each nonidentity output component c, take the eight corners of the selected
cell and evaluate red first, then green, then blue:

```text
a00 = L(V[r0,g0,b0,c], V[r1,g0,b0,c], tR)
a10 = L(V[r0,g1,b0,c], V[r1,g1,b0,c], tR)
a01 = L(V[r0,g0,b1,c], V[r1,g0,b1,c], tR)
a11 = L(V[r0,g1,b1,c], V[r1,g1,b1,c], tR)
b0  = L(a00, a10, tG)
b1  = L(a01, a11, tG)
out[c] = L(b0, b1, tB)
```

Axis order and endpoint anchoring are numerical-version behavior, even when
alternative formulas are mathematically equivalent. Exact lattice vertices
emit their saved RGB values. Nonidentity signed-zero behavior follows the
helper and final cast; there is no general bit-preservation guarantee.

## Exact identity

An output component c is exact identity only if, at **every** lattice entry,
its saved numeric value equals the generated coordinate of the matching input
axis: output R equals x[R,r], G equals x[G,g], B equals x[B,b], independently
of the other two indices. Use exact numeric equality, with no tolerance.

That component copies the matching input float32 bits for every finite input,
including values outside the box, signed zero, subnormals and extreme finite
headroom. This avoids arithmetic drift or overflow in an identity cube. Other
components continue through interpolation. Finite input/tile validation remains
mandatory before copying. A full identity cube copies all three components
before computing cell indices or fractions, so a tiny valid input box cannot
overflow a needless normalized coordinate. For partial identity, remaining
components can still fail interpolation/overflow validation and reject the tile.
Channel permutations, affine tables and approximate identities have no additional
bypass in v1; they follow the frozen evaluation and overflow rules.

## Saved graph and rendering

The saved operation is `rawengine.lut3d`, schema1/processing2, with exactly four
parameters: `size`, `input_min`, `input_max` and `values`. `size` must be a JSON
integer, excluding booleans and floating-point spellings such as 2.0. Endpoints
and values accept finite numeric scalars, excluding booleans. Endpoint arrays
have exactly three elements and `values` is one flat array.

For example, the size-2 identity cube is:

```json
{
  "size": 2,
  "input_min": [0,0,0],
  "input_max": [1,1,1],
  "values": [
    0,0,0, 1,0,0, 0,1,0, 1,1,0,
    0,0,1, 1,0,1, 0,1,1, 1,1,1
  ]
}
```

Reject missing/extra/wrong-shaped/nonfinite/bound violations
and unsupported schema/processing versions even disabled. Enabled execution
requires one image input, matching supported working domains, normal blending,
opacity1 and no masks/extensions. Disabled execution follows the existing
validated exact passthrough and domain rules. The example supplies the parameters
for an enabled operation with matching supported working domains.

Native final/preview and mip1/2 preview propagate the requested level upstream
and map those returned float32 RGB pixels. Nonlinear cross-channel mapping can
differ from mapping native pixels before averaging; the node does not alter
source or calibrated reduction order. No halo, geometry or descriptor changes.
Existing geometry/conversion/mixed-source footprints, cache/jobs/latest/history
and read-only analysis remain the integration path. Size, ranges, values,
type/version, actual descriptor, upstream and requested level enter cache
identity. No new source fingerprint, default recipe or decoder dependency.

## Implementation and qualification gates

1. Add the exported copied-settings node and strict saved-graph validation.
   Keep prior math/defaults intact and pin floating-point compiler behavior.
2. Verify the grid, flat layout and numerical rules with independent exact-rational
   references. Include asymmetric affine and RG/GB/RB/RGB cross-term tables,
   exact vertices, cell interiors/faces, all boundary directions and multiple-axis
   extrapolation, unequal axis ranges and non-dyadic grid sizes. Trilinear
   cross-term truth must distinguish it from tetrahedral interpolation.
3. Verify exact full/partial identity bits at N=2,3,7,17, including signed zeros,
   subnormals and extreme finite values; near-identity edits must still execute.
   Cover constant tables, descending outputs, malformed counts/types, collapsed
   grids, edge-slope bounds, nonfinite inputs/intermediates, final overflow,
   disabled validation, versions, domain mismatches and copied ownership.
4. Verify native/mip/ROI/tile parity, the requested-level counterexample, no added
   halo, geometry, actual working conversion and mixed-source footprints. Verify
   size/range/table-specific cache edits, jobs/latest, saved history and analysis.
   Run the rebuilt default/core/LittleCMS suites and inspect the installed API,
   binaries, exports and notices. Archive previous binaries before rebuilding.
5. Use the existing ISO20000 background/skin development ROIs, both demosaicers
   and spaces, native/mip1/mip2, identity and asymmetric/cross-channel tables.
   Include the maximum grid, independent references, partition/identity checks,
   historical input hashes and inspected diagnostic display boards. Bind reports
   to source/helper/native/decoded/ROI/runtime/output hashes.
6. Measure bounded API costs after concurrent tests finish: warm-input uncached
   edits and cached outputs, seven repeats, fixed 256-square ROI/64 MiB cache,
   minimum and maximum grids. Record cache misses/hits, initialization and
   Windows process memory separately. PERF-017 tracks measured bounded API cost
   alongside unresolved profiling and full-frame/concurrency gaps.
7. Update the authoritative handoff, feature matrix and checklist with actual
   evidence. Implementation alone does not close file/UI/production color,
   profile/camera/corpus, Adobe, full-frame/concurrent or release gates.

## Contract-only verification — 2026-10-02

Before implementation, the independent ignored contract checker validated example layout, maximum payload
counts, exact-rational interpolation of asymmetric affine and cross-term tables,
vertices, interiors and simultaneous out-of-range axes, with unequal ranges.
It also exercises the specified binary64 grid/helper order on bounded fixtures,
identity classification and near-identity rejection. Those checks assessed the
contract; they did not test a native implementation or establish render
performance. Its report and checker remain historical and bind the pre-native
contract and DLL/pyd archived in `build-msvc-release/before-lut3d/`.
Do not rerun that helper unmodified against the implemented type: its former
unknown-type rejection is no longer the expected behavior. The independent
native/graph/camera verification below supersedes its implementation status.

## Native verification — 2026-10-02

Full rebuilt Release CTest passes **42/42 default, 24/24 core-only and 25/25
LittleCMS**. Native controls and seven Python integration cases verify independent
polynomial/rational truth, cross terms, layout, vertices, maximum and non-dyadic
grids, signed/headroom extrapolation, exact identity bits, strict errors, requested
levels and graph/cache/jobs/history/analysis/geometry/footprints. Tests explicitly
cover fraction/intermediate overflow, tiny-box identity and finite binary64
intermediates above float32 range that cancel before the final cast. Sixteen
focused suites pass; the final added fixtures pass the focused two-test 3D suite
and all full matrices. Eight viewer and ten adapter tests pass with unchanged UI.
Installed headers, binaries, CMake exports and notices were verified in ignored
`build-msvc-release/lut3d-install-v1`.

Ignored `build-msvc-release/research/lut3d-camera-check.py` covers two ISO20000
background/skin ROIs, both demosaicers/spaces, native/mip1/mip2 and four tables:
identity, size-2 asymmetric mix, size-3 cross terms and size-17 piecewise quadratic
with cross terms. All 96 outputs match independent float64 eight-corner
barycentric references within float32 rounding (72 exact, maximum absolute
error 2.98023224e-08). All tile-256/64 and identity comparisons are exact; twelve
historical calibrated input hashes remain unchanged. Eight encoded boards were
inspected: channel changes follow the tables and quadratic mapping darkens tones.
These diagnostic camera/display crops do not qualify a photographic look, profile,
production color behavior or Adobe equivalence.

Sixteen timing cases, seven repeats each, use warm calibrated input with uncached
edits and cached outputs, 256-square output and a 64 MiB cache. Median case medians
are 10.380950/3.525100 ms uncached/cached overall; size-2 mix 6.737600/0.286450 ms, size-17 dense 13.209300/7.107150 ms.
Each uncached edit adds one miss and upstream hits; cached requests add zero
misses. The maximum table remains expensive even with cached pixels. PERF-017
records this measured API cost and parsing/compilation/edge-validation/signature/
copying/lookup profiling points. Timing includes graph and Python-returned bytes;
it does not isolate stages or establish a controlled regression. Full-frame,
concurrent and allocator costs remain unmeasured. Initialization 292.145/275.535
ms remains PERF-003. Workflow 5.227 s; peak working set 334,376,960 bytes and peak
commit 1,036,877,824 bytes include full Bayer storage, NumPy, cache and boards.
These counters do not isolate LUT scratch or establish a global memory budget.

Bound report: ignored `tests/rawfiles/_librawops_local/high-iso/lut3d-verified-v1/lut3d-camera-check-v1.json`,
SHA256 `bcbe138fb48e48b2897a650576fda15fb4bfd47dfe09af6c0627686dfb98e81e`. It records core source/helper/native/decoded/ROI/runtime/board
hashes and Windows memory counters. DLL `4bfc2b4b9510fca677fd17bb3552b4422f574e4f2664acd316d28a3780dce493`;
pyd `913560e28004dc87a112c36239f593cfaf86ae4ef1002957bd6910a1c04095a3`. Earlier 1D binaries and the pre-native contract
are preserved in `before-lut3d/`. The original checker/report binds that archived
contract and binaries; its unknown-type assertion is historical. Menon, frozen
policies, defaults and viewer sources are unchanged. Graphify has 2,525 nodes,
5,592 links and 132 communities, with 18 expected data-only JSON warnings.
File formats, larger grids, UI and production/release qualification remain open.

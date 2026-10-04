# Typed mask graph and masked RGB acceptance

2026-10-04 bounded core acceptance, worktree based on HEAD `d3c4617`.
The [frozen contract](MASK_GRAPH_CONTRACT_V1.md) adds typed scalar coverage
nodes to the existing executable edit graph. Coverage inversion and explicit
add/subtract/intersect algebra feed named mask ports on a scene-linear RGB
adjustment. Coverage remains a scalar payload; RGB defaults and the accepted
coverage source/alpha arithmetic remain unchanged.

## Implemented scope

[MaskOps.hpp](../MaskOps.hpp) and [MaskOps.cpp](../MaskOps.cpp) expose
CoverageNode, CoverageRasterNode, CoverageInvertNode, CoverageCombineNode and
MaskedMixNode. Native Final/Preview and mip-1/2 Preview use requested-level
source reduction before algebra or mixing. Masked RGB uses staged binary64
arithmetic, exact endpoint copies and complete actual-tile validation.

Explicit manifest format 4 admits `coverage_raster_f32` source records,
`coverage` edges and `rawengine.mask_invert`, `rawengine.mask_combine`,
`rawengine.masked_mix` operations, schema 1/process 2. Existing format-1 migration
and format-2/3 defaults remain; format 4 retains explicit Bayer reconstruction
identity. Typed binding checks reject source substitution, wrong content, wrong
extent and RGB/scalar confusion. Disabled operations validate saved settings and
edges, then alias only the base branch's runtime identity and source footprint.

The existing TileCache shares one budget/LRU/generation across RGB and scalar
payloads, with distinct payload-kind keys. Enabled signatures incorporate every
upstream RGB/mask digest. Source-ID footprint planning includes mask dependencies
and composes existing RGB crop/reduction anchors and neighborhood halos.
Core history serializes, restores and pins either output type. The existing RGB
renderer/scheduler executes masked RGB; scalar output uses explicit typed
accessors and rejects RGB entry points.

## Verification

The [independent Fraction oracle](../tests/reference/mask_graph_numeric_oracle.py)
pins **324 algebra cases and 180 staged RGB mixing cases**, plus a full 7x5
two-source/two-mask chain at native/mip-1/mip-2. It imports the unchanged exact
numeric fixture helpers, without importing the engine or a scientific package.
Tiny coverage, double amount extremes, finite signed/headroom/extreme RGB and
endpoint bits are covered. The mapped-range guard is defensive; these valid
convex-mix cases do not claim an exercised overflow branch.

[Native tests](../tests/mask_graph_tests.cpp) verify exact frozen outputs in both
working spaces, native Final/Preview and reduced Preview, 1/2/8 tile partitions
and every single-pixel ROI. Tests cover actual malformed tile rejection,
strict domains/ports/parameters/schema/process/enabled-disabled behavior,
mask-only cycles, unknown-field preservation with fail-closed execution,
source fingerprint/extent/type binding, disabled scalar/RGB bypass footprints,
shared runtime sources retaining distinct source IDs, mask-content/signature
invalidation, consumer-edit upstream reuse, scalar/RGB cache key separation,
shared budget, copied return ownership and a promise-gated clear-generation
test. Crop/blur composition passes native/reduced partition and explicit native
footprint checks. Format-4 RAW replay agrees with format 3 and requires the
explicit policy; existing full suites retain format-1/2/3 regression coverage.

Core format-4 history commit/undo/redo/restore and pinned output after owner
destruction pass. RGB job output is exact at native/mip-1/mip-2. A one-shot
source-boundary bad_alloc reaches its caller and a following request on the
same one-worker scheduler succeeds. This is source-fault recovery evidence;
no internal allocator injection, cancellation-latency or guaranteed queued-
behind-fault race test is claimed.

Full Release suites: **82 default / 46 core / 47 LittleCMS / 83 LittleCMS + Python**.
Source/native/log bindings: `build-msvc-release/research/mask-graph-full-v1/verification.json`,
SHA256 `c75c5e0008f59c8e20b2bfd2885b7a9b200e05f297434a468f72bbdcfd9125ad`. Frozen contract SHA256 `5e7e5d4db7289baf39a6c15f2f7f8867c3ed48106078c9cbc0ef189deb093411`.

Fresh default/LCMS installed C++ consumers use installed headers and exported
RawEngine targets. Mask graph tests, the 20-source/14,287 ROI coverage source
tests and all 616 alpha numeric fixtures pass in both installations. Copied,
installed and build DLL hashes agree. Receipt:
`build-msvc-release/research/mask-graph-installed-v1/verification.json`,
SHA256 `f03f43ea655bfc71e1b7a07930fde2c3d43bdcb8d35608d1db25032ba3247d7b`.

Exact pre-mask source-era CMake, graph files, source/helper files, four DLLs,
Python modules and handoff documents are preserved under
`build-msvc-release/research/before-mask-graph-v1`, with bindings.json.
Older accepted receipts remain immutable and describe their historical binaries.
CoverageSource/CoverageOps and their oracle fixtures remain byte unchanged.
Full/installed orchestration and logs are in ignored research storage; use fresh
versioned output/install/build directories for reruns.

```bat
build-msvc-release\research\run-with-windows-env.cmd C:\Users\tylle\anaconda3\python.exe tests\reference\mask_graph_numeric_oracle.py --check
build-msvc-release\build-pinned-camera.cmd
build-msvc-release\research\run-with-windows-env.cmd build-msvc-release\research\run-ctest-tool.cmd --test-dir build-msvc-release -R MaskGraph --output-on-failure
```

## Remaining scope and performance policy

Python coverage sources/session/history ownership, dedicated scalar streaming/jobs,
brush/raster painting, parametric/range/feather nodes, asset persistence and RGBA
graph/layers remain required. This core gate does not complete the original
reusable mask or coverage/alpha rows. Checklist stays **145 checked / 135 open /
280 total**. No Python coverage API, UI expansion, commit or push was added.
Tests/research/viewer artifacts are not installed; the UI remains testing-only.

Feature completion takes priority over performance fixes. PERF-039 records the
code-observed repeated validation/ownership cost as an unmeasured deferred gap.
PERF-028 retains ordinary correctness-suite workflow timings and confounders.
No dedicated profile, scale benchmark or optimization was performed.

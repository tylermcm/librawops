# Extended LUT and bounded Cube text import v1

Status: functional bounded implementation; engine/file contract frozen before coding. Broader production LUT/color qualification remains open.

Keep existing `rawengine.lut1d` (2..256) and `rawengine.lut3d` (2..17) schema1/
processing2 behavior and saved recipes unchanged. The graph currently requires
operation processing versions to match its processing2 document; introduce new
types instead of silently expanding legacy types or changing document defaults.

`rawengine.lut1d_large`, schema1/processing2: same shared-domain table parameters
and interpolation/extrapolation/identity/slope bounds as the existing 1D type,
with equal channel sizes2..4096. Export LargeLut1DNode and separate validator.
`rawengine.lut3d_large`, schema1/processing2: same axes/red-fastest layout/ordered
trilinear/boundary extrapolation/identity/finite/slope bounds as existing3D,
with cubic sizes2..33. Export LargeLut3DNode and separate validator. Both reuse
existing settings representations; the selected node/validator fixes size limits.
Legacy nodes/validators/curves retain their original bounds.

Maximum large1D numeric values:98,304 bytes, plus compiled knots/slopes.
Maximum large3D values:862,488 bytes, plus792 axis bytes. These are table payloads,
not parser/session/history/cache/process memory caps. Existing manifest limits
still apply. Validate size/storage before products, allocation and indexing.

Original `CubeLut.hpp/.cpp` implementation reads a bounded subset of Cube syntax.
Syntax/layout provenance: Adobe's 2013 [Cube LUT Specification1.0, sections5–7](https://kono.phpage.fr/images/a/a1/Adobe-cube-lut-specification-1.0.pdf), authored
by Adobe and accessed through a mirror because the original Adobe URL is unavailable.
No sample implementation, sample LUT, document or new dependency is incorporated.

Accept one `LUT_1D_SIZE` or `LUT_3D_SIZE`, optional quoted ASCII `TITLE`,
`DOMAIN_MIN`/`DOMAIN_MAX`, and RGB numeric rows. Defaults are domain0..1;3D data
is red-fastest. Keywords precede data and occur once. Require exact row count,
finite decimal/exponent numbers, no unknown keywords. Accept LF and CRLF and
full-line comments/blank lines; no inline comments, BOM or CR-only lines.
Cap text/file bytes at4 MiB and line content at250 bytes; ASCII text/tab only.
Reject combined shaper/cube tables and range dialects. Large1D uses shared bounds,
so unequal per-channel1D domains reject. Table size/value/domain/slope bounds
remain those of the selected native operation. This is a bounded importer;
the full format's larger tables and recommended tetrahedral mapping are outside
this contract. Imported3D uses the engine's explicit trilinear semantics.

Caller must explicitly declare scene-linear ProPhoto/D50 or Rec.2020/D65 when
parsing/loading; Cube carries no trustworthy transfer/profile declaration.
No inferred log/display transform, scaling or automatic conversion. Native API:
parse text, bounded load file, build a matching node, construct a saved operation.
Python parse/load return copied operation parameters/type/versions plus title and
working-space metadata. Small tables select legacy types; larger select new types.
Save numeric values in the recipe, not a live file path. Later file mutation or
deletion cannot change the replay. Existing signatures identify actual values,
domain/type/version/level/upstream; comments/title/filename are not render identity.

Tests: independent affine/cross-term/piecewise tables and both maximum sizes,
identity/component bits/signed-headroom/overflow, legacy rejection of larger
tables, strict types/versions even disabled, malformed/duplicate/oversized files,
decimal grammar/ASCII/row/layout/domain failures, exact parse-to-inline replay,
explicit working-domain rejection, file mutation/ownership, graph/mip/ROI/tile/
cache/jobs/history/analysis/conversion/mixed sources, full builds/install and
camera/visual/performance gates. Preserve resource/validation cost evidence under
PERF-017 and new file-import measurements; avoid unsupported optimization claims.
Photographic looks/profile/corpus/Adobe qualification stays separate. UI unchanged.


## Implementation and verification — 2026-10-02

Exported native nodes and strict schema1/process2 saved types are implemented.
Graph/cache/jobs/latest/history/analysis/geometry/working conversion/mixed-source
and native/preview mip1/2 behavior are covered. Full Release CTest passed
**54/54 default, 29/29 core, 30/30 LittleCMS**; native grading/Cube cases and
7 grading, 7 large1D, 7 large3D and 5 Cube Python cases pass. Independent truth
uses Fraction/80-digit Decimal and separate barycentric/piecewise references.
Legacy LUT/curve limits remain fixed and oversized legacy recipes reject even
disabled. File imports own copied values; Unicode paths, file changes/deletion,
malformed numeric/header/domain/layout/storage/byte limits are tested. Native
producer/domain/finite/overflow/bypass and allocation-path cleanup were reviewed.
Viewer9/9 and optional adapter10/10 pass; UI source and historical window
evidence are unchanged and excluded from installation.

Camera report `tests/rawfiles/_librawops_local/high-iso/extended-lut-verified-v1/extended-lut-camera-check-v1.json` SHA-256
`685e393c9b326dcbd0dcb46f7efc0bec3308fac494d4f28ea82968858de64a1f`.
96 original references, 71 exact, maximum error 1.49011612e-08; all partition/identity checks exact.
Twelve unique historical calibrated inputs are unchanged in each report.
Two ISO20000 development ROIs, two demosaicers, both working spaces and mip0/1/2;
eight boards per family inspected. Grading lift/gamma change shadow brightness
and mixed controls add a tint; existing high-ISO chromatic grain remains visible.
Large LUT identity boards match and mild cross/curve tables shift tone/chroma;
no new tile seams were observed. This diagnostic camera/display chain does not
qualify production skin/profile appearance, denoise or Adobe compatibility.
Luminance-target errors compare the rounded RGB oracle's native-Y, not an
unrounded ideal luminance or a luminance-preservation promise.

Sixteen timing cases, seven repeats each, 256-square output, 64 MiB cache,
warm calibrated input: median of case medians uncached/cached
45.047900/34.929150 ms.
Every uncached edit adds one miss and reuses upstream; following requests add
zero misses. These are synchronous API costs including graph parsing,
validation/signatures/cache and returned copies, not isolated kernels or a
controlled regression. Whole-process memory includes full Bayer ownership,
NumPy/cache/boards; no node scratch/global cap claim. PERF-022/023 in the
authoritative plan retain stage/full-frame/concurrency/allocator profiling gaps.

Reproduce with restored Windows profile/temp/SystemRoot variables and explicit
Anaconda Python3.9: `build-msvc-release/build-pinned-camera.cmd`, full CTest in
the default/core/LittleCMS trees, then ignored research camera helpers
`grading-camera-check.py` and `extended-lut-camera-check.py` sequentially after
tests. Helpers refuse to overwrite completed output directories; use a fresh
version for another run. `cube-import-benchmark.py` binds original max-size
tables and parse/load costs. Install prefix `grading-lut-install-v1` includes
matching DLL/cp39 extension, seven public headers including CubeLut.hpp, CMake
exports and two existing notices; no new third-party code/data/dependency or UI.

Cube import benchmark `build-msvc-release/research/cube-import-benchmark-v2.json` SHA-256
`02bf9341ac810499c099301c84ff8d54c8700531ee49b815061613e63886efa1`.
Seven repeats per API/space with warm file pages: 4096-sample parse
3.671900/3.714000 ms, load4.037600/4.109600 ms; 33-cube parse32.020000/32.192100 ms,
load34.050300/34.050700 ms (ProPhoto/Rec.2020). LF text245,674/1,931,219 bytes, CRLF file249,772/1,967,158 bytes;
canonical parameters236,727/1,276,368 bytes. These include validation, copied
saved-operation conversion and Python objects, not isolated lexical parsing
or cold disk. Cross33 render API uncached/cached74.283600/65.442750 ms;
curve4096 15.881550/6.952850 ms. Retained output cache still has table-bearing
request overhead; profile repeated compilation/validation/signatures/copies
before considering immutable compiled-table reuse. No optimization or confirmed
root cause follows from these measurements alone.

Frozen pre-native contracts and prior grayscale DLL/cp39 extension plus old
ToneOps/EditGraph/Python/CMake sources are preserved in ignored
`build-msvc-release/before-grading-lut/`. Historical evidence stays bound to
those archived files. Current HEAD advanced externally to85c989d during this
run; remaining changes are local, with no agent commit or push.


Installed runtime smoke also passes: `build-msvc-release/research/grading-lut-install-smoke.py`
imports the staged cp39 extension and verifies Cube import plus exact signed/headroom
grading graph bits. Report `grading-lut-install-smoke-v1.json` SHA-256
`aa820b41e664a22ae9f8de0e90361c6a8860f7317ffb2924b172bd61ef1f5b70`.
On this Anaconda3.9 host, direct staged import failed until the helper retained
stage/host-runtime DLL search handles and explicitly preloaded the staged
RawEngine.dll by absolute path. No binary/dependency change was needed.
Dumpbin lists only existing MSVC/OpenMP/Windows imports (plus python39.dll for
extension). The staged library is verified with the host runtime; standalone
loader/runtime redistribution and clean-machine qualification remain release
gates. Final source/evidence/package audit is reproducible with ignored
`build-msvc-release/research/verify-grading-lut-final.py`, producing
`grading-lut-final-verification-v1.json`; keep its source/doc hashes current.

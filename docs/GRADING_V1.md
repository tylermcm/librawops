# Bounded lift/gain/signed-gamma grading v1

Status: functional bounded implementation; original equations frozen before coding. Broader production color qualification remains open.

This adds creative per-channel grading in actual scene-linear ProPhoto/D50 or
Rec.2020/D65 RGB. It is an original explicitly ordered operator, not a claim of
matching another editor's lift/gamma/gain controls, exposure or white balance.

`GradingSettings`: RGB arrays `lift` (finite [-1,1], default0), `gain` (finite
[0,4], default1), `gamma` (finite [.25,4], default1). Copy settings/input into
immutable `GradingNode`. Validate every finite sample and exact tile storage,
bounds and descriptor before bypass. For each channel:

```text
if lift==0 and gain==1 and gamma==1: copy original float32 bits
scaled = input if gain==1 else double(input)*gain
u = scaled if lift==0 else scaled+lift
y = u if gamma==1 else copysign(pow(abs(u),1/gamma),u)
check y is finite and fits float32; cast once
```

No contraction, clipping, normalization, epsilon, gamut mapping or implicit
transfer conversion. Signed power is defined at zero with its intermediate sign.
Identity components preserve signed zeros, subnormals and extreme finite values;
nonidentity controls may underflow on cast. Genuine output overflow rejects.
Equal controls preserve numeric neutral equality; asymmetric controls can tint
neutrals/black. Each component's map is monotone nondecreasing for the allowed
gain range, continuous at zero but not necessarily differentiable there.
Gamma uses the platform binary64 math library; cross-toolchain bit-exact pow is
not promised. Independent high-precision truth uses a declared float32 tolerance.

Saved `rawengine.grading`, schema1/processing2, has exactly three three-number
arrays `lift`, `gain`, `gamma`; reject bools, missing/extra/shape/range/nonfinite
values and wrong versions even disabled. Enabled operation uses one image input,
matching supported domains, normal blend, opacity1, no masks/extensions. No halo
or geometry change. Native final/preview and preview mip1/2 map requested upstream
float32 samples. Nonlinear grading after reduction differs from grading before
reduction; preserve that order in cache/history/source-footprint contracts.

Verify independent Fraction affine and Decimal signed-power truth, channel/full
identity bits, neutral equality, monotonicity/continuity, negative/headroom,
black lift, boundaries/subnormals, overflow, malformed producers/settings/domains,
requested-level order, graph/cache/jobs/latest/history/analysis/geometry/working
conversion/mixed-source ownership. Run full default/core/LittleCMS builds/tests,
install checks and existing camera reference/partition/identity/visual/performance
gates. Production photographic/profile/corpus/Adobe qualification remains open;
personal aesthetic references do not define or block implementation. UI unchanged.


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

Camera report `tests/rawfiles/_librawops_local/high-iso/grading-verified-v1/grading-camera-check-v1.json` SHA-256
`6b135d46808635becd03988498f4251765a188a058cb3dbc05f134188771fd49`.
96 original references, 96 exact, maximum error 0; all partition/identity checks exact.
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
4.137300/0.297000 ms.
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

# Coverage/alpha numeric foundation acceptance

2026-10-03, working tree based on HEAD `d3c4617`. The
[frozen numeric contract](COVERAGE_ALPHA_CONTRACT_V1.md) is implemented in
[CoverageOps.hpp](../CoverageOps.hpp) and [CoverageOps.cpp](../CoverageOps.cpp).
This accepts dedicated scalar coverage and scene-linear premultiplied RGBA payloads,
validation, premultiplication, safe binary64/checked binary32 straight access,
coverage application and normal source-over. Existing RGB Tile/Node descriptors,
recipes, graph formats and default paths are unchanged.

## Independent evidence

The checked-in [Fraction oracle](../tests/reference/alpha_numeric_oracle.py)
uses integer/rational arithmetic and explicit nearest-even binary64 stage and
binary32 storage rounding, with midpoint/subnormal/carry self checks. It imports
no engine or scientific packages and uses no native floating casts to generate
expected values. Its frozen header contains 496 staged scalar cases and 120
binary64 straight-access cases, including tiny alpha, finite maximum colors,
signed/headroom values, cancellation, storage underflow and expected overflow.

[Native tests](../tests/coverage_alpha_tests.cpp) pass exact bit comparisons for
all 616 fixtures. Additional tests cover transparent hidden color and nonfinite
rejection even on bypasses, canonical positive-zero transparent output, opaque
and unit-coverage signed-zero identity, explicit float32 straight overflow,
an independently calculated signed/headroom composition, layer order, complete
storage/domain/bounds validation, partition parity for all three bulk helpers in
both working spaces and fresh output ownership. These are aligned point helpers;
they do not establish spatial mask/brush/reduction or graph ROI correctness.

## Builds, regression and installed consumers

Full Release suites pass: **78 default / 42 core / 43 LittleCMS /
79 LittleCMS + Python**. Source/native hashes and complete build/test logs are in
`build-msvc-release/research/coverage-alpha-full-v1/verification.json`, SHA256
`431d037f54e5f95eb546c8ef37ec085d0aa923ccf976a02082f1fbf4736e5a13`.
The numeric contract SHA256 is
`bec610ac39e247b880b8f4fa5c5e2c3cfda9e7b3e95a2c146e672bfd4103e24f`.

Fresh default and LittleCMS installed C++ consumers include the installed header,
link the existing exported `RawEngine::RawEngine` target and execute the same
independent fixture/admission/partition tests. Copied/installed/build DLL hashes
agree. Receipt: `build-msvc-release/research/coverage-alpha-installed-v2/verification.json`,
SHA256 `a95c55b2cdec8942659ff8f979d846ecc1c0fccd187decec3180a63b39b20eab`.
The first consumer harness assumed a package Config file; this checkout exports
Targets directly. Its failed configure log is preserved in `coverage-alpha-installed-v1`;
the corrected consumer imports those existing Targets and uses fresh v2 paths.
No package-system change or successful report overwrite was made.

Reproduction on the configured Windows environment:

```bat
build-msvc-release\research\run-with-windows-env.cmd C:\Users\tylle\anaconda3\python.exe tests\reference\alpha_numeric_oracle.py --check
build-msvc-release\build-pinned-camera.cmd
build-msvc-release\research\run-with-windows-env.cmd build-msvc-release\research\run-ctest-tool.cmd --test-dir build-msvc-release -R CoverageAlpha --output-on-failure
```

Full/installed orchestration helpers and their logs remain in ignored local research
storage. Existing receipts are immutable; use new output/install/build versions
when rerunning them. The installed header is added to the library installation;
tests, fixtures, viewers and research scripts are not installed.

## Scope still open

This is an accepted numeric foundation, not completion of the original Phase 5
row. Immutable coverage sources/fingerprints, typed mask/RGBA graph edges,
reduction/ROI/cache/jobs/history/Python and saved-document integration remain
next. Brush/raster mask graph, mask algebra/feathering, local edits and layers
also remain open. Numeric replay is qualified for binary32/binary64 nearest-even
with gradual underflow on this Windows build; broader environments remain a
production qualification gate. No Photoshop blend match is claimed.

Observed development costs are logged under PERF-028 (required full-suite
workflow) and PERF-037 (PowerShell/tool initialization) in
[the ongoing performance log](PERFORMANCE_ISSUES.md). No kernel benchmark,
allocation tracing, optimization or UI expansion was performed. Checklist remains
**145 checked / 135 open / 280 total**. No commit or push.

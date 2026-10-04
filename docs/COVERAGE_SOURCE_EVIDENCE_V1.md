# Immutable coverage source acceptance

2026-10-04 acceptance record, worktree based on HEAD `d3c4617`. The
[frozen source contract](COVERAGE_SOURCE_CONTRACT_V1.md) is implemented in
[CoverageSource.hpp](../CoverageSource.hpp) and
[CoverageSource.cpp](../CoverageSource.cpp). Coverage sources now own canonical,
tightly packed immutable snapshots, provide thread-safe source fingerprint v1,
native render, direct mip-1/2 preview averages and exact native ROI footprints.
They remain separate from the RGB-only Node and Tile interfaces.

## Evidence

The [independent Python oracle](../tests/reference/coverage_source_oracle.py)
pins 20 padded/packed/odd/singleton/extreme-origin sources. It serializes canonical
source bytes with Python struct/hashlib, and uses integer/Fraction arithmetic with
explicit binary64 stage rounding and binary32 storage for reduced output. It
imports the unchanged alpha numeric oracle, without importing the engine or a
scientific package. Padding includes nonfinite/out-of-range values deliberately;
it is discarded and has no source identity or pixel influence.

[Native tests](../tests/coverage_source_tests.cpp) pass all 20 exact independent
fingerprints and native/mip outputs, **14,287 exhaustive bounded ROI/footprint
checks**, admission and unsupported-level rejection, caller-buffer isolation,
canonical zero identity, visible-bit/origin/dimension identity changes,
fresh output ownership, shared immutable copy/lifetime and eight-worker first
fingerprint/render replay. The footprint oracle independently enumerates selected
native cells instead of calling the implementation's mapping. This is selected
engineering evidence, not a production-scale mask/brush or scheduler study.

Full Release suites pass: **80 default / 44 core / 45 LittleCMS /
81 LittleCMS + Python**. Complete source/native/log bindings are in
`build-msvc-release/research/coverage-source-full-v1/verification.json`, SHA256
`947cfa0d85e34cca5b05de4ce684ddc82ffc4b0bea90813f1f5b1a4c840a3db8`.
The frozen source contract SHA256 is `7f552e5d79b5a3d3e8303da931c8f76bcc93455f2d6303283338f6edbdc221b6`.

Fresh default and LittleCMS installed C++ consumers compile using installed
CoverageSource/CoverageOps headers and the existing exported library target.
Copied/installed/build DLL hashes agree. Both source and all 616 alpha numeric
cases pass in each installed consumer. Receipt:
`build-msvc-release/research/coverage-source-installed-v1/verification.json`,
SHA256 `0cb7fb7773c9c4ab53d168b96abd9a1029b7d0f2e2a6e896116268d379d7f7be`.

The older alpha contract, helper source/header, tests and fixtures are byte
unchanged. Exact pre-source CMake text and all four prior accepted alpha DLLs are
preserved in `build-msvc-release/research/coverage-source-prior-foundation-v1`.
Existing immutable reports are retained; their old native/CMake bindings describe
that historical checkpoint, while the new reports bind the current implementation.

Reproduction on the configured Windows environment:

```bat
build-msvc-release\research\run-with-windows-env.cmd C:\Users\tylle\anaconda3\python.exe tests\reference\coverage_source_oracle.py --check
build-msvc-release\build-pinned-camera.cmd
build-msvc-release\research\run-with-windows-env.cmd build-msvc-release\research\run-ctest-tool.cmd --test-dir build-msvc-release -R Coverage --output-on-failure
```

Full and installed orchestration helpers and logs remain in ignored local research
storage. Use fresh output/install/build versions for reruns; successful receipts
are immutable. Tests, fixtures and research scripts are not installed.

## Remaining scope

This accepts the immutable scalar source gate. Typed reusable mask nodes, named
manifest mask-edge execution, graph signatures/ROI/cache/jobs/history/Python and
saved assets remain next. Raster/brush painting, parametric/range masks, algebra,
feathering, local edits and RGBA layer graph integration remain open. The original
coverage/alpha and reusable mask graph rows are not checked off by this source
milestone. No RGB schema/default path or Python registry change, UI expansion,
commit or push was made. Checklist remains **145 checked / 135 open / 280 total**.

Routine test workflow and the observed source-scale improvement hypothesis are
recorded under PERF-028 and PERF-038 in [the performance log](PERFORMANCE_ISSUES.md).
No dedicated profile, scale benchmark or optimization was performed; performance
work remains deferred until the functional build is complete.

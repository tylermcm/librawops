# Range-mask native primitive/node evidence v1

2026-10-04. [RangeMask.hpp](../RangeMask.hpp) and
[RangeMask.cpp](../RangeMask.cpp) implement the
[frozen range contract](RANGE_MASK_CONTRACT_V1.md). This checkpoint accepts the
direct native API and nodes; saved graph registration and Python graph/session
integration are still required. The original range implementation row is open.

Three validators, three pure scalar evaluators and copied immutable
CoverageLuminanceRangeNode/CoverageColorRangeNode/CoverageDepthRangeNode classes
are exported. RGB-guided constructors take the mask handle, RGB image handle,
explicit RGB native bounds and settings; RGB Node does not expose a generic
native extent, so the host supplies it and the constructor requires exact matching
mask bounds. The depth constructor takes two typed scalar handles and settings.
The future graph builder supplies its already validated runtime RGB extent.

All 12,236 independent frozen selections and all 12,236 actual native input-mask
products match bit for bit in each of the default/core/LCMS/LCMS+Python Release
builds. Thirty frames match native Final/Preview and direct mip-1/2 Preview,
1/2/8 tile partitions and every nonempty rectangular ROI. Shifted/extreme origins,
odd/singleton dimensions, native Final requests on both branches and direct source
block mapping pass. RGB/coverage actual wrong bounds, short storage, nonfinite,
invalid descriptor/coverage samples reject even with a zero mask. Invalid controls,
unsupported levels, empty/overflow extents, input extent mismatch, copied settings
and pre-upstream materialized-capacity guards pass. Native test sources deliberately
retain input signed zero to prove endpoint bit copying rather than source
canonicalization. These tests do not yet prove saved mixed-branch footprints.

All four builds use /fp:strict for RangeMask.cpp. Seven native test executables per
configuration pass (28 executions): range, geometric, brush, mask graph, delivery,
source and alpha. Immutable receipt
`build-msvc-release/research/range-mask-native-foundation-v1/verification.json`
SHA256 312e816c20b0a3056bbba4f9277684f69a62add4a3db0bc0b274aec14d4c966b binds sources, binaries and logs. Prior accepted 56-file archive
remains exact; prior engine/graph/Python/scheduler/numeric sources and fixtures
remain unchanged. CMake adds the module, strict FP option, exported header and
native test. This is focused native verification, not new full-suite, installed,
Python range execution, clean-machine, camera-quality or release acceptance.

Next register strict format-4/schema1/process2 saved types and mixed scalar/RGB
dependency planning; replay frozen scalar graph/Python/cache/history/jobs and
fresh installed consumers, then run complete Release suites. Preserve disabled
guide typing while excluding its sample footprint. PERF-044 logs unmeasured
guide validation/storage and test-fixture compilation candidates; performance
fixes are deferred. Fixed original checklist remains147 checked/133 open/280 total.
No UI, dedicated profiling, agent commit or push.

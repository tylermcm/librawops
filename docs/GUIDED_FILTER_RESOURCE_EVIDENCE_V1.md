# Working-Y guided-filter resource evidence v1

The accepted finite-check optimization preserves the frozen ordered arithmetic,
support, coefficient layout and buffer policy. This document records selected
Windows resource studies of that implementation. It supplements the
[mathematical contract](GUIDED_FILTER_CONTRACT_V1.md) and
[finite-check proof](GUIDED_FILTER_FINITE_CHECK_OPTIMIZATION_V1.md).

Reproducible opt-in sources and commands are in the
[qualification tools](../bench/guided_filter_qualification/README.md).
The original successful reports remain local and immutable; public benchmark
sources are byte-identical to the sources that generated those reports.

## Scope and bindings

MSVC19.40 Release, i9-14900K, strict floating-point consumers, actual installed
optimized DLL. Both scene-linear working spaces use deterministic signed/headroom
owned RGB input. No other builds or tests ran concurrently with measured studies.
Hardware power/load conditions were not locked. These are selected baselines;
repeated45MP or cross-machine regression claims require further measurements.

Combined accepted resource record:
`build-msvc-release/research/guided-filter-resource-accepted-v1.json`, SHA256
`97d63bc861a254b49b516c15f77520e31621a4bb573fedf885eb7076480679ae`.
It binds original frame/thread/45MP reports, their source/native artifacts, full
output files or SHA pairs, and the published-tool verification. Native library
code/DLL and the prior full76/40/41/77, installed, FP and930-camera acceptance
remain unchanged during these resource studies.

## Logical accounting

LetO be the requested output pixel count, N its actual clipped2r input support,
and A its clippedr coefficient-center count. At positive radius the simultaneous
logical node payload is `12*N + 56*A + 12*O` bytes: returned RGB input halo,
seven-double coefficients, and output RGB. Radius0 validates the finite input and
returns its exact bits, with no coefficient table; its input and returned copy
give a `24*O` logical upper estimate. A direct raster source returns `12*O`.

The assembled renderer retains another `12*W*H` bytes. A streaming renderer
does not retain the full output, but callbacks can keep their own copies.
Source ownership, vector capacity, allocator/runtime overhead, upstream scratch,
cache, outstanding jobs and history are separate accounting categories.
At mostK simultaneous calls give a conservative filter estimate ofK times the
largest per-call payload, plus separately retained outputs. This estimate does
not establish an allocator or whole-process ceiling. GPU execution is absent;
future GPU allocation/accounting requires its own selected backend contract.

## Odd full-frame delivery

1025×769 input; radius0/default3/max8, including two radius8 epsilon endpoints;
native Final and mip1/2 Preview. Direct full helper/node, assembled256/64 and
streamed256 delivery pass96 cases,288 measured exact repeats,24 helper/node
comparisons,2,904 independent support checks and48 cancellation checks.
One warmup precedes three measured repeats. The published CLI repeats this
complete study and preserves all24 original full-output file hashes.

Selected native radius8 tight-epsilon medians across both spaces:

| Delivery | Wall ms | Maximum logical node bytes | Renderer full-output bytes | Total coefficient centers |
|---|---:|---:|---:|---:|
| Direct |1514.264|63,058,000|0|788,225|
| Assembled256 |1563.778|5,924,864|9,458,700|876,420|
| Assembled64 |1906.809|518,144|9,458,700|1,215,396|
| Streamed256 |1566.536|5,924,864|0|876,420|

Smaller tiles reduce transient logical memory while repeating more coefficient
and source-halo work. Stream wall includes separately reported validation time;
assembled validation is outside timing. Process lifetime peaks90,230,784 working
set/85,274,624 commit bytes include references and earlier cases; they are not
per-mode memory attribution. Pre-cancellation performs no source work; cancellation
after the first callback stops before the next tile. No inside-node preemption.

## Fixed-work thread scaling

Eight128-square corner/interior requests on one immutable source/node,1/2/4
`std::async` workers, both spaces and native/mip1/2. One warmup and seven measured
batches yield54 cases,378 measured batches and3,456 exact outputs including
warmup. The published CLI repeats the complete study successfully.

| Native radius |1 worker ms |2 workers ms |4 workers ms | Paired4-worker speedup |
|---|---:|---:|---:|---:|
|3|55.734|28.281|14.500|3.844x|
|8|271.148|135.766|68.317|3.969x|

Times are medians across the two space-specific batch medians. Speedup is the
median of space-specific ratios. Task count is fixed across worker settings.
Timing includes gate release, wakeup, rendering and future return; thread
creation and validation are excluded. This is direct native throughput, without
cache or job-queue scheduling. Process lifetime peaks26,488,832 working set and
23,158,784 commit bytes include all cases and harness references.

## Materialized45MP input

Exactly45,000,000 owned input pixels (7200×6250),540,000,000 source payload bytes.
Twenty fresh processes cover both spaces, native source/radius0/3/8 and radius8
mip2 Preview, with256-pixel streaming or materialized output. Each performs one
measured render. All ten complete stream/image tile-order SHA256 pairs and two
source/radius0 identity pairs match;80 selected corner tiles are byte-exact to
separate direct renders. Reduced output is1800×1563 (2,813,400 pixels).

| Setting | Stream wall seconds | Assembled wall seconds | Stream peak working set MB | Assembled peak working set MB |
|---|---:|---:|---:|---:|
| Source |0.234|0.148|548.043|1087.791|
| Radius0 |0.550|0.465|548.944|1088.784|
| Radius3 |19.303|19.241|552.511|1092.514|
| Radius8 |91.338|91.245|552.940|1092.952|
| Radius8 mip2 |5.793|5.756|552.581|586.359|

Values are medians across two space-specific single runs, not repeated-run
medians. MB means decimal1,000,000 bytes. Source construction/validation takes
about203..208ms and is excluded. Streaming includes checksum/corner validation
(about200..203ms native and13ms at mip2), separately recorded with a subtraction
estimate. Assembled checksum occurs outside rendering time. Fresh process peaks
include the owned source, selected references, allocator and runtime.
Native materialized output adds540,000,000 logical bytes; mip2 adds33,760,800.
Observed native peak commit is about549.6MB streamed and1090.7MB assembled at
radius8. No attribution to individual allocations is claimed.

Radius3/8 native derived coefficient work is47,110,992/50,736,832 centers, compared
with45,000,000 unique input/output pixels; returned source halos total
591,244,416/681,812,736 derived bytes. PERF-036 records repeated halos and ordered
window work as remaining investigation points. The radius8 result establishes
substantial serial CPU cost at full resolution.

## Remaining qualification

The broad original build-plan rows remain open. These studies do not establish
whole-engine allocation tracking, allocator/backend failure recovery, combined
cache/job/history/concurrent memory ceilings, portable performance, controlled
power/load targets, a representative camera/profile corpus, ICC/codec/file export
throughput, or production/release readiness. Full-output SHA agreement is distinct
from an independent45MP mathematical oracle. Numeric accuracy and artifact limits
remain those of the frozen contract and prior independent kernel evidence.

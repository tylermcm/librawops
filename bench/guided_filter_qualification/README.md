# Guided-filter resource qualification

These opt-in Windows C++20 tools exercise an installed RawEngine through public
headers and its exported CMake target. They are separate from the library build,
installation and diagnostic viewer. They use Windows process counters; the 45 MP
harness uses the Windows SHA-256 provider to compare complete output digests.
Neither adds a library dependency.

Build from an MSVC developer prompt after installing the engine to a fresh prefix:

```powershell
cmake -S bench/guided_filter_qualification -B build-guided-qualification -G Ninja -DCMAKE_BUILD_TYPE=Release -DRAWENGINE_INSTALL_PREFIX=C:/absolute/engine-install
cmake --build build-guided-qualification
python bench/guided_filter_qualification/run.py --build-dir build-guided-qualification --engine-dir C:/absolute/engine-install --output-dir C:/absolute/evidence/frame-v1 --study frame
```

Use `--study threads` or `--study 45mp` with a different fresh output directory for
each run. The 45 MP study requires at least 2 GiB of available physical memory and
runs 20 separate processes; native radius-8 cases can take about 90 seconds each
on the recorded i9-14900K. Successful output directories are immutable. The runner
checks actual DLL loading, binds source/executable/DLL hashes before and after the
study, rejects duplicate JSON keys, and records native-report/output hashes.

## Studies

All inputs are owned signed/headroom float32 rasters with deterministic samples
`float(int((19*x + 37*y + 83*c) % 257) - 64) / 128`, in both LinearProPhoto/D50 and
LinearRec.2020/D65. Samples include negative values and values above one. Radius
is measured at the requested input level, with upstream reduction first.

- **frame:** odd 1025×769 input, radius0 with epsilon2^-24, radius3 with epsilon2^-12,
  and radius8 with epsilon2^-24/65536. Native Final and mip1/2 Preview compare the
  full helper against the node, then direct, assembled256/64 and streamed256
  delivery. One warmup plus three measured repeats yields96 cases,288 exact
  output comparisons,24 helper/node comparisons,2,904 independently expanded
  support checks and48 cooperative cancellation checks. Complete output files
  are preserved. Source crop/reduction and callback comparison clocks are separate.
- **threads:** one immutable 1025×769 source/node, radius0/3/8 and native/mip1/2;
  eight fixed128-square corner/interior requests processed by1/2/4 workers. One
  warmup plus seven measured batches yields54 cases,378 measured batches and
  3,456 byte-exact outputs including warmup. Timing starts at gate release after
  thread creation and includes wakeup, task assignment, rendering and future
  return; validation occurs afterward. This measures direct native calls using
  `std::async`, without a cache or the library job queue.
- **45mp:** 7200×6250 input (exactly45,000,000 pixels), native source/radius0/3/8
  and radius8 at mip2 Preview, with256-pixel streaming and materialized output.
  Each case performs one measured render in a fresh process. Ten complete
  stream/image tile-order SHA-256 pairs and two source/radius0 identity pairs
  must match;80 selected corner tiles compare byte for byte against separate
  direct renders. The SHA includes each tile's four uint32 bounds followed by
  its row RGB bytes. This is complete-output digest agreement, with selected
  exact comparisons, rather than an independent45MP mathematical oracle.

## Interpreting resources and timings

For a guided-filter output tile, independently clipped2r input support contains
N pixels, clippedr coefficient centers containA, and output containsO. Logical
simultaneous node payload is `12*N + 56*A + 12*O` bytes; radius0 allocates no
coefficient table. A plain source call has only its returned RGB payload.
The renderer additionally retains `12*W*H` bytes for assembled output; streaming
does not retain a full image. Client callbacks can retain their own copies.

These formulas exclude owned source pixels, vector capacity, allocator overhead,
upstream scratch, cache, jobs, history and concurrent calls. The thread study
reports a conservative maximum per-call payload multiplied by configured workers,
plus separately retained batch outputs. This is a logical upper estimate, not
an allocator trace or the observed simultaneous byte count.

The frame/thread process peaks include previous cases and harness reference
outputs. The45MP peaks are fresh per case but include the540MB owned source,
selected reference tiles, runtime and allocator. Peak working set is physical
residency; peak commit counts committed private memory. They are distinct from
node payload and from a production memory ceiling.

Frame streaming wall time includes separately reported callback validation;
assembled comparison occurs after timing. The45MP stream includes checksum and
corner validation, reported separately with a subtraction estimate; image
checksum is outside rendering time. Source construction is separately reported
and excluded from render wall. The45MP study has no warmup or repeated median.
Run studies serially, separately from builds/tests, and record hardware, load and
power conditions before treating results as performance regression gates.

Cancellation is checked between renderer tiles. The frame study proves zero work
on pre-cancellation and stopping after the first delivered tile; it does not
provide cancellation inside a filter call. These studies do not qualify camera
decoding, RAW profiles, ICC, codecs, file export, allocator failures, cross-platform
behavior, representative image quality, or release readiness.

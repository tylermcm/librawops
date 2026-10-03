# Remaining color-control qualification gates

This records the two open checklist items immediately after the completed tonal
color-balance foundation. Continue from the authoritative
[plan and handoff](plan/LIBRAWOPS_PLAN.md); this is evidence scope, not a second handoff.
User instruction remains authoritative: the UI is a local test harness and will
not ship with the library. Product UI is excluded from these library gates.

**Objective qualification — user clarified 2026-10-02.** Personal aesthetic
preferences and preferred-edit examples are not requirements or blockers.
Choose and document engineering contracts from the plan, then test independent
mathematical truth, declared color behavior, signed/headroom handling, numerical
continuity, artifacts, replay and performance. Photographic references evaluate
those declared properties; they do not bind the engine to a person's taste.
Exact editor matching is a separate, explicitly selected compatibility goal.

| Requested checkbox | Current evidence | What remains to close it |
| --- | --- | --- |
| Qualify broader color balance/grading and production photographic color behavior | Three-band tonal balance plus original signed lift/gain/gamma grading implemented; independent Fraction/Decimal/native/Python truth, replay/cache/history/mip integration, grading96 camera references and eight inspected boards | representative color/skin/shadow/highlight/gamut comparisons using recorded camera/profile/display settings and explicit quality criteria |
| Qualify larger-table/file/UI LUT and channel mixer production control behavior | Channel mixer and legacy LUTs retain their contracts; new4096-sample/33-cube types and bounded ASCII Cube import have independent numeric/integration/file/resource/ownership/camera evidence,96 new references/eight boards | Extend beyond the declared bounded subset only for concrete requirements; qualify declared original controls on representative profile-aware inputs and complete stage/full-frame/concurrency/allocator measurements. UI remains optional test-only and outside library installation |

The existing `_DSC1793` ISO20000 background/skin boards deliberately use a
diagnostic camera/display chain. Exact matches to the defined equations verify
implementation, not intended photographic appearance or a qualified profile.
The corpus declaration `tests/reference/raw/camera_corpus_plan_v1.json` still has
no formally admitted assets and targets at least three camera models, low/high
ISO, daylight/tungsten and varied content. Eleven local Nikon Z7 II files are
valuable development inputs but do not complete that declared coverage.

Current evidence can be reused rather than regenerated speculatively:

- [Tonal color balance](COLOR_BALANCE_CONTRACT_V1.md): equations, bits, versions,
  requested-level and native/camera verification.
- [Channel mixer](CHANNEL_MIXER_V1.md): immutable bounded matrices, exact unit-row
  bits, numeric/graph/camera checks, including repeated-row monochrome matrices.
- [1D LUT](LUT1D_V1.md) and [3D LUT](LUT3D_CONTRACT_V1.md): table limits, range,
  axis order, interpolation/extrapolation, bounded resource and replay contracts.
- [Local camera study](research/CAMERA_RAW_LOCAL_STUDY.md): capture evidence,
  diagnostic calibration limitations and repeatable inspection/measurement paths.
- [Grayscale](GRAYSCALE_V1.md): parameter-free working-Y RGB monochrome, separate
  from selecting a photographic black-and-white look.

The four authorized bounded steps are complete: original grading contract,
version-preserving larger LUT/file policy, native/Python/graph implementation,
and objective verification/install/camera/performance evidence. See
[grading](GRADING_V1.md) and [extended LUT/file policy](LUT_FILE_V1.md).
This completes the new foundations, while the two broad production checkboxes
still require the representative/profile-aware evidence stated above. No
personal preferred edit is required. Adobe reference captures are needed only
for an Adobe compatibility claim. Native grading pow is platform math, with
tolerances rather than cross-toolchain exactness. Cube import explicitly assumes
the caller-declared scene-linear space, one ASCII table, 4 MiB/250-byte bounds,
shared1D domain and trilinear3D; it does not infer log/display transfer or accept
every Cube dialect. Existing processing2 operation types/limits/defaults remain
fixed. PERF-023 records larger-table/import costs; prior PERF-017 evidence stays
historical. The [bounded clarity foundation](CLARITY_CONTRACT_V1.md) is now implemented
and independently verified,including true-boundary halos and native/mip replay.
The [separate bounded texture foundation](TEXTURE_CONTRACT_V1.md) is now implemented: exported TextureNode/strict saved type,staged per-pass true-border binomial native-Y band. Independent native/Python/math/tile/replay/RAW/mip/cache/history/analysis gates,144 exact camera references,twelve inspected boards,installed C++/Python smoke and full58/31/32 pass. PERF-025 records measured bounded API costs;photographic/profile/corpus/full-frame/concurrency/allocator qualification remains open. Next freeze a separate original dehaze contract before coding. UI remains optional,unpackaged and testing-only.

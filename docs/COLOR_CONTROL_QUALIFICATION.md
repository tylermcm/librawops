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
| Qualify broader color balance/grading and production photographic color behavior | Original three-band linear RGB offsets, fixed native-Y weights and optional Y projection implemented; rational/native/Python truth, graph integration, 96 camera references and eight inspected boards | Define the additional grading controls and their intended domain/behavior; independent truth and versioned replay for those controls; representative color/skin/shadow/highlight/gamut comparisons using recorded camera/profile/display settings and explicit quality criteria |
| Qualify larger-table/file/UI LUT and channel mixer production control behavior | Channel mixer, 1D LUT 2..256 and 3D LUT 2..17 have bounded numeric/integration/camera evidence; errors, extrapolation, exact identity and cache behavior tested | Select required larger table sizes and file format/encoding/domain/axis-order policy; preserve processing2 replay if limits or interpolation change; implement and verify parsing, resource bounds and malformed assets; qualify intended looks/channel controls on representative profile-aware inputs. UI remains optional test-only and outside library installation |

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

To make the next implementation concrete, select grading controls, maximum
1D/3D sizes and LUT file policy as documented engineering decisions under the
plan. Establish objective original-behavior acceptance criteria before coding;
personal preferred edits are optional examples, not necessary input. Record
source/profile identity, ownership/use declarations and actual render settings
for representative photographic evidence. Adobe references are required only
for an Adobe compatibility claim; native qualification can use independent
original targets. Continue bounded implementation while broader corpus/profile
evidence is collected. Do not mark
these broad checkboxes complete from bounded equation tests alone, or silently
change processing2 limits/defaults. Larger-table validation/copy/signature costs
remain PERF-017; no optimization is justified without comparable measurements.

# Extended LUT and bounded Cube text import v1

Status: frozen engine/file contract before implementation.

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
Syntax/layout provenance: Adobe's 2013 [Cube LUT Specification1.0, sections5–7]
(https://kono.phpage.fr/images/a/a1/Adobe-cube-lut-specification-1.0.pdf), authored
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

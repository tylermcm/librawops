# Projective and perspective transform contract draft v1

Status: **scoping draft; neither frozen nor implemented**. This continues the
Phase 4 geometry item in the [build plan](plan/LIBRAWOPS_PLAN.md) after the
verified fixed-canvas RotateNode. Existing geometry remains unchanged.

## Proposed control and coordinate policy

Supply an explicit positive uint32 output width/height and an immutable 3x3
`source_from_output` inverse homography. Coordinates are local normalized
pixel centers. Output endpoint centers map to -1 and +1 on each non-singleton
axis; a singleton axis maps to zero. For native output integer x,y:

```text
u = 0 if output_width==1 else (2*double(x)-double(output_width-1))/double(output_width-1)
v = 0 if output_height==1 else (2*double(y)-double(output_height-1))/double(output_height-1)
d = (m20*u+m21*v)+1
nx = ((m00*u+m01*v)+m02)/d
ny = ((m10*u+m11*v)+m12)/d
sx = ((nx+1)*0.5)*double(input_width-1)
sy = ((ny+1)*0.5)*double(input_height-1)
```

Pin order in strict binary64, no contraction/reassociation. Identity matrix
with equal input/output extents maps integer coordinates directly to preserve
native bits; other transforms use the normalized map. This gives endpoint-
aligned identity-matrix resampling when sizes differ, a declared convention
separate from ResizeNode's extent-ratio pixel-center mapping. Input origin is
added after local tap selection. Output origin is zero.

Proposed matrix admission: nine finite numeric coefficients in [-16,16], m22
exactly one, a nonsingular matrix under a pinned determinant computation with
absolute determinant at least 2^-20. Reject a collapsed mapping. Require the
ordered denominator at all four normalized corners (-1,+1)^2 to be at least
0.25. Because each multiplication/addition is monotone over the clamped output
coordinate range, that admits no denominator pole in the output canvas. Check
the computed denominator again when sampling. This is a deliberately bounded
manual homography foundation; negative determinants permit reflection.
Forward homography fitting/inversion, landmark editing and automatic perspective
controls are separate consumers/contracts.

Retain scene-linear float32 ProPhoto/D50 or Rec.2020/D65 RGB, signed/headroom,
true-source replicated borders and the frozen ordered bilinear active-tap/bit/
finite/overflow rules from [rotation](ROTATE_CONTRACT_V1.md). No alpha, fill,
color conversion, hidden crop or automatic canvas expansion. Border replication
can streak and bilinear minification can alias. Selected full-canvas coordinates
need independent high-precision characterization before setting an acceptance
tolerance. Matrix admission is an API policy, not a condition-number guarantee.

## Proposed bounded rendering and preview

Plan exact conservative source support by scanning actual mapped active taps,
using the same floating routine as rendering. Compose native source rectangles
through upstream geometry/RAW halos. Planning does not allocate image buffers.

A fixed 128-square native block can span most of a huge source image under
strong scale/perspective. The rotation block cap therefore cannot simply be
reused. Proposed renderer processes at most128-square native output groups in
global row order. For each native row, select at most128 output samples, scan
their source bbox, then recursively partition oversized spans into left/right
halves until each source rectangle is at most257-square. Visit left before
right, preserving row order. At one sample the active bilinear support is at
most2-square, so splitting terminates within seven splits from128 samples.
Fetch/validate that bounded rectangle, sample once-cast
float32 RGB, and release it before the next span.

Mip1/2 Preview averages native transformed float32 values in global y-major/
x-major order, directly into bounded per-output-block double sums/counts. Align
groups to whole preview cells; never split one output cell across groups.
Each cell's contributions must retain identical order when row spans split.
Use actual sample counts on final partial cells. Native Final and existing
geometry admission of upstream native/requested reduced support are proposed;
other levels reject. Avoid a full-frame native temporary or unbounded native
input bbox. Caller output and upstream/cache budgets remain separate.

Resource arithmetic, empty/native/uint32 endpoints, span termination, partial
cells, exact partition/ROI/mip/source planning and identity all remain to prove.
PERF-031 records unmeasured adaptive row scanning, worst-case tiny fetches,
bounding amplification, repeated validation, reductions, graph/cache/copy and
allocator costs. Faster rectangle/corner planning needs a numerical proof;
performance optimization is a later measured step.

## Open gates

1. Freeze matrix/determinant/denominator/coordinate conventions and precision
   after independent rational/high-precision tests, including small/large/thin
   extents, reflections, translations, affine scale/shear, perspective and
   rejected near-singular/pole/nonfinite/invalid inputs.
2. Prove adaptive row support/resource termination and exact native-before-mip,
   output-cell ordering, arbitrary viewport partitions and source-origin mapping.
3. Inspect fixed synthetic and camera artifacts with controls/display fixed
   before rendering; declare border/crop/minification effects objectively.
4. Freeze exported settings/node, saved type/schema/processing and strict disabled
   behavior, bounds/levels/cache/history/jobs/analysis/multi-source ownership.
5. Archive the normative contract and existing rotation source/native/full-log/
   consumer evidence before native code. Implement only after these gates, then
   run native/independent integration/camera/performance/full builds/install.

No checklist closed by this draft. UI remains an unpackaged testing harness,
and camera NR stays deferred until explicitly resumed.

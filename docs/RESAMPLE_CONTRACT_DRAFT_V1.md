# Sharper resampling scope draft v1

Status: **research/scoping only; no frozen contract or native change**.
This follows the verified projective foundation under Phase4's sharper
reconstruction/resampling checkbox. Existing ResizeNode nearest/bilinear/area,
RotateNode and ProjectiveNode keep their current versions and arithmetic.

## Candidate and provenance

Compare independently implemented separable four-tap cubic reconstruction:
Keys/Catmull-Rom (a=-0.5, equivalently B=0,C=0.5) and Mitchell-Netravali
(B=C=1/3), with current bilinear and area references. Keys derives a compact
interpolating cubic with four samples per axis. This scope retains replicated
true borders instead of adopting the paper's extrapolated boundary conditions;
the paper's boundary convergence claims therefore do not apply. Source:
[Keys, Cubic Convolution Interpolation for Digital Image Processing, 1981](https://ncorr.com/download/publications/keysbicubic.pdf).

Mitchell and Netravali describe reconstruction tradeoffs among ringing,
blur, anisotropy and aliasing, including a non-interpolating cubic family.
Their subjective preferred parameters do not establish a universal engine
default. Source: [Reconstruction Filters in Computer Graphics, 1988](https://www.cs.utexas.edu/~fussell/courses/cs384g-spring2016/lectures/mitchell/Mitchell.pdf).
Use mathematical descriptions and original implementation; copy no external
source code or paper artifacts into the shipped library, add no dependency.

Candidate measurements fix synthetic controls before evaluation: smooth
bandlimited functions, signed/headroom ramps, step/impulse/grid/checker patterns
and frequencies above the resized Nyquist limit; then fixed historical camera
crops with fixed display. Measure interior analytic error, step lobes/overshoot,
high-frequency alias residual, DC/identity/signed-zero behavior, tap support and
prototype work. Sharpness alone does not prove antialiasing or NR benefit.

## Policies to investigate before freezing

Keep existing extent-ratio pixel-center coordinates rather than projective's
endpoint normalization. A new explicit-canvas reconstruction node/type may
isolate the new processing contract from legacy ResizeNode replay. Scene-linear
ProPhoto/D50 or Rec.2020/D65 RGB only; signed/headroom, no color clip. Negative
lobes can produce real overshoot beyond finite float32; admission/arithmetic
must reject finite-range overflow explicitly. Native identity and component
constants should retain bits, with strict whole-fetched-rectangle validation.

Four fixed taps are a candidate for enlargement, not a complete minifier.
Investigate scale-widened cubic kernels with per-axis support
`2*max(1,input_size/output_size)`, normalized discrete weights and true-source
replication. Compare against current exact-overlap area minification. A bounded
manual foundation could admit at most4x native shrink per axis and use32-square
native blocks, bounding per-axis taps/source work without hiding a larger
unsupported ratio. This is a proposed gate, not a settled admission rule.
General/Jacobian-aware warped reconstruction and arbitrary strong shrink remain
separate research; do not silently change the frozen rotation/projective nodes.

Pin kernel coefficient evaluation, normalization, ordered centered-difference
accumulation, horizontal/vertical rounding, identity and overflow rules only
after independent rational/high-precision/extreme evidence. Shared actual taps
must drive support/render; no ideal corner substitute. Retain native-before-
mip1/2 direct output-cell means, partial cells, nonzero input origins, empty
requests and uint32 final-pixel/exclusive-end behavior.

Resource arithmetic, independent ROI/mip/halo/partition proofs, immutable
settings, saved type/schema/processing, strict disabled behavior, graph/cache/
history/jobs/analysis/RAW/multi-source and installed consumers remain open.
PERF-032 should tag scale-dependent taps, repeated coefficient/planning work,
source fetch amplification and validation/copy/cache/allocator costs before code.
No checkbox closes from this draft; production photographic/corpus/Adobe and
cross-runtime qualification remain open. UI stays an unpackaged testing harness;
camera NR remains explicitly deferred.

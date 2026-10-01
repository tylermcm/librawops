# Standalone Menon C++ research port

This executable is isolated from the engine build, public API and install rules. It is a research parity prototype of Colour Science's Menon 2007 base/refined reconstruction at commit `7bff324983fb77b41444fda3bf922e354d386d1c`. It does not admit an engine backend or override the previous policy-v1 rejections. The existing shipment/patent/jurisdiction review remains open.

Contract fixed before C++ implementation:

- Input: owned, finite, already normalized float32 Bayer plane, local effective RGGB/BGGR/GRBG/GBRG pattern. The caller performs sensor-level normalization and active-area/phase rebasing. At most 1,000,000 samples in this bounded prototype.
- Processing: completed float64 stages, horizontal tie preference, original posterior direction map, optional original three-stage refinement, no sign guard or clipping. Output is interleaved float32 RGB; observed channels are copied exactly.
- Boundaries: whole-sample mirror for directional/color convolution and forward gradient extension; zero extension of classifier accumulation, matching the pinned code. Singleton axes use native-v1 valid-neighbor bilinear fallback because upstream squeezes them away. This is an explicit research adaptation.
- Requests: compute complete active planes, then extract the requested ROI. Internal ROI boundaries never become image boundaries. This proves crop equivalence only; it is not native tile/halo/cache integration or bounded tile memory.
- Reference screen: compare with pinned upstream output, allowing two float32 ULPs plus `64 * max(1,max_abs_input) * epsilon64` absolute slack per reconstructed channel, for independently ordered float64 convolution/refinement. Observed samples must be exact. Report bit-exact counts and maximum error even when the screen passes.
- Validate all 160 photo-remosaicing reference planes in both modes, signed/headroom inputs across all effective patterns, border constants, thin fallback, ROI equivalence and malformed input. A passing prototype is separate from photographic quality and product admission.

Build independently with `cmake -S research/menon -B build/research/menon-cpp`, then `cmake --build build/research/menon-cpp --config Release`.

The CLI takes `input.f32 width height pattern base|refined output.f32`, optionally followed by `x y width height`. Pattern is 0=RGGB, 1=BGGR, 2=GRBG, 3=GBRG. Files contain little-endian IEEE float32 without headers. The executable rejects mismatched file sizes, nonfinite values, invalid controls and out-of-bounds ROIs.

This adaptation retains Colour Developers' BSD-3-Clause notice in [LICENSE.colour-demosaicing](LICENSE.colour-demosaicing). It links only the C++ standard library; NumPy/SciPy/Colour remain optional research-oracle dependencies outside the executable.

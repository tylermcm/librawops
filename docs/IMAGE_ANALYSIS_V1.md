# Tiled RGB histogram foundation, version 1

This read-only API analyzes the selected output of an existing graph. It is a
foundation for future histogram displays and editing controls. It does not
estimate camera noise, change an image, or add an edit operation. Custom NR
research can remain deferred while this engine work proceeds independently.

## API and domain

Include `ImageAnalysis.hpp` and call `histogram_rgb(graph, request, options,
cancellation)`. Overloads accept `ImageGraph`, `ExecutableEditGraph`, or a
`Node` with explicit native output bounds. The result carries the full output
`ImageDescriptor`, request, range, pixel count and three R/G/B channel records.
Coordinates, mip and quality follow the existing renderer's conventions.

Python `RawSession`, `RasterSession` and `RasterGraphSession` expose:

```python
result = session.histogram_manifest(
    saved_manifest, {"tile_size": 256}, bins=256, lower=0.0, upper=1.0
)
print(result["descriptor"], result["channels"][0]["counts"])
```

The manifest selects the stage. For example, a camera source tap measures
camera-linear samples; a calibrated tap measures its declared scene-linear
working space; a display graph measures its encoded output. These are different
histograms. A default recipe can include tone/clipping, so select the intended
tap explicitly rather than interpreting every graph as linear data. Source-only
manifests have no operations and set `output` to the selected source ID; validate
other saved taps using the existing manifest rules.

The optional request dict accepts only existing viewport, tile, mip and quality
fields. Histogram settings are separate keyword-only arguments. Python returns
an owned dict with `version`, `bounds`, `mip`, `quality`, `bins`, `lower`, `upper`,
`pixel_count`, `descriptor` and `channels`. The descriptor includes format,
domain, primaries, white point, transfer, reference, alpha and profile SHA-256.
Each channel has `counts`, `underflow`, `overflow`, `nonfinite`, `minimum` and
`maximum`. A channel with no finite values has absent C++ extrema / Python
`None`. Returned lists/dicts cannot modify the graph or future results.

## Numerical contract

- 1..65536 equally spaced bins; default 256 over [0,1]. Bounds and their positive
  difference must be finite float64 values. The viewport must be nonempty.
- Samples strictly below `lower` or above `upper` use separate uint64 counters.
  Bins include the lower edge and exclude the next edge, except that the final
  bin includes exactly `upper`. Native classification uses float64 normalized
  position and truncation with a final-bin bound; no reciprocal that can
  overflow for a tiny range. Samples remain the renderer's float32 values.
- NaN and both infinities count as `nonfinite`; they do not affect extrema.
  Extrema include all finite samples, including signed values and headroom
  outside the selected range. There is no implicit clamp or transfer conversion.
- Each channel's bins plus underflow, overflow and nonfinite sum to
  `pixel_count = viewport.width * viewport.height`. Counts use uint64.
- Integer counts/extrema are independent of analysis partitioning for identical
  rendered samples. Upstream operations are still responsible for render tile
  consistency; this API cannot repair a partition-dependent custom node.

## Memory, cache and cancellation

The analyzer retains one renderer tile and three counter vectors: 6 KiB of
counters at 256 bins, at most 1.5 MiB at 65536 bins. A default 256-square output
tile contains 768 KiB of RGB. Source ownership, upstream halo/intermediates,
existing graph cache and Python result objects are additional memory. Callers
control tile size; a frame-sized tile still allocates a frame-sized tile.

Analysis shares the graph's bounded tile cache and obeys saved-source identity,
working-space and render-level validation. It does not save results to manifests,
create history revisions, start scheduler jobs or change processing defaults.
Custom-node tile bounds, RGB storage length and descriptor must agree with the
requested output. Malformed tiles throw before their samples are accessed.

C++ cancellation is checked before rendering, at tile callbacks and after the
last tile. Cancellation throws `RenderCancelled`; no partial result escapes.
The sampler does not interrupt an upstream node inside a render call. Python
releases the GIL for graph construction and native rendering/counting and is
synchronous; it does not yet expose cancellable histogram jobs or history methods.
An already-started synchronous analysis owns its graph snapshot, like an
already-started synchronous render; closing the session rejects new analyses.

## Validation and limits

`tests/image_analysis_tests.cpp` checks explicit bin-edge truth, signed/headroom
counts, NaN/infinities, absent extrema, single/max-bin and tiny/wide ranges,
nonzero origin, clipped tile coverage and bounded tile requests, invalid ranges,
malformed custom tiles, unsupported levels and cancellation including the final
tile. `tests/python_image_analysis_tests.py` has five cases using an independent
interior-edge/binary-search oracle against rendered float32 samples. It covers
RAW bilinear/Menon source and calibrated taps, odd reduced extents, geometry,
both working spaces, multi-source graphs, display descriptor semantics,
partition parity, cache reuse, copied ownership and closed-session validation.

This checkpoint supplies histograms, finite extrema and range counts. Local
mean/population variance and bounded convolution are now available in
[spatial foundations](SPATIAL_FOUNDATIONS_V1.md). Richer moments, percentiles,
luma/chroma definitions, automatic exposure and adaptive NR remain separate work. No Adobe comparison,
camera-noise calibration, GPU backend or production performance target is claimed.

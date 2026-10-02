#pragma once

#include "RawEngine.hpp"

namespace rawengine {

struct HistogramOptions {
    std::uint32_t bins = 256; // 1..65536, equally spaced in [lower, upper].
    double lower = 0.0;
    double upper = 1.0;
};

struct ChannelHistogram {
    std::vector<std::uint64_t> counts;
    std::uint64_t underflow = 0, overflow = 0, nonfinite = 0;
    std::optional<float> minimum, maximum; // All finite values, including outliers.
};

struct RgbHistogram {
    RenderRequest request;
    HistogramOptions options;
    ImageDescriptor descriptor;
    std::uint64_t pixel_count = 0;
    std::array<ChannelHistogram, 3> channels; // R, G, B in the output domain.
};

// Read-only analysis of exactly the requested graph output. Bins are half-open
// except the last bin includes upper. Signed values and headroom are not clipped.
// NaN and infinities count as nonfinite and do not contribute to extrema.
// Uses one rendered tile plus O(bins) counters, rather than a full ROI buffer.
// Cancellation throws; callers never receive a partially accumulated result.
RAWENGINE_API RgbHistogram histogram_rgb(
    const Node& output, Rect source_bounds, const RenderRequest& request,
    const HistogramOptions& options = {}, const CancellationToken* cancellation = nullptr);
RAWENGINE_API RgbHistogram histogram_rgb(
    const ImageGraph& graph, const RenderRequest& request,
    const HistogramOptions& options = {}, const CancellationToken* cancellation = nullptr);
RAWENGINE_API RgbHistogram histogram_rgb(
    const ExecutableEditGraph& graph, const RenderRequest& request,
    const HistogramOptions& options = {}, const CancellationToken* cancellation = nullptr);

} // namespace rawengine

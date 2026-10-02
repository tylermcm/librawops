#include "ImageAnalysis.hpp"
#include "EditGraph.hpp"

#include <algorithm>
#include <cmath>

namespace rawengine {

RgbHistogram histogram_rgb(const Node& output, Rect source_bounds,
                           const RenderRequest& request, const HistogramOptions& options,
                           const CancellationToken* cancellation) {
    const double span = options.upper - options.lower;
    if (options.bins == 0 || options.bins > 65536 ||
        !std::isfinite(options.lower) || !std::isfinite(options.upper) ||
        !(span > 0.0) || !std::isfinite(span))
        throw std::invalid_argument("histogram requires 1..65536 bins and a finite positive range");
    auto check_cancelled = [&] {
        if (cancellation && cancellation->is_cancelled()) throw RenderCancelled();
    };
    check_cancelled();
    RgbHistogram result;
    result.request = request;
    result.options = options;
    result.descriptor = output.output_descriptor();
    if (result.descriptor.format != PixelFormat::RGBFloat32)
        throw std::invalid_argument("histogram requires RGBFloat32 output");
    for (auto& channel : result.channels) channel.counts.resize(options.bins);

    const auto right = std::uint64_t(request.viewport.x) + request.viewport.width;
    const auto bottom = std::uint64_t(request.viewport.y) + request.viewport.height;
    if (!request.viewport.width || !request.viewport.height ||
        right > (std::uint64_t{1} << 32) || bottom > (std::uint64_t{1} << 32))
        throw std::invalid_argument("histogram requires a nonempty addressable viewport");
    std::uint64_t next_x = request.viewport.x, next_y = request.viewport.y;
    Renderer{}.render_tiles(output, source_bounds, request, [&](const Tile& tile) {
        check_cancelled();
        // The renderer delivers row-major tiles. Reject malformed custom nodes
        // before reading their storage or accepting incomplete coverage.
        const Rect expected{static_cast<std::uint32_t>(next_x),
                            static_cast<std::uint32_t>(next_y),
                            static_cast<std::uint32_t>(std::min<std::uint64_t>(request.tile_size, right - next_x)),
                            static_cast<std::uint32_t>(std::min<std::uint64_t>(request.tile_size, bottom - next_y))};
        const auto pixels = std::uint64_t(expected.width) * expected.height;
        if (tile.bounds.x != expected.x || tile.bounds.y != expected.y ||
            tile.bounds.width != expected.width || tile.bounds.height != expected.height ||
            tile.rgb.size() % 3 != 0 || tile.rgb.size() / 3 != pixels ||
            tile.descriptor != result.descriptor)
            throw std::invalid_argument("histogram received inconsistent tile bounds, storage or descriptor");
        for (std::size_t i = 0; i < tile.rgb.size(); ++i) {
            const float sample = tile.rgb[i];
            auto& channel = result.channels[i % 3];
            if (!std::isfinite(sample)) { ++channel.nonfinite; continue; }
            if (!channel.minimum || sample < *channel.minimum) channel.minimum = sample;
            if (!channel.maximum || sample > *channel.maximum) channel.maximum = sample;
            if (sample < options.lower) { ++channel.underflow; continue; }
            if (sample > options.upper) { ++channel.overflow; continue; }
            // Divide before multiplying to avoid overflow for extreme ranges.
            const auto bin = std::min<std::uint32_t>(options.bins - 1,
                static_cast<std::uint32_t>((double(sample) - options.lower) / span * options.bins));
            ++channel.counts[bin];
        }
        result.pixel_count += pixels;
        next_x += expected.width;
        if (next_x == right) { next_x = request.viewport.x; next_y += expected.height; }
        check_cancelled();
    }, cancellation);
    check_cancelled();
    return result;
}

RgbHistogram histogram_rgb(const ImageGraph& graph, const RenderRequest& request,
                           const HistogramOptions& options, const CancellationToken* cancellation) {
    return histogram_rgb(graph.output(), graph.source_bounds(), request, options, cancellation);
}

RgbHistogram histogram_rgb(const ExecutableEditGraph& graph, const RenderRequest& request,
                           const HistogramOptions& options, const CancellationToken* cancellation) {
    return histogram_rgb(graph.output(), graph.output_bounds(), request, options, cancellation);
}

} // namespace rawengine

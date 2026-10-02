#include "ImageAnalysis.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>

using namespace rawengine;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class E = std::invalid_argument, class F> void rejects(F action) {
    try { action(); } catch (const E&) { return; }
    throw std::runtime_error("expected analysis exception");
}

class Probe final : public Node {
public:
    enum class Fault { None, Bounds, Storage, Descriptor };
    Fault fault = Fault::None;
    CancellationToken* cancellation = nullptr;
    mutable unsigned calls = 0;
    mutable std::uint64_t largest_tile = 0;
    ImageDescriptor output_descriptor() const noexcept override { return ImageDescriptor::camera_linear(); }
    Tile render(Rect r) const override {
        ++calls;
        largest_tile = std::max(largest_tile, std::uint64_t(r.width) * r.height);
        Tile tile{r, {}, output_descriptor()};
        for (std::uint32_t y = r.y; y < r.y + r.height; ++y)
            for (std::uint32_t x = r.x; x < r.x + r.width; ++x) {
                tile.rgb.push_back(float(x % 9) * 0.25f - 0.5f);
                tile.rgb.push_back(float(y % 9) * 0.25f - 0.5f);
                tile.rgb.push_back(std::numeric_limits<float>::quiet_NaN());
            }
        if (fault == Fault::Bounds) ++tile.bounds.x;
        if (fault == Fault::Storage) tile.rgb.pop_back();
        if (fault == Fault::Descriptor) tile.descriptor = ImageDescriptor::srgb_output();
        if (cancellation) cancellation->cancel();
        return tile;
    }
};

void edge_bins_and_tiles() {
    Probe node;
    const Rect bounds{7, 5, 31, 23};
    const RenderRequest request{{9, 7, 19, 13}, 4};
    auto result = histogram_rgb(node, bounds, request, {4, 0, 1});
    require(result.pixel_count == 247 && node.largest_tile <= 16 && node.calls == 20,
            "analysis must stream clipped tiles over a nonzero ROI");
    require(result.descriptor == node.output_descriptor(), "output descriptor lost");
    for (unsigned c = 0; c < 2; ++c) {
        std::array<std::uint64_t, 4> expected{};
        std::uint64_t below = 0, above = 0;
        // Independent discrete truth: site residues 0,1 underflow; 2..5
        // each occupy one bin; 6 joins the last bin; 7,8 overflow.
        for (unsigned y = 7; y < 20; ++y) for (unsigned x = 9; x < 28; ++x) {
            const auto site = (c == 0 ? x : y) % 9;
            if (site < 2) ++below;
            else if (site > 6) ++above;
            else ++expected[site == 6 ? 3 : site - 2];
        }
        const auto& channel = result.channels[c];
        require(std::equal(expected.begin(), expected.end(), channel.counts.begin()), "bin edge convention differs");
        require(channel.underflow == below && channel.overflow == above && channel.nonfinite == 0,
                "signed/headroom samples were lost");
        require(channel.minimum == -0.5f && channel.maximum == 1.5f, "extrema must include outliers");
    }
    require(result.channels[2].nonfinite == 247 && !result.channels[2].minimum && !result.channels[2].maximum,
            "all nonfinite channel needs absent extrema");
    for (unsigned size : {1u, 3u, 7u, 256u}) {
        auto other = histogram_rgb(node, bounds, RenderRequest{request.viewport, size}, {4, 0, 1});
        for (unsigned c = 0; c < 3; ++c) {
            require(other.channels[c].counts == result.channels[c].counts &&
                    other.channels[c].underflow == result.channels[c].underflow &&
                    other.channels[c].overflow == result.channels[c].overflow &&
                    other.channels[c].nonfinite == result.channels[c].nonfinite &&
                    other.channels[c].minimum == result.channels[c].minimum &&
                    other.channels[c].maximum == result.channels[c].maximum, "tile partition changes analysis");
        }
    }
}

void nonfinite_and_extreme_ranges() {
    class Values final : public Node {
    public:
        ImageDescriptor output_descriptor() const noexcept override { return ImageDescriptor::linear_srgb(); }
        Tile render(Rect r) const override {
            return {r, {-std::numeric_limits<float>::infinity(), 0, -2,
                         std::numeric_limits<float>::infinity(), 1, 2,
                         std::numeric_limits<float>::quiet_NaN(), 0.5f, 0}, output_descriptor()};
        }
    } node;
    auto result = histogram_rgb(node, {0,0,3,1}, RenderRequest{{0,0,3,1}}, {1,0,1});
    require(result.channels[0].nonfinite == 3 && !result.channels[0].minimum, "infinities must not become outliers");
    require(result.channels[1].counts[0] == 3 && result.channels[2].underflow == 1 && result.channels[2].overflow == 1,
            "single bin must include both endpoints");
    result = histogram_rgb(node, {0,0,3,1}, RenderRequest{{0,0,3,1}}, {65536,-1e300,1e300});
    require(result.channels[1].counts[32768] == 3, "wide finite ranges must avoid multiplication overflow");
    result = histogram_rgb(node, {0,0,3,1}, RenderRequest{{0,0,3,1}}, {2,0,std::numeric_limits<double>::denorm_min()});
    require(result.channels[1].counts[0] == 1 && result.channels[1].overflow == 2, "tiny finite ranges misclassified");
}

void invalid_and_cancelled() {
    Probe node;
    const Rect bounds{0,0,9,9};
    const RenderRequest request{bounds, 3};
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    for (const auto options : {HistogramOptions{0,0,1}, {65537,0,1}, {4,1,1}, {4,2,1},
                               {4,nan,1}, {4,0,inf}, {4,-1e308,1e308}})
        rejects([&] { histogram_rgb(node, bounds, request, options); });
    require(node.calls == 0, "invalid options rendered input");
    rejects([&] { histogram_rgb(node, bounds, RenderRequest{{0,0,0,1}}); });
    rejects([&] { histogram_rgb(node, bounds, RenderRequest{bounds,0}); });
    rejects([&] { histogram_rgb(node, bounds, RenderRequest{{0,0,4,4},3,{1,RenderQuality::Preview}}); });
    rejects<std::out_of_range>([&] { histogram_rgb(node, bounds, RenderRequest{{8,8,2,2}}); });
    rejects([&] { histogram_rgb(node, {0xffffffffu,0,2,1}, RenderRequest{{0xffffffffu,0,2,1}}); });
    for (auto fault : {Probe::Fault::Bounds, Probe::Fault::Storage, Probe::Fault::Descriptor}) {
        node.fault = fault;
        rejects([&] { histogram_rgb(node, bounds, request); });
    }
    node.fault = Probe::Fault::None;
    CancellationToken token;
    token.cancel();
    const auto before = node.calls;
    rejects<RenderCancelled>([&] { histogram_rgb(node, bounds, request, {}, &token); });
    require(node.calls == before, "pre-cancelled analysis rendered input");
    CancellationToken last_tile;
    node.cancellation = &last_tile;
    rejects<RenderCancelled>([&] { histogram_rgb(node, bounds, RenderRequest{bounds}, {}, &last_tile); });
    require(node.calls == before + 1, "last-tile cancellation test did not render one tile");
}

void image_graph_overload() {
    RawImage image(5,3,std::vector<std::uint16_t>(15,32768));
    ImageGraph graph(image, GraphRecipe{});
    const auto h = histogram_rgb(graph, RenderRequest{{0,0,5,3},2});
    require(h.pixel_count == 15 && h.descriptor == graph.output().output_descriptor(), "ImageGraph overload differs");
    for (const auto& c : h.channels)
        require(std::accumulate(c.counts.begin(),c.counts.end(),std::uint64_t{}) + c.underflow + c.overflow + c.nonfinite == 15,
                "per-channel coverage incomplete");
}
} // namespace

int main() {
    try {
        edge_bins_and_tiles(); nonfinite_and_extreme_ranges(); invalid_and_cancelled(); image_graph_overload();
        std::cout << "Image analysis tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}

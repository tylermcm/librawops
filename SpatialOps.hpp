#pragma once

#include "RawEngine.hpp"

namespace rawengine {

struct LocalStatisticsTile {
    Rect bounds;
    ImageDescriptor descriptor;
    std::uint32_t radius = 3; // 0..8, measured in requested-level pixels.
    std::vector<double> mean, variance; // Interleaved RGB; population variance.
    std::vector<std::uint32_t> finite_count; // Nonfinite samples excluded per channel.
};

// Windows clip at the true image edge. Missing neighbors at a tile edge must
// be supplied through the complete halo. Empty finite windows have NaN moments.
RAWENGINE_API Rect local_statistics_region(Rect output, Rect image_bounds, std::uint32_t radius);
RAWENGINE_API LocalStatisticsTile local_statistics_rgb(
    const Tile& input, Rect output, Rect image_bounds, std::uint32_t radius = 3);
using LocalStatisticsCallback = std::function<void(const LocalStatisticsTile&)>;
// Callback receives temporary storage; copy only if retaining it. No full-frame
// result is retained. Cancellation/errors stop delivery; earlier callbacks remain.
RAWENGINE_API void analyze_local_rgb(
    const Node& output, Rect native_bounds, const RenderRequest& request,
    const LocalStatisticsCallback& callback, std::uint32_t radius = 3,
    const CancellationToken* cancellation = nullptr);
RAWENGINE_API void analyze_local_rgb(
    const ImageGraph& graph, const RenderRequest& request,
    const LocalStatisticsCallback& callback, std::uint32_t radius = 3,
    const CancellationToken* cancellation = nullptr);
RAWENGINE_API void analyze_local_rgb(
    const ExecutableEditGraph& graph, const RenderRequest& request,
    const LocalStatisticsCallback& callback, std::uint32_t radius = 3,
    const CancellationToken* cancellation = nullptr);

struct ConvolutionKernel {
    std::uint32_t width = 1, height = 1; // Odd dimensions, each 1..17.
    std::vector<double> coefficients{1.0}; // Row-major, finite, abs <=65536.
};
RAWENGINE_API void validate_convolution_kernel(const ConvolutionKernel& kernel);

// Original bounded reference convolution on scene-linear working RGB. The
// kernel is flipped on both axes (convolution, not correlation), accumulates in
// float64 and casts once to float32. Replicate the true image boundary; no
// normalization/clipping. Requested-level radius; reduction happens upstream.
class RAWENGINE_API ConvolutionNode final : public Node {
public:
    ConvolutionNode(std::shared_ptr<const Node> input, Rect native_bounds, ConvolutionKernel kernel);
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region(Rect output, Rect source_bounds) const override;
    Rect input_region_level(Rect output, Rect source_bounds, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return input_->output_descriptor(); }
private:
    std::shared_ptr<const Node> input_;
    Rect native_bounds_;
    ConvolutionKernel kernel_;
    bool identity_ = false;
};

} // namespace rawengine

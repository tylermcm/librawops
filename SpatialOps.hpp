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

struct WorkingYGuidedFilterSettings {
    std::uint32_t radius = 3; // 0..8, requested-level pixels; complete support is 2r.
    double epsilon = 0x1p-12; // Finite [2^-24,65536], squared working-Y units.
};
RAWENGINE_API void validate_working_y_guided_filter_settings(const WorkingYGuidedFilterSettings& settings);
RAWENGINE_API Rect working_y_guided_filter_region(Rect output, Rect image_bounds, std::uint32_t radius);
// Finite scene-linear working RGB; true-edge actual counts, shared scalar Y
// guidance and channel regressions. Signed output/overshoot are intentional.
RAWENGINE_API Tile working_y_guided_filter_rgb(
    const Tile& input, Rect output, Rect image_bounds, const WorkingYGuidedFilterSettings& settings = {});

class RAWENGINE_API WorkingYGuidedFilterNode final : public Node {
public:
    WorkingYGuidedFilterNode(std::shared_ptr<const Node> input, Rect native_bounds,
                            WorkingYGuidedFilterSettings settings = {});
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region(Rect output, Rect source_bounds) const override;
    Rect input_region_level(Rect output, Rect source_bounds, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return descriptor_; }
private:
    std::shared_ptr<const Node> input_;
    Rect native_bounds_;
    WorkingYGuidedFilterSettings settings_;
    ImageDescriptor descriptor_;
};

struct ClaritySettings {
    double amount = 0.0; // Finite [-1,1].
    std::uint32_t radius = 3; // 1..8, in requested-level pixels.
};
RAWENGINE_API void validate_clarity_settings(const ClaritySettings& settings);

// Original midtone-weighted local working-Y contrast. True borders clip the
// window; center-anchored differences accumulate in fixed global y/x order.
class RAWENGINE_API ClarityNode final : public Node {
public:
    ClarityNode(std::shared_ptr<const Node> input, Rect native_bounds, ClaritySettings settings = {});
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
    ClaritySettings settings_;
    double weight_red_, weight_blue_;
};

struct TextureSettings {
    double amount = 0.0; // Finite [-1,1].
    std::uint32_t scale = 2; // 1..4 unit blur passes; support is 2*scale pixels.
};
RAWENGINE_API void validate_texture_settings(const TextureSettings& settings);

// Original native-Y band control: B^scale - B^(2*scale), where B is ordered
// horizontal/vertical [1,2,1] with per-pass true-edge weight normalization.
class RAWENGINE_API TextureNode final : public Node {
public:
    TextureNode(std::shared_ptr<const Node> input, Rect native_bounds, TextureSettings settings = {});
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
    TextureSettings settings_;
    double weight_red_, weight_blue_;
};

struct SharpenSettings {
    double amount = 0.0; // Finite 0..2; exact zero bypass.
    std::uint32_t radius = 1; // 1..3, requested-level pixels.
};
RAWENGINE_API void validate_sharpen_settings(const SharpenSettings& settings);

// Original bounded RGB unsharp residual; actual-count square at true borders.
// Each channel uses fixed row-major center differences; no Y/tone/noise selector.
class RAWENGINE_API SharpenNode final : public Node {
public:
    SharpenNode(std::shared_ptr<const Node> input, Rect native_bounds, SharpenSettings settings = {});
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
    SharpenSettings settings_;
};

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

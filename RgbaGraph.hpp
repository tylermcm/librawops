#pragma once

#include "MaskOps.hpp"

namespace rawengine {

struct RgbaMetadata {
    Rect bounds;
    std::uint64_t row_stride_samples = 0;
    WorkingSpace working_space = WorkingSpace::LinearProPhotoD50;
};

class RAWENGINE_API RgbaImage {
public:
    RgbaImage(RgbaMetadata metadata, const std::vector<float>& samples);
    const RgbaMetadata& metadata() const noexcept;
    const std::vector<float>& samples() const noexcept;
    std::array<std::uint8_t,32> fingerprint() const;
private:
    struct State;
    std::shared_ptr<const State> state_;
};

struct StraightRgbFloat64Tile {
    Rect bounds;
    std::vector<double> rgb;
    WorkingSpace working_space = WorkingSpace::LinearProPhotoD50;
};
RAWENGINE_API StraightRgbFloat64Tile straight_rgb_float64(const PremultipliedRgbaTile& tile);

// Separate typed graph; RGB-only Node never stores or implicitly accepts alpha.
class RAWENGINE_API RgbaNode {
public:
    virtual ~RgbaNode() = default;
    virtual PremultipliedRgbaTile render_level(Rect bounds, RenderLevel level) const = 0;
    PremultipliedRgbaTile render(Rect bounds) const { return render_level(bounds, {}); }
    virtual bool supports_level(RenderLevel level) const noexcept = 0;
    virtual Rect native_bounds() const noexcept = 0;
    virtual WorkingSpace working_space() const noexcept = 0;
    Rect output_bounds(RenderLevel level = {}) const;
    Rect input_region_level(Rect output, Rect input_bounds, RenderLevel level) const;
    RenderLevel input_level(RenderLevel level) const;
    virtual const RgbaNode* rgba_input(std::size_t index) const noexcept { (void)index; return nullptr; }
    virtual const Node* rgb_input() const noexcept { return nullptr; }
    virtual const CoverageNode* coverage_input() const noexcept { return nullptr; }
    virtual std::optional<std::array<std::uint8_t,32>> source_fingerprint() const { return std::nullopt; }
};

class RAWENGINE_API RgbaRasterNode final : public RgbaNode {
public:
    explicit RgbaRasterNode(RgbaImage image);
    PremultipliedRgbaTile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override;
    WorkingSpace working_space() const noexcept override;
    std::optional<std::array<std::uint8_t,32>> source_fingerprint() const override;
private:
    RgbaImage image_;
};

class RAWENGINE_API RgbPremultiplyNode final : public RgbaNode {
public:
    RgbPremultiplyNode(std::shared_ptr<const Node> image,
                      std::shared_ptr<const CoverageNode> alpha, Rect native_bounds);
    PremultipliedRgbaTile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override { return bounds_; }
    WorkingSpace working_space() const noexcept override { return space_; }
    const Node* rgb_input() const noexcept override { return image_.get(); }
    const CoverageNode* coverage_input() const noexcept override { return alpha_.get(); }
private:
    std::shared_ptr<const Node> image_;
    std::shared_ptr<const CoverageNode> alpha_;
    Rect bounds_;
    WorkingSpace space_;
};

class RAWENGINE_API RgbaApplyCoverageNode final : public RgbaNode {
public:
    RgbaApplyCoverageNode(std::shared_ptr<const RgbaNode> image,
                          std::shared_ptr<const CoverageNode> coverage);
    PremultipliedRgbaTile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override { return bounds_; }
    WorkingSpace working_space() const noexcept override { return space_; }
    const RgbaNode* rgba_input(std::size_t index) const noexcept override { return index==0 ? image_.get() : nullptr; }
    const CoverageNode* coverage_input() const noexcept override { return coverage_.get(); }
private:
    std::shared_ptr<const RgbaNode> image_;
    std::shared_ptr<const CoverageNode> coverage_;
    Rect bounds_;
    WorkingSpace space_;
};

class RAWENGINE_API RgbaSourceOverNode final : public RgbaNode {
public:
    RgbaSourceOverNode(std::shared_ptr<const RgbaNode> source,
                       std::shared_ptr<const RgbaNode> backdrop);
    PremultipliedRgbaTile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override { return bounds_; }
    WorkingSpace working_space() const noexcept override { return space_; }
    const RgbaNode* rgba_input(std::size_t index) const noexcept override {
        return index==0 ? source_.get() : index==1 ? backdrop_.get() : nullptr;
    }
private:
    std::shared_ptr<const RgbaNode> source_, backdrop_;
    Rect bounds_;
    WorkingSpace space_;
};

class RAWENGINE_API RgbaAlphaNode final : public CoverageNode {
public:
    explicit RgbaAlphaNode(std::shared_ptr<const RgbaNode> image);
    CoverageTile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override { return bounds_; }
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region_level(Rect output, Rect input_bounds, RenderLevel level) const override;
    const RgbaNode* rgba_input() const noexcept { return image_.get(); }
private:
    std::shared_ptr<const RgbaNode> image_;
    Rect bounds_;
    WorkingSpace space_;
};

class RAWENGINE_API RgbaStraightRgbNode final : public Node {
public:
    explicit RgbaStraightRgbNode(std::shared_ptr<const RgbaNode> image);
    Tile render(Rect bounds) const override { return render_level(bounds, {}); }
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    ImageDescriptor output_descriptor() const noexcept override { return ImageDescriptor::scene_linear(space_); }
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region_level(Rect output, Rect input_bounds, RenderLevel level) const override;
    Rect output_bounds(RenderLevel level = {}) const;
    const RgbaNode* rgba_input() const noexcept { return image_.get(); }
private:
    std::shared_ptr<const RgbaNode> image_;
    Rect bounds_;
    WorkingSpace space_;
};

} // namespace rawengine

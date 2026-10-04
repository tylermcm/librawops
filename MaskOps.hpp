#pragma once

#include "CoverageSource.hpp"

namespace rawengine {

enum class MaskCombineMode { Add, Subtract, Intersect };
RAWENGINE_API float invert_coverage(float value);
RAWENGINE_API float combine_coverage(float base, float layer, MaskCombineMode mode);
RAWENGINE_API Tile masked_mix_rgb(const Tile& base, const Tile& layer,
                                 const CoverageTile& mask, double amount);

class RAWENGINE_API CoverageNode {
public:
    virtual ~CoverageNode() = default;
    virtual CoverageTile render(Rect bounds) const { return render_level(bounds, {}); }
    virtual CoverageTile render_level(Rect bounds, RenderLevel level) const = 0;
    virtual bool supports_level(RenderLevel level) const noexcept = 0;
    virtual Rect native_bounds() const noexcept = 0;
    virtual Rect output_bounds(RenderLevel level = {}) const;
    virtual RenderLevel input_level(RenderLevel level) const { return level; }
    virtual Rect input_region_level(Rect output, Rect input_bounds, RenderLevel level) const {
        (void)input_bounds; (void)level; return output;
    }
    virtual std::optional<std::array<std::uint8_t,32>> source_fingerprint() const { return std::nullopt; }
    virtual Rect required_native_region(Rect output, RenderLevel level) const {
        (void)output; (void)level;
        throw std::invalid_argument("coverage node is not a native source");
    }
};

class RAWENGINE_API CoverageRasterNode final : public CoverageNode {
public:
    explicit CoverageRasterNode(CoverageImage image);
    CoverageTile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override;
    std::optional<std::array<std::uint8_t,32>> source_fingerprint() const override;
    Rect required_native_region(Rect output, RenderLevel level) const override;
private:
    CoverageSourceNode source_;
};

class RAWENGINE_API CoverageInvertNode final : public CoverageNode {
public:
    explicit CoverageInvertNode(std::shared_ptr<const CoverageNode> input);
    CoverageTile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override;
private:
    std::shared_ptr<const CoverageNode> input_;
};

class RAWENGINE_API CoverageCombineNode final : public CoverageNode {
public:
    CoverageCombineNode(std::shared_ptr<const CoverageNode> base,
                        std::shared_ptr<const CoverageNode> layer, MaskCombineMode mode);
    CoverageTile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override;
private:
    std::shared_ptr<const CoverageNode> base_, layer_;
    MaskCombineMode mode_;
};

class RAWENGINE_API MaskedMixNode final : public Node {
public:
    MaskedMixNode(std::shared_ptr<const Node> base, std::shared_ptr<const Node> layer,
                  std::shared_ptr<const CoverageNode> mask, Rect native_bounds, double amount);
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    ImageDescriptor output_descriptor() const noexcept override;
    const Node* input_node() const noexcept override { return base_.get(); }
    Rect input_region_level(Rect output, Rect input_bounds, RenderLevel level) const override;
private:
    std::shared_ptr<const Node> base_, layer_;
    std::shared_ptr<const CoverageNode> mask_;
    Rect bounds_;
    double amount_;
};

} // namespace rawengine

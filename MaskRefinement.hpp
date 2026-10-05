#pragma once

#include "MaskOps.hpp"

namespace rawengine {

struct MaskDensitySettings { double density=1; };
struct MaskFeatherSettings { std::uint32_t radius=0; };
struct MaskRefineSettings { std::uint32_t radius=3; double epsilon=0x1p-12; };
RAWENGINE_API void validate_mask_density_settings(const MaskDensitySettings& settings);
RAWENGINE_API void validate_mask_feather_settings(const MaskFeatherSettings& settings);
RAWENGINE_API void validate_mask_refine_settings(const MaskRefineSettings& settings);
RAWENGINE_API float evaluate_mask_density(float coverage,const MaskDensitySettings& settings);

class RAWENGINE_API CoverageDensityNode final : public CoverageNode {
public:
    CoverageDensityNode(std::shared_ptr<const CoverageNode> mask,const MaskDensitySettings& settings);
    CoverageTile render_level(Rect bounds,RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override;
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region_level(Rect output,Rect input_bounds,RenderLevel level) const override;
private:
    std::shared_ptr<const CoverageNode> mask_;
    MaskDensitySettings settings_;
};

class RAWENGINE_API CoverageFeatherNode final : public CoverageNode {
public:
    CoverageFeatherNode(std::shared_ptr<const CoverageNode> mask,const MaskFeatherSettings& settings);
    CoverageTile render_level(Rect bounds,RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override;
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region_level(Rect output,Rect input_bounds,RenderLevel level) const override;
private:
    std::shared_ptr<const CoverageNode> mask_;
    MaskFeatherSettings settings_;
    std::array<double,65> weights_{};
};

class RAWENGINE_API CoverageRefineNode final : public CoverageNode {
public:
    CoverageRefineNode(std::shared_ptr<const CoverageNode> mask,std::shared_ptr<const Node> image,
                       Rect image_bounds,const MaskRefineSettings& settings);
    CoverageTile render_level(Rect bounds,RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override;
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region_level(Rect output,Rect input_bounds,RenderLevel level) const override;
private:
    std::shared_ptr<const CoverageNode> mask_;
    std::shared_ptr<const Node> image_;
    MaskRefineSettings settings_;
    ImageDescriptor descriptor_;
    std::array<double,2> weights_{};
};

} // namespace rawengine

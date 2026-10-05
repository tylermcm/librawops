#pragma once

#include "MaskOps.hpp"

namespace rawengine {

struct ScalarRangeSettings {
    std::array<double,4> edges{0,0,1,1};
    bool invert=false;
};
struct ColorRangeSettings {
    std::array<double,3> center{0,0,0},scales{1,1,1};
    double inner=0,outer=1;
    bool invert=false;
};
RAWENGINE_API void validate_luminance_range_settings(const ScalarRangeSettings& settings);
RAWENGINE_API void validate_depth_range_settings(const ScalarRangeSettings& settings);
RAWENGINE_API void validate_color_range_settings(const ColorRangeSettings& settings);
RAWENGINE_API float evaluate_luminance_range_mask(std::array<float,3> rgb,ImageDescriptor descriptor,
                                                 const ScalarRangeSettings& settings);
RAWENGINE_API float evaluate_color_range_mask(std::array<float,3> rgb,ImageDescriptor descriptor,
                                             const ColorRangeSettings& settings);
RAWENGINE_API float evaluate_depth_range_mask(float depth,const ScalarRangeSettings& settings);

class RAWENGINE_API CoverageLuminanceRangeNode final : public CoverageNode {
public:
    CoverageLuminanceRangeNode(std::shared_ptr<const CoverageNode> mask,std::shared_ptr<const Node> image,
                              Rect image_bounds,const ScalarRangeSettings& settings);
    CoverageTile render_level(Rect bounds,RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override;
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region_level(Rect output,Rect input_bounds,RenderLevel level) const override;
private:
    std::shared_ptr<const CoverageNode> mask_;
    std::shared_ptr<const Node> image_;
    ScalarRangeSettings settings_;
};
class RAWENGINE_API CoverageColorRangeNode final : public CoverageNode {
public:
    CoverageColorRangeNode(std::shared_ptr<const CoverageNode> mask,std::shared_ptr<const Node> image,
                          Rect image_bounds,const ColorRangeSettings& settings);
    CoverageTile render_level(Rect bounds,RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override;
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region_level(Rect output,Rect input_bounds,RenderLevel level) const override;
private:
    std::shared_ptr<const CoverageNode> mask_;
    std::shared_ptr<const Node> image_;
    ColorRangeSettings settings_;
};
class RAWENGINE_API CoverageDepthRangeNode final : public CoverageNode {
public:
    CoverageDepthRangeNode(std::shared_ptr<const CoverageNode> mask,std::shared_ptr<const CoverageNode> depth,
                          const ScalarRangeSettings& settings);
    CoverageTile render_level(Rect bounds,RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override;
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region_level(Rect output,Rect input_bounds,RenderLevel level) const override;
private:
    std::shared_ptr<const CoverageNode> mask_,depth_;
    ScalarRangeSettings settings_;
};

} // namespace rawengine

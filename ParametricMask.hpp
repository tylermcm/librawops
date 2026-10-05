#pragma once

#include "MaskOps.hpp"

namespace rawengine {

struct MaskPoint { double x=0, y=0; };
struct LinearGradientMaskSettings {
    MaskPoint start, end{1,0};
    bool invert=false;
};
struct RadialGradientMaskSettings {
    MaskPoint center;
    std::array<double,4> axes{1,0,0,1};
    double inner=0;
    bool invert=false;
};
struct PolygonMaskSettings {
    std::vector<std::vector<MaskPoint>> rings;
    bool invert=false;
};

RAWENGINE_API void validate_linear_gradient_mask_settings(const LinearGradientMaskSettings& settings);
RAWENGINE_API void validate_radial_gradient_mask_settings(const RadialGradientMaskSettings& settings);
RAWENGINE_API void validate_polygon_mask_settings(const PolygonMaskSettings& settings);
RAWENGINE_API float evaluate_linear_gradient_mask(double x,double y,const LinearGradientMaskSettings& settings);
RAWENGINE_API float evaluate_radial_gradient_mask(double x,double y,const RadialGradientMaskSettings& settings);
RAWENGINE_API float evaluate_polygon_mask(double x,double y,const PolygonMaskSettings& settings);

class RAWENGINE_API CoverageLinearGradientNode final:public CoverageNode {
public:
    CoverageLinearGradientNode(std::shared_ptr<const CoverageNode> input,const LinearGradientMaskSettings& settings);
    CoverageTile render_level(Rect bounds,RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override;
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region_level(Rect output,Rect input_bounds,RenderLevel level) const override;
private:
    std::shared_ptr<const CoverageNode> input_;
    LinearGradientMaskSettings settings_;
};
class RAWENGINE_API CoverageRadialGradientNode final:public CoverageNode {
public:
    CoverageRadialGradientNode(std::shared_ptr<const CoverageNode> input,const RadialGradientMaskSettings& settings);
    CoverageTile render_level(Rect bounds,RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override;
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region_level(Rect output,Rect input_bounds,RenderLevel level) const override;
private:
    std::shared_ptr<const CoverageNode> input_;
    RadialGradientMaskSettings settings_;
};
class RAWENGINE_API CoveragePolygonNode final:public CoverageNode {
public:
    CoveragePolygonNode(std::shared_ptr<const CoverageNode> input,const PolygonMaskSettings& settings);
    CoverageTile render_level(Rect bounds,RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override;
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region_level(Rect output,Rect input_bounds,RenderLevel level) const override;
private:
    std::shared_ptr<const CoverageNode> input_;
    PolygonMaskSettings settings_;
};

} // namespace rawengine

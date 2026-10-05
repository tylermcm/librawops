#pragma once

#include "MaskOps.hpp"

namespace rawengine {

enum class BrushMode { Paint, Erase };

struct BrushPoint {
    double x = 0, y = 0, pressure = 1;
};

struct BrushStroke {
    BrushMode mode = BrushMode::Paint;
    double radius = 8, hardness = 0.5, flow = 1, opacity = 1;
    std::vector<BrushPoint> points;
};

struct BrushMaskSettings {
    std::vector<BrushStroke> strokes;
};

RAWENGINE_API void validate_brush_mask_settings(const BrushMaskSettings& settings);
RAWENGINE_API float apply_brush_mask(float coverage, double center_x, double center_y,
                                   const BrushMaskSettings& settings);

class RAWENGINE_API CoverageBrushNode final : public CoverageNode {
public:
    CoverageBrushNode(std::shared_ptr<const CoverageNode> input, const BrushMaskSettings& settings);
    CoverageTile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect native_bounds() const noexcept override;
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region_level(Rect output, Rect input_bounds, RenderLevel level) const override;
private:
    std::shared_ptr<const CoverageNode> input_;
    BrushMaskSettings settings_;
};

} // namespace rawengine

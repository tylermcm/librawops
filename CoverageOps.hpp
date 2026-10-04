#pragma once

#include "RawEngine.hpp"

namespace rawengine {

// Scalar coverage has no color space or transfer function; one float per pixel.
struct CoverageTile {
    Rect bounds;
    std::vector<float> coverage;
};

struct PremultipliedRgbaPixel {
    std::array<float, 3> rgb{};
    float alpha = 0;
};

// Scene-linear signed/headroom RGB with finite [0,1] alpha, four floats/pixel.
// This is a separate payload; RGB-only Tile/Node never carry RGBA or masks.
struct PremultipliedRgbaTile {
    Rect bounds;
    std::vector<float> rgba;
    WorkingSpace working_space = WorkingSpace::LinearProPhotoD50;
};

RAWENGINE_API void validate_coverage_tile(const CoverageTile& tile);
RAWENGINE_API void validate_premultiplied_rgba_pixel(const PremultipliedRgbaPixel& pixel);
RAWENGINE_API void validate_premultiplied_rgba_tile(const PremultipliedRgbaTile& tile);

RAWENGINE_API PremultipliedRgbaPixel premultiply_rgb(
    const std::array<float, 3>& straight_rgb, float alpha);
// No epsilon floor. Binary64 safely represents finite binary32 RGB / positive alpha.
RAWENGINE_API std::array<double, 3> straight_rgb(const PremultipliedRgbaPixel& pixel);
// Explicit narrowed access throws overflow_error for values outside float32 range.
RAWENGINE_API std::array<float, 3> straight_rgb_float32(const PremultipliedRgbaPixel& pixel);
RAWENGINE_API PremultipliedRgbaPixel apply_coverage(
    const PremultipliedRgbaPixel& pixel, float coverage);
RAWENGINE_API PremultipliedRgbaPixel source_over(
    const PremultipliedRgbaPixel& source, const PremultipliedRgbaPixel& backdrop);

// Aligned point operations. Entire inputs are validated even for transparent bypass.
RAWENGINE_API PremultipliedRgbaTile premultiply_rgb(
    const Tile& straight_rgb, const CoverageTile& alpha);
RAWENGINE_API PremultipliedRgbaTile apply_coverage(
    const PremultipliedRgbaTile& image, const CoverageTile& coverage);
RAWENGINE_API PremultipliedRgbaTile source_over(
    const PremultipliedRgbaTile& source, const PremultipliedRgbaTile& backdrop);

} // namespace rawengine

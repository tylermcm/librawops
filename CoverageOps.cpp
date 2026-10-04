#include "CoverageOps.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace rawengine {
namespace {

static_assert(std::numeric_limits<float>::is_iec559 &&
              std::numeric_limits<float>::radix == 2 &&
              std::numeric_limits<float>::digits == 24 && sizeof(float) == 4);
static_assert(std::numeric_limits<double>::is_iec559 &&
              std::numeric_limits<double>::radix == 2 &&
              std::numeric_limits<double>::digits == 53 && sizeof(double) == 8);

void coverage_value(float value) {
    if (!std::isfinite(value) || value < 0 || value > 1)
        throw std::invalid_argument("coverage must be finite and in [0,1]");
}

std::size_t sample_count(Rect bounds, std::size_t channels) {
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    if (!bounds.width || !bounds.height || bounds.x > maximum - bounds.width ||
        bounds.y > maximum - bounds.height)
        throw std::invalid_argument("coverage/alpha bounds are empty or overflow");
    const auto pixels = static_cast<std::uint64_t>(bounds.width) * bounds.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / channels / sizeof(float))
        throw std::invalid_argument("coverage/alpha storage size overflows");
    return static_cast<std::size_t>(pixels) * channels;
}

bool same_bounds(Rect a, Rect b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

void matching_bounds(Rect a, Rect b) {
    if (!same_bounds(a, b))
        throw std::invalid_argument("coverage/alpha inputs must have matching bounds");
}

void working_space(WorkingSpace space) {
    if (space != WorkingSpace::LinearProPhotoD50 && space != WorkingSpace::LinearRec2020D65)
        throw std::invalid_argument("RGBA requires an admitted scene-linear working space");
}

float store_color(double value) {
    constexpr auto maximum = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(value) || value < -maximum || value > maximum)
        throw std::overflow_error("alpha color result is outside finite float32 range");
    return static_cast<float>(value);
}

PremultipliedRgbaPixel load(const std::vector<float>& values, std::size_t offset) {
    return {{values[offset], values[offset + 1], values[offset + 2]}, values[offset + 3]};
}

void store(std::vector<float>& values, std::size_t offset, const PremultipliedRgbaPixel& pixel) {
    for (std::size_t c = 0; c < 3; ++c) values[offset + c] = pixel.rgb[c];
    values[offset + 3] = pixel.alpha;
}

PremultipliedRgbaPixel canonical(const PremultipliedRgbaPixel& pixel) {
    return pixel.alpha == 0 ? PremultipliedRgbaPixel{} : pixel;
}

} // namespace

void validate_coverage_tile(const CoverageTile& tile) {
    if (tile.coverage.size() != sample_count(tile.bounds, 1))
        throw std::invalid_argument("coverage storage must contain exactly one value per pixel");
    for (float value : tile.coverage) coverage_value(value);
}

void validate_premultiplied_rgba_pixel(const PremultipliedRgbaPixel& pixel) {
    coverage_value(pixel.alpha);
    for (float value : pixel.rgb) {
        if (!std::isfinite(value) || (pixel.alpha == 0 && value != 0))
            throw std::invalid_argument("premultiplied color is nonfinite or hidden at zero alpha");
    }
}

void validate_premultiplied_rgba_tile(const PremultipliedRgbaTile& tile) {
    working_space(tile.working_space);
    if (tile.rgba.size() != sample_count(tile.bounds, 4))
        throw std::invalid_argument("RGBA storage must contain exactly four values per pixel");
    for (std::size_t i = 0; i < tile.rgba.size(); i += 4)
        validate_premultiplied_rgba_pixel(load(tile.rgba, i));
}

PremultipliedRgbaPixel premultiply_rgb(const std::array<float, 3>& color, float alpha) {
    coverage_value(alpha);
    for (float value : color)
        if (!std::isfinite(value))
            throw std::invalid_argument("straight color must be finite, including at zero alpha");
    if (alpha == 0) return {};
    if (alpha == 1) return {color, alpha};
    PremultipliedRgbaPixel result{{}, alpha};
    for (std::size_t c = 0; c < 3; ++c)
        result.rgb[c] = store_color(static_cast<double>(color[c]) * static_cast<double>(alpha));
    return result;
}

std::array<double, 3> straight_rgb(const PremultipliedRgbaPixel& pixel) {
    validate_premultiplied_rgba_pixel(pixel);
    std::array<double, 3> result{};
    if (pixel.alpha != 0)
        for (std::size_t c = 0; c < 3; ++c)
            result[c] = static_cast<double>(pixel.rgb[c]) / static_cast<double>(pixel.alpha);
    return result;
}

std::array<float, 3> straight_rgb_float32(const PremultipliedRgbaPixel& pixel) {
    const auto wide = straight_rgb(pixel);
    std::array<float, 3> result{};
    for (std::size_t c = 0; c < 3; ++c) result[c] = store_color(wide[c]);
    return result;
}

PremultipliedRgbaPixel apply_coverage(const PremultipliedRgbaPixel& pixel, float coverage) {
    validate_premultiplied_rgba_pixel(pixel);
    coverage_value(coverage);
    if (coverage == 0 || pixel.alpha == 0) return {};
    if (coverage == 1) return pixel;
    PremultipliedRgbaPixel result{{}, static_cast<float>(
        static_cast<double>(pixel.alpha) * static_cast<double>(coverage))};
    if (result.alpha == 0) return {};
    for (std::size_t c = 0; c < 3; ++c)
        result.rgb[c] = store_color(static_cast<double>(pixel.rgb[c]) * static_cast<double>(coverage));
    return result;
}

PremultipliedRgbaPixel source_over(
    const PremultipliedRgbaPixel& source, const PremultipliedRgbaPixel& backdrop) {
    validate_premultiplied_rgba_pixel(source);
    validate_premultiplied_rgba_pixel(backdrop);
    if (source.alpha == 0) return canonical(backdrop);
    if (source.alpha == 1 || backdrop.alpha == 0) return source;
    const double remaining = 1.0 - static_cast<double>(source.alpha);
    const double backdrop_alpha = static_cast<double>(backdrop.alpha) * remaining;
    PremultipliedRgbaPixel result{{}, static_cast<float>(
        static_cast<double>(source.alpha) + backdrop_alpha)};
    coverage_value(result.alpha);
    if (result.alpha == 0) return {};
    for (std::size_t c = 0; c < 3; ++c) {
        const double contribution = static_cast<double>(backdrop.rgb[c]) * remaining;
        result.rgb[c] = store_color(static_cast<double>(source.rgb[c]) + contribution);
    }
    return result;
}

PremultipliedRgbaTile premultiply_rgb(const Tile& image, const CoverageTile& alpha) {
    validate_coverage_tile(alpha);
    matching_bounds(image.bounds, alpha.bounds);
    WorkingSpace space;
    if (image.descriptor == ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50))
        space = WorkingSpace::LinearProPhotoD50;
    else if (image.descriptor == ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65))
        space = WorkingSpace::LinearRec2020D65;
    else
        throw std::invalid_argument("premultiplication requires exact scene-linear RGB metadata");
    if (image.rgb.size() != sample_count(image.bounds, 3))
        throw std::invalid_argument("straight RGB storage must match bounds");
    // Validate all RGB before allocating an output, including fully transparent pixels.
    for (float value : image.rgb)
        if (!std::isfinite(value)) throw std::invalid_argument("straight RGB contains nonfinite values");
    PremultipliedRgbaTile result{image.bounds, std::vector<float>(sample_count(image.bounds, 4)), space};
    for (std::size_t i = 0; i < alpha.coverage.size(); ++i)
        store(result.rgba, i * 4, premultiply_rgb(
            std::array<float, 3>{image.rgb[i * 3], image.rgb[i * 3 + 1], image.rgb[i * 3 + 2]},
            alpha.coverage[i]));
    return result;
}

PremultipliedRgbaTile apply_coverage(const PremultipliedRgbaTile& image, const CoverageTile& coverage) {
    validate_premultiplied_rgba_tile(image);
    validate_coverage_tile(coverage);
    matching_bounds(image.bounds, coverage.bounds);
    PremultipliedRgbaTile result{image.bounds, std::vector<float>(image.rgba.size()), image.working_space};
    for (std::size_t i = 0; i < coverage.coverage.size(); ++i)
        store(result.rgba, i * 4, apply_coverage(load(image.rgba, i * 4), coverage.coverage[i]));
    return result;
}

PremultipliedRgbaTile source_over(
    const PremultipliedRgbaTile& source, const PremultipliedRgbaTile& backdrop) {
    validate_premultiplied_rgba_tile(source);
    validate_premultiplied_rgba_tile(backdrop);
    matching_bounds(source.bounds, backdrop.bounds);
    if (source.working_space != backdrop.working_space)
        throw std::invalid_argument("source-over requires matching scene-linear working spaces");
    PremultipliedRgbaTile result{source.bounds, std::vector<float>(source.rgba.size()), source.working_space};
    for (std::size_t i = 0; i < source.rgba.size(); i += 4)
        store(result.rgba, i, source_over(load(source.rgba, i), load(backdrop.rgba, i)));
    return result;
}

} // namespace rawengine

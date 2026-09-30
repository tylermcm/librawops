#include "RawEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace rawengine {
namespace {

std::size_t checked_elements(std::uint32_t width, std::uint32_t height,
                             std::size_t channels) {
    const auto max = std::vector<float>().max_size();
    if (width && height > max / width / channels)
        throw std::length_error("image dimensions exceed addressable storage");
    return static_cast<std::size_t>(width) * height * channels;
}

void validate_rect(Rect area, Rect r) {
    if (r.x < area.x || r.y < area.y ||
        static_cast<std::uint64_t>(r.x) + r.width >
            static_cast<std::uint64_t>(area.x) + area.width ||
        static_cast<std::uint64_t>(r.y) + r.height >
            static_cast<std::uint64_t>(area.y) + area.height)
        throw std::out_of_range("viewport is outside the active RAW area");
}

Rect request_bounds(Rect source_bounds, RenderLevel level) {
    if (level.mip == 0 &&
        (level.quality == RenderQuality::Final ||
         level.quality == RenderQuality::Preview))
        return source_bounds;
    if (level.mip == 1 && level.quality == RenderQuality::Preview &&
        source_bounds.x == 0 && source_bounds.y == 0)
        return {0, 0, source_bounds.width / 2 + source_bounds.width % 2,
                source_bounds.height / 2 + source_bounds.height % 2};
    throw std::invalid_argument("render level is not implemented for these source bounds");
}

void validate_rect(const RawImage& image, Rect r) {
    validate_rect(image.metadata().active_area, r);
}

Rect expand_rect(Rect output, Rect source_bounds, std::uint32_t radius) {
    validate_rect(source_bounds, output);
    const auto left = std::max<std::int64_t>(source_bounds.x,
                                             static_cast<std::int64_t>(output.x) - radius);
    const auto top = std::max<std::int64_t>(source_bounds.y,
                                            static_cast<std::int64_t>(output.y) - radius);
    const auto right = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(source_bounds.x) + source_bounds.width,
        static_cast<std::uint64_t>(output.x) + output.width + radius);
    const auto bottom = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(source_bounds.y) + source_bounds.height,
        static_cast<std::uint64_t>(output.y) + output.height + radius);
    return {static_cast<std::uint32_t>(left), static_cast<std::uint32_t>(top),
            static_cast<std::uint32_t>(right - left),
            static_cast<std::uint32_t>(bottom - top)};
}

void validate_tile(const Tile& tile, Rect requested, ImageDescriptor expected) {
    if (tile.bounds.x != requested.x || tile.bounds.y != requested.y ||
        tile.bounds.width != requested.width || tile.bounds.height != requested.height ||
        tile.descriptor != expected ||
        tile.rgb.size() != checked_elements(requested.width, requested.height, 3))
        throw std::domain_error("upstream node returned an invalid tile");
}

std::size_t site_index(const RawMetadata& metadata, std::uint32_t x, std::uint32_t y) {
    return (((y & 1u) + metadata.cfa_phase_y) & 1u) * 2u +
           (((x & 1u) + metadata.cfa_phase_x) & 1u);
}

int cfa_color(const RawMetadata& metadata, std::uint32_t x, std::uint32_t y) {
    // R=0, G=1, B=2. The four patterns are their 2x2 top-left cells.
    constexpr int colors[4][2][2] = {
        {{0, 1}, {1, 2}}, {{2, 1}, {1, 0}},
        {{1, 0}, {2, 1}}, {{1, 2}, {0, 1}}
    };
    const auto site = site_index(metadata, x, y);
    return colors[static_cast<int>(metadata.pattern)][site / 2][site % 2];
}

float sample(const RawImage& image, std::uint32_t x, std::uint32_t y) {
    const auto& metadata = image.metadata();
    const auto site = site_index(metadata, x, y);
    const auto raw = image.samples()[static_cast<std::size_t>(y) *
                                     metadata.row_stride_samples + x];
    const float v = (static_cast<float>(raw) - metadata.black_levels[site]) /
                    (metadata.white_levels[site] - metadata.black_levels[site]);
    return v; // Preserve values below black and above white until an explicit output operation.
}

RawMetadata uniform_metadata(std::uint32_t width, std::uint32_t height,
                             BayerPattern pattern, std::uint16_t black,
                             std::uint16_t white) {
    RawMetadata metadata;
    metadata.width = width;
    metadata.height = height;
    metadata.pattern = pattern;
    metadata.black_levels.fill(black);
    metadata.white_levels.fill(white);
    return metadata;
}

void valid_gain(float gain) {
    if (!std::isfinite(gain) || gain <= 0.0f || gain > 65536.0f)
        throw std::invalid_argument("white balance gains must be finite and in (0, 65536]");
}

using Matrix3 = std::array<double, 9>;

Matrix3 multiply(const Matrix3& a, const Matrix3& b) {
    Matrix3 out{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            for (int k = 0; k < 3; ++k)
                out[row * 3 + col] += a[row * 3 + k] * b[k * 3 + col];
    return out;
}

std::array<double, 3> multiply(const Matrix3& a, const std::array<double, 3>& v) {
    return {a[0] * v[0] + a[1] * v[1] + a[2] * v[2],
            a[3] * v[0] + a[4] * v[1] + a[5] * v[2],
            a[6] * v[0] + a[7] * v[1] + a[8] * v[2]};
}

Matrix3 inverse(const Matrix3& m) {
    const double determinant =
        m[0] * (m[4] * m[8] - m[5] * m[7]) -
        m[1] * (m[3] * m[8] - m[5] * m[6]) +
        m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12)
        throw std::invalid_argument("color matrix is singular");
    const double d = 1.0 / determinant;
    return {(m[4] * m[8] - m[5] * m[7]) * d,
            (m[2] * m[7] - m[1] * m[8]) * d,
            (m[1] * m[5] - m[2] * m[4]) * d,
            (m[5] * m[6] - m[3] * m[8]) * d,
            (m[0] * m[8] - m[2] * m[6]) * d,
            (m[2] * m[3] - m[0] * m[5]) * d,
            (m[3] * m[7] - m[4] * m[6]) * d,
            (m[1] * m[6] - m[0] * m[7]) * d,
            (m[0] * m[4] - m[1] * m[3]) * d};
}

std::array<double, 3> xyz_white(double x, double y) {
    return {x / y, 1.0, (1.0 - x - y) / y};
}

Matrix3 rgb_to_xyz(const std::array<double, 6>& xy, double wx, double wy) {
    Matrix3 basis{};
    for (int col = 0; col < 3; ++col) {
        const double x = xy[2 * col], y = xy[2 * col + 1];
        basis[col] = x / y;
        basis[3 + col] = 1.0;
        basis[6 + col] = (1.0 - x - y) / y;
    }
    const auto scales = multiply(inverse(basis), xyz_white(wx, wy));
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            basis[row * 3 + col] *= scales[col];
    return basis;
}

Matrix3 d50_to_d65() {
    constexpr Matrix3 bradford{0.8951, 0.2664, -0.1614,
                               -0.7502, 1.7135, 0.0367,
                               0.0389, -0.0685, 1.0296};
    const auto d50 = multiply(bradford, xyz_white(0.3457, 0.3585));
    const auto d65 = multiply(bradford, xyz_white(0.3127, 0.3290));
    const Matrix3 scale{d65[0] / d50[0], 0, 0,
                        0, d65[1] / d50[1], 0,
                        0, 0, d65[2] / d50[2]};
    return multiply(inverse(bradford), multiply(scale, bradford));
}

Matrix3 xyz_d50_to_working(WorkingSpace target) {
    // Chromaticities from ICC ROMM RGB and ITU-R BT.2020. All operations here
    // are linear; neither standard's nonlinear encoding is used.
    switch (target) {
    case WorkingSpace::LinearProPhotoD50:
        return inverse(rgb_to_xyz({0.7347, 0.2653, 0.1596, 0.8404,
                                   0.0366, 0.0001}, 0.3457, 0.3585));
    case WorkingSpace::LinearRec2020D65:
        return multiply(inverse(rgb_to_xyz({0.708, 0.292, 0.170, 0.797,
                                            0.131, 0.046}, 0.3127, 0.3290)),
                        d50_to_d65());
    }
    throw std::invalid_argument("unknown working space");
}

Matrix3 working_to_linear_srgb(WorkingSpace source) {
    // sRGB D65 primaries and the conversion order follow W3C CSS Color 4.
    const auto srgb_to_xyz_d65 = rgb_to_xyz({0.640, 0.330, 0.300, 0.600,
                                             0.150, 0.060}, 0.3127, 0.3290);
    switch (source) {
    case WorkingSpace::LinearProPhotoD50:
        return multiply(inverse(srgb_to_xyz_d65),
                        multiply(d50_to_d65(),
                                 rgb_to_xyz({0.7347, 0.2653, 0.1596, 0.8404,
                                             0.0366, 0.0001}, 0.3457, 0.3585)));
    case WorkingSpace::LinearRec2020D65:
        return multiply(inverse(srgb_to_xyz_d65),
                        rgb_to_xyz({0.708, 0.292, 0.170, 0.797,
                                    0.131, 0.046}, 0.3127, 0.3290));
    }
    throw std::invalid_argument("unknown working space");
}

Matrix3 working_to_working(WorkingSpace source, WorkingSpace target) {
    return multiply(xyz_d50_to_working(target),
                    inverse(xyz_d50_to_working(source)));
}

} // namespace

RawImage::RawImage(RawMetadata metadata, std::vector<std::uint16_t> samples)
    : metadata_(std::move(metadata)),
      bayer_(std::make_shared<const std::vector<std::uint16_t>>(std::move(samples))) {
    if (!metadata_.width || !metadata_.height ||
        static_cast<int>(metadata_.pattern) < 0 || static_cast<int>(metadata_.pattern) > 3 ||
        metadata_.cfa_phase_x > 1 || metadata_.cfa_phase_y > 1)
        throw std::invalid_argument("invalid RAW dimensions, Bayer pattern, or CFA phase");
    if (metadata_.row_stride_samples == 0)
        metadata_.row_stride_samples = metadata_.width;
    if (metadata_.row_stride_samples < metadata_.width)
        throw std::invalid_argument("RAW row stride is smaller than width");
    const Rect empty{};
    if (metadata_.active_area.x == empty.x && metadata_.active_area.y == empty.y &&
        metadata_.active_area.width == empty.width && metadata_.active_area.height == empty.height)
        metadata_.active_area = {0, 0, metadata_.width, metadata_.height};
    const auto area = metadata_.active_area;
    if (!area.width || !area.height ||
        static_cast<std::uint64_t>(area.x) + area.width > metadata_.width ||
        static_cast<std::uint64_t>(area.y) + area.height > metadata_.height)
        throw std::invalid_argument("RAW active area is outside the sensor");
    for (std::size_t site = 0; site < 4; ++site)
        if (metadata_.white_levels[site] <= metadata_.black_levels[site])
            throw std::invalid_argument("RAW white level must exceed black level at every site");
    if (metadata_.height > std::vector<std::uint16_t>().max_size() /
                           metadata_.row_stride_samples ||
        bayer_->size() != static_cast<std::size_t>(metadata_.row_stride_samples) * metadata_.height)
        throw std::invalid_argument("Bayer sample count does not match stride and height");
}

RawImage::RawImage(std::uint32_t width, std::uint32_t height,
                   std::vector<std::uint16_t> samples, BayerPattern pattern,
                   std::uint16_t black, std::uint16_t white)
    : RawImage(uniform_metadata(width, height, pattern, black, white),
               std::move(samples)) {}

RasterImage::RasterImage(RasterMetadata metadata, std::vector<float> pixels)
    : metadata_(metadata),
      pixels_(std::make_shared<const std::vector<float>>(std::move(pixels))) {
    if (!metadata_.width || !metadata_.height ||
        (metadata_.working_space != WorkingSpace::LinearProPhotoD50 &&
         metadata_.working_space != WorkingSpace::LinearRec2020D65))
        throw std::invalid_argument("invalid scene-linear raster metadata");
    if (!metadata_.row_stride_pixels)
        metadata_.row_stride_pixels = metadata_.width;
    if (metadata_.row_stride_pixels < metadata_.width ||
        pixels_->size() != checked_elements(metadata_.row_stride_pixels,
                                            metadata_.height, 3))
        throw std::invalid_argument("raster float count does not match stride and height");
    if (!std::all_of(pixels_->begin(), pixels_->end(),
                     [](float value) { return std::isfinite(value); }))
        throw std::invalid_argument("scene-linear raster pixels must be finite");
}

RawUnpackNode::RawUnpackNode(RawImage image) : image_(std::move(image)) {}

Rect RawUnpackNode::input_region(Rect output, Rect source_bounds) const {
    const auto area = image_.metadata().active_area;
    if (source_bounds.x != area.x || source_bounds.y != area.y ||
        source_bounds.width != area.width || source_bounds.height != area.height)
        throw std::invalid_argument("RAW source bounds differ from active area");
    return expand_rect(output, area, 1);
}

Tile RawUnpackNode::render(Rect r) const {
    validate_rect(image_, r);
    const auto& metadata = image_.metadata();
    const auto area = metadata.active_area;
    Tile tile{r, std::vector<float>(checked_elements(r.width, r.height, 3))};
    // A simple bilinear demosaic. Neighbors come directly from the owned Bayer
    // source, so requesting a tile needs no separately allocated halo tile.
#ifdef _OPENMP
#pragma omp parallel for if(static_cast<std::uint64_t>(r.width) * r.height >= 65536)
#endif
    for (std::int64_t row = 0; row < r.height; ++row) {
        const auto y = r.y + static_cast<std::uint32_t>(row);
        for (std::uint32_t col = 0; col < r.width; ++col) {
            const auto x = r.x + col;
            const auto base = (static_cast<std::size_t>(row) * r.width + col) * 3;
            for (int channel = 0; channel < 3; ++channel) {
                if (cfa_color(metadata, x, y) == channel) {
                    tile.rgb[base + channel] = sample(image_, x, y);
                    continue;
                }
                float sum = 0.0f;
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    const auto yy = static_cast<std::int64_t>(y) + dy;
                    if (yy < area.y || yy >= static_cast<std::int64_t>(area.y) + area.height)
                        continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        const auto xx = static_cast<std::int64_t>(x) + dx;
                        if (xx < area.x || xx >= static_cast<std::int64_t>(area.x) + area.width)
                            continue;
                        if (cfa_color(metadata, static_cast<std::uint32_t>(xx),
                                      static_cast<std::uint32_t>(yy)) == channel) {
                            sum += sample(image_, static_cast<std::uint32_t>(xx),
                                          static_cast<std::uint32_t>(yy));
                            ++count;
                        }
                    }
                }
                tile.rgb[base + channel] = count ? sum / count : 0.0f;
            }
        }
    }
    return tile;
}

RasterSourceNode::RasterSourceNode(RasterImage image) : image_(std::move(image)) {}

Tile RasterSourceNode::render(Rect r) const {
    validate_rect({0, 0, image_.width(), image_.height()}, r);
    Tile tile{r, std::vector<float>(checked_elements(r.width, r.height, 3)),
              output_descriptor()};
    if (!r.width || !r.height) return tile;
    const auto stride = image_.metadata().row_stride_pixels;
    for (std::uint32_t row = 0; row < r.height; ++row) {
        const auto source = (static_cast<std::size_t>(r.y + row) * stride + r.x) * 3;
        const auto target = static_cast<std::size_t>(row) * r.width * 3;
        std::memcpy(tile.rgb.data() + target, image_.pixels().data() + source,
                    static_cast<std::size_t>(r.width) * 3 * sizeof(float));
    }
    return tile;
}

Tile RasterSourceNode::render_level(Rect r, RenderLevel level) const {
    if (level.mip == 0 &&
        (level.quality == RenderQuality::Final ||
         level.quality == RenderQuality::Preview))
        return render(r);
    if (level.mip != 1 || level.quality != RenderQuality::Preview)
        throw std::invalid_argument("raster source does not support this render level");
    // Output pixel (x,y) has nominal source-center (2x+0.5, 2y+0.5).
    // Average its 2x2 source footprint in scene-linear light; at odd edges,
    // use only present samples and divide by their actual count.
    const Rect reduced_bounds{0, 0,
        image_.width() / 2 + image_.width() % 2,
        image_.height() / 2 + image_.height() % 2};
    validate_rect(reduced_bounds, r);
    Tile tile{r, std::vector<float>(checked_elements(r.width, r.height, 3)),
              output_descriptor()};
    const auto stride = image_.metadata().row_stride_pixels;
    const auto& pixels = image_.pixels();
    for (std::uint32_t row = 0; row < r.height; ++row) {
        const auto source_y = static_cast<std::uint64_t>(r.y + row) * 2;
        const auto end_y = std::min<std::uint64_t>(source_y + 2, image_.height());
        for (std::uint32_t column = 0; column < r.width; ++column) {
            const auto source_x = static_cast<std::uint64_t>(r.x + column) * 2;
            const auto end_x = std::min<std::uint64_t>(source_x + 2, image_.width());
            double sums[3]{};
            for (auto y = source_y; y < end_y; ++y) {
                for (auto x = source_x; x < end_x; ++x) {
                    const auto offset = (static_cast<std::size_t>(y) * stride +
                                         static_cast<std::size_t>(x)) * 3;
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        sums[channel] += pixels[offset + channel];
                }
            }
            const auto count = static_cast<double>((end_x - source_x) *
                                                   (end_y - source_y));
            const auto target = (static_cast<std::size_t>(row) * r.width + column) * 3;
            for (std::size_t channel = 0; channel < 3; ++channel)
                tile.rgb[target + channel] = static_cast<float>(sums[channel] / count);
        }
    }
    return tile;
}

WhiteBalanceNode::WhiteBalanceNode(std::shared_ptr<const Node> input,
                                   float red, float green, float blue)
    : input_(std::move(input)), gains_{red, green, blue} {
    if (!input_) throw std::invalid_argument("white balance input is null");
    if (input_->output_descriptor() != ImageDescriptor::camera_linear())
        throw std::invalid_argument("white balance requires camera-linear RGB input");
    for (float gain : gains_) valid_gain(gain);
}

Tile WhiteBalanceNode::render(Rect r) const {
    Tile tile = input_->render(r);
    validate_tile(tile, r, input_->output_descriptor());
    const auto pixels = tile.rgb.size() / 3;
#ifdef _OPENMP
#pragma omp parallel for if(pixels >= 65536)
#endif
    for (std::int64_t i = 0; i < static_cast<std::int64_t>(pixels); ++i) {
        const auto offset = static_cast<std::size_t>(i) * 3;
        tile.rgb[offset] *= gains_[0];
        tile.rgb[offset + 1] *= gains_[1];
        tile.rgb[offset + 2] *= gains_[2];
    }
    return tile;
}

ExposureNode::ExposureNode(std::shared_ptr<const Node> input, float stops)
    : input_(std::move(input)), multiplier_(std::exp2(stops)) {
    if (!input_ || !std::isfinite(stops) || stops < -32.0f || stops > 32.0f)
        throw std::invalid_argument("exposure stops must be finite and in [-32, 32]");
    descriptor_ = input_->output_descriptor();
    if (descriptor_ != ImageDescriptor::camera_linear() &&
        descriptor_ != ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50) &&
        descriptor_ != ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65))
        throw std::invalid_argument("exposure requires linear RGB input");
}

Tile ExposureNode::render(Rect r) const {
    return render_level(r, {});
}

bool ExposureNode::supports_level(RenderLevel level) const noexcept {
    if (level.mip == 0 && level.quality == RenderQuality::Final) return true;
    return level.mip == 1 && level.quality == RenderQuality::Preview &&
           descriptor_.domain == PixelDomain::SceneLinearRGB &&
           input_->supports_level(level);
}

Tile ExposureNode::render_level(Rect r, RenderLevel level) const {
    if (!supports_level(level))
        throw std::invalid_argument("exposure does not support this render level");
    Tile tile = input_->render_level(r, level);
    validate_tile(tile, r, input_->output_descriptor());
    const auto count = tile.rgb.size();
#ifdef _OPENMP
#pragma omp parallel for if(count >= 196608)
#endif
    for (std::int64_t i = 0; i < static_cast<std::int64_t>(count); ++i)
        tile.rgb[static_cast<std::size_t>(i)] *= multiplier_;
    return tile;
}

CameraToWorkingNode::CameraToWorkingNode(std::shared_ptr<const Node> input,
                                         CameraColorTransform transform)
    : input_(std::move(input)), descriptor_(ImageDescriptor::scene_linear(transform.target)) {
    if (!input_ || input_->output_descriptor() != ImageDescriptor::camera_linear())
        throw std::invalid_argument("color transform requires camera-linear RGB input");
    for (double value : transform.camera_to_xyz_d50)
        if (!std::isfinite(value))
            throw std::invalid_argument("camera-to-XYZ matrix must be finite");
    // Check the supplied calibration matrix before composing it with the
    // target conversion. A collapsed camera axis cannot be recovered.
    (void)inverse(transform.camera_to_xyz_d50);
    const auto composed = multiply(xyz_d50_to_working(transform.target),
                                   transform.camera_to_xyz_d50);
    for (std::size_t i = 0; i < matrix_.size(); ++i) {
        if (!std::isfinite(composed[i]) ||
            std::abs(composed[i]) > std::numeric_limits<float>::max())
            throw std::invalid_argument("composed color matrix exceeds float32 range");
        matrix_[i] = static_cast<float>(composed[i]);
    }
}

Tile CameraToWorkingNode::render(Rect r) const {
    Tile tile = input_->render(r);
    validate_tile(tile, r, input_->output_descriptor());
    const auto pixels = tile.rgb.size() / 3;
#ifdef _OPENMP
#pragma omp parallel for if(pixels >= 65536)
#endif
    for (std::int64_t i = 0; i < static_cast<std::int64_t>(pixels); ++i) {
        const auto base = static_cast<std::size_t>(i) * 3;
        const float red = tile.rgb[base], green = tile.rgb[base + 1], blue = tile.rgb[base + 2];
        tile.rgb[base] = matrix_[0] * red + matrix_[1] * green + matrix_[2] * blue;
        tile.rgb[base + 1] = matrix_[3] * red + matrix_[4] * green + matrix_[5] * blue;
        tile.rgb[base + 2] = matrix_[6] * red + matrix_[7] * green + matrix_[8] * blue;
    }
    tile.descriptor = descriptor_;
    return tile;
}

WorkingSpaceConvertNode::WorkingSpaceConvertNode(
    std::shared_ptr<const Node> input, WorkingSpace target)
    : input_(std::move(input)), descriptor_(ImageDescriptor::scene_linear(target)) {
    if (!input_) throw std::invalid_argument("working-space conversion input is null");
    const auto source_descriptor = input_->output_descriptor();
    WorkingSpace source;
    if (source_descriptor == ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50))
        source = WorkingSpace::LinearProPhotoD50;
    else if (source_descriptor == ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65))
        source = WorkingSpace::LinearRec2020D65;
    else
        throw std::invalid_argument("working-space conversion requires declared scene-linear RGB");
    // Validate the target even when a caller supplies an invalid enum value.
    (void)xyz_d50_to_working(target);
    identity_ = source == target;
    if (!identity_) {
        const auto matrix = working_to_working(source, target);
        for (std::size_t i = 0; i < matrix_.size(); ++i)
            matrix_[i] = static_cast<float>(matrix[i]);
    }
}

Tile WorkingSpaceConvertNode::render(Rect r) const {
    return render_level(r, {});
}

bool WorkingSpaceConvertNode::supports_level(RenderLevel level) const noexcept {
    if (level.mip == 0 && level.quality == RenderQuality::Final) return true;
    return level.mip == 1 && level.quality == RenderQuality::Preview &&
           input_->supports_level(level);
}

Tile WorkingSpaceConvertNode::render_level(Rect r, RenderLevel level) const {
    if (!supports_level(level))
        throw std::invalid_argument("working-space conversion does not support this render level");
    Tile tile = input_->render_level(r, level);
    validate_tile(tile, r, input_->output_descriptor());
    if (!identity_) {
        const auto pixels = tile.rgb.size() / 3;
#ifdef _OPENMP
#pragma omp parallel for if(pixels >= 65536)
#endif
        for (std::int64_t i = 0; i < static_cast<std::int64_t>(pixels); ++i) {
            const auto base = static_cast<std::size_t>(i) * 3;
            const float red = tile.rgb[base], green = tile.rgb[base + 1], blue = tile.rgb[base + 2];
            tile.rgb[base] = matrix_[0] * red + matrix_[1] * green + matrix_[2] * blue;
            tile.rgb[base + 1] = matrix_[3] * red + matrix_[4] * green + matrix_[5] * blue;
            tile.rgb[base + 2] = matrix_[6] * red + matrix_[7] * green + matrix_[8] * blue;
        }
    }
    tile.descriptor = descriptor_;
    return tile;
}

WorkingToSrgbNode::WorkingToSrgbNode(std::shared_ptr<const Node> input)
    : input_(std::move(input)) {
    if (!input_) throw std::invalid_argument("working-to-sRGB input is null");
    const auto descriptor = input_->output_descriptor();
    WorkingSpace source;
    if (descriptor == ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50))
        source = WorkingSpace::LinearProPhotoD50;
    else if (descriptor == ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65))
        source = WorkingSpace::LinearRec2020D65;
    else
        throw std::invalid_argument("sRGB conversion requires declared scene-linear working RGB");
    const auto matrix = working_to_linear_srgb(source);
    for (std::size_t i = 0; i < matrix_.size(); ++i)
        matrix_[i] = static_cast<float>(matrix[i]);
}

Tile WorkingToSrgbNode::render(Rect r) const {
    Tile tile = input_->render(r);
    validate_tile(tile, r, input_->output_descriptor());
    const auto pixels = tile.rgb.size() / 3;
#ifdef _OPENMP
#pragma omp parallel for if(pixels >= 65536)
#endif
    for (std::int64_t i = 0; i < static_cast<std::int64_t>(pixels); ++i) {
        const auto base = static_cast<std::size_t>(i) * 3;
        const float red = tile.rgb[base], green = tile.rgb[base + 1], blue = tile.rgb[base + 2];
        tile.rgb[base] = matrix_[0] * red + matrix_[1] * green + matrix_[2] * blue;
        tile.rgb[base + 1] = matrix_[3] * red + matrix_[4] * green + matrix_[5] * blue;
        tile.rgb[base + 2] = matrix_[6] * red + matrix_[7] * green + matrix_[8] * blue;
    }
    tile.descriptor = output_descriptor();
    return tile;
}

ToneCurveNode::ToneCurveNode(std::shared_ptr<const Node> input,
                             float shoulder, float gamma)
    : input_(std::move(input)), shoulder_(shoulder), inverse_gamma_(1.0f / gamma) {
    if (!input_ || !std::isfinite(shoulder) || shoulder <= 0.0f ||
        !std::isfinite(gamma) || gamma <= 0.0f || !std::isfinite(inverse_gamma_))
        throw std::invalid_argument("tone shoulder and gamma must be finite and positive");
    const auto source = input_->output_descriptor();
    if (source != ImageDescriptor::camera_linear() &&
        source != ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50) &&
        source != ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65) &&
        source != ImageDescriptor::linear_srgb())
        throw std::invalid_argument("tone curve requires linear RGB input");
    if (source == ImageDescriptor::linear_srgb())
        descriptor_ = ImageDescriptor::display_linear_srgb();
    else {
        descriptor_ = ImageDescriptor::tone_mapped();
        descriptor_.primaries = source.primaries;
        descriptor_.white_point = source.white_point;
    }
}

Tile ToneCurveNode::render(Rect r) const {
    Tile tile = input_->render(r);
    validate_tile(tile, r, input_->output_descriptor());
    const auto count = tile.rgb.size();
#ifdef _OPENMP
#pragma omp parallel for if(count >= 196608)
#endif
    for (std::int64_t i = 0; i < static_cast<std::int64_t>(count); ++i) {
        const auto index = static_cast<std::size_t>(i);
        const float linear = tile.rgb[index];
        const float magnitude = std::abs(linear);
        const float compressed = magnitude / (magnitude + shoulder_);
        const float mapped = inverse_gamma_ == 1.0f
                                 ? compressed : std::pow(compressed, inverse_gamma_);
        tile.rgb[index] = std::copysign(mapped, linear);
    }
    tile.descriptor = output_descriptor();
    return tile;
}

BoxBlurNode::BoxBlurNode(std::shared_ptr<const Node> input,
                         Rect source_bounds, std::uint32_t radius)
    : input_(std::move(input)), source_bounds_(source_bounds), radius_(radius) {
    if (!input_ || !source_bounds.width || !source_bounds.height ||
        static_cast<std::uint64_t>(source_bounds.x) + source_bounds.width >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1 ||
        static_cast<std::uint64_t>(source_bounds.y) + source_bounds.height >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1 ||
        radius < 1 || radius > 8)
        throw std::invalid_argument("box blur needs bounded input and radius 1..8");
    descriptor_ = input_->output_descriptor();
    if (descriptor_ != ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50) &&
        descriptor_ != ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65))
        throw std::invalid_argument("box blur requires scene-linear working RGB");
}

Rect BoxBlurNode::input_region(Rect output, Rect source_bounds) const {
    if (source_bounds.x != source_bounds_.x || source_bounds.y != source_bounds_.y ||
        source_bounds.width != source_bounds_.width ||
        source_bounds.height != source_bounds_.height)
        throw std::invalid_argument("box blur source bounds differ from construction");
    return expand_rect(output, source_bounds_, radius_);
}

Tile BoxBlurNode::render(Rect r) const {
    return render_level(r, {});
}

bool BoxBlurNode::supports_level(RenderLevel level) const noexcept {
    if (level.mip == 0 && level.quality == RenderQuality::Final) return true;
    return level.mip == 1 && level.quality == RenderQuality::Preview &&
           source_bounds_.x == 0 && source_bounds_.y == 0 &&
           input_->supports_level(level);
}

Rect BoxBlurNode::input_region_level(Rect output, Rect source_bounds,
                                      RenderLevel level) const {
    if (level.mip == 0 && level.quality == RenderQuality::Final)
        return input_region(output, source_bounds);
    if (!supports_level(level))
        throw std::invalid_argument("box blur has no mapping for this render level");
    const Rect expected{0, 0,
        source_bounds_.width / 2 + source_bounds_.width % 2,
        source_bounds_.height / 2 + source_bounds_.height % 2};
    if (source_bounds.x != expected.x || source_bounds.y != expected.y ||
        source_bounds.width != expected.width ||
        source_bounds.height != expected.height)
        throw std::invalid_argument("reduced box blur bounds differ from construction");
    return expand_rect(output, expected, radius_);
}

Tile BoxBlurNode::render_level(Rect r, RenderLevel level) const {
    if (!supports_level(level))
        throw std::invalid_argument("box blur does not support this render level");
    const Rect level_bounds = level.mip == 0
        ? source_bounds_
        : Rect{0, 0, source_bounds_.width / 2 + source_bounds_.width % 2,
                      source_bounds_.height / 2 + source_bounds_.height % 2};
    validate_rect(level_bounds, r);
    Tile output{r, std::vector<float>(checked_elements(r.width, r.height, 3)), descriptor_};
    if (!r.width || !r.height) return output;
    const Rect needed = input_region_level(r, level_bounds, level);
    Tile input = input_->render_level(needed, level);
    validate_tile(input, needed, descriptor_);
    for (std::uint32_t row = 0; row < r.height; ++row) {
        const auto y = static_cast<std::int64_t>(r.y) + row;
        const auto top = std::max<std::int64_t>(needed.y, y - radius_);
        const auto bottom = std::min<std::int64_t>(
            static_cast<std::int64_t>(needed.y) + needed.height - 1, y + radius_);
        for (std::uint32_t col = 0; col < r.width; ++col) {
            const auto x = static_cast<std::int64_t>(r.x) + col;
            const auto left = std::max<std::int64_t>(needed.x, x - radius_);
            const auto right = std::min<std::int64_t>(
                static_cast<std::int64_t>(needed.x) + needed.width - 1, x + radius_);
            double sum[3]{};
            for (auto yy = top; yy <= bottom; ++yy)
                for (auto xx = left; xx <= right; ++xx) {
                    const auto i = (static_cast<std::size_t>(yy - needed.y) * needed.width +
                                    static_cast<std::size_t>(xx - needed.x)) * 3;
                    for (int channel = 0; channel < 3; ++channel)
                        sum[channel] += input.rgb[i + channel];
                }
            const auto count = static_cast<double>((bottom - top + 1) * (right - left + 1));
            const auto target = (static_cast<std::size_t>(row) * r.width + col) * 3;
            for (int channel = 0; channel < 3; ++channel)
                output.rgb[target + channel] = static_cast<float>(sum[channel] / count);
        }
    }
    return output;
}

OutputClipNode::OutputClipNode(std::shared_ptr<const Node> input)
    : input_(std::move(input)) {
    if (!input_ || input_->output_descriptor().domain != PixelDomain::ToneMappedUnmanagedRGB)
        throw std::invalid_argument("output clip requires tone-mapped RGB input");
    descriptor_ = input_->output_descriptor();
    descriptor_.domain = PixelDomain::BoundedUnmanagedRGB;
}

Tile OutputClipNode::render(Rect r) const {
    Tile tile = input_->render(r);
    validate_tile(tile, r, input_->output_descriptor());
    for (float& value : tile.rgb) {
        if (!std::isfinite(value))
            throw std::domain_error("non-finite value at output boundary");
        value = std::clamp(value, 0.0f, 1.0f);
    }
    tile.descriptor = output_descriptor();
    return tile;
}

SrgbEncodeNode::SrgbEncodeNode(std::shared_ptr<const Node> input)
    : input_(std::move(input)) {
    if (!input_) throw std::invalid_argument("sRGB encode input is null");
    const auto descriptor = input_->output_descriptor();
    if (descriptor != ImageDescriptor::display_linear_srgb())
        throw std::invalid_argument("sRGB encode requires display-linear sRGB input");
}

Tile SrgbEncodeNode::render(Rect r) const {
    Tile tile = input_->render(r);
    validate_tile(tile, r, input_->output_descriptor());
    if (!std::all_of(tile.rgb.begin(), tile.rgb.end(),
                     [](float value) { return std::isfinite(value); }))
        throw std::domain_error("non-finite value at sRGB output boundary");
    const auto count = tile.rgb.size();
#ifdef _OPENMP
#pragma omp parallel for if(count >= 196608)
#endif
    for (std::int64_t i = 0; i < static_cast<std::int64_t>(count); ++i) {
        const auto index = static_cast<std::size_t>(i);
        const float value = tile.rgb[index];
        const float linear = std::clamp(value, 0.0f, 1.0f);
        // IEC sRGB component encoding as documented by W3C CSS Color 4.
        tile.rgb[index] = linear <= 0.0031308f
                              ? 12.92f * linear
                              : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    }
    tile.descriptor = output_descriptor();
    return tile;
}

IccDisplayNode::IccDisplayNode(
    std::shared_ptr<const Node> input,
    std::shared_ptr<const IccDisplayTransform> transform)
    : input_(std::move(input)), transform_(std::move(transform)) {
    if (!input_ || !transform_ ||
        input_->output_descriptor() != ImageDescriptor::display_linear_srgb())
        throw std::invalid_argument("ICC display requires display-linear sRGB and a transform");
    const auto digest = transform_->profile_sha256();
    if (std::all_of(digest.begin(), digest.end(), [](std::uint8_t v) { return v == 0; }))
        throw std::invalid_argument("ICC output profile digest must be nonzero");
    descriptor_ = ImageDescriptor::icc_display(digest);
}

Tile IccDisplayNode::render(Rect r) const {
    Tile tile = input_->render(r);
    validate_tile(tile, r, input_->output_descriptor());
    if (!std::all_of(tile.rgb.begin(), tile.rgb.end(),
                     [](float value) { return std::isfinite(value); }))
        throw std::domain_error("non-finite value at ICC input boundary");
    if (!tile.rgb.empty()) transform_->apply(tile.rgb.data(), tile.rgb.size() / 3);
    if (!std::all_of(tile.rgb.begin(), tile.rgb.end(),
                     [](float value) { return std::isfinite(value) && value >= 0.0f && value <= 1.0f; }))
        throw std::domain_error("ICC display transform returned unbounded or non-finite RGB");
    tile.descriptor = descriptor_;
    return tile;
}

ImageGraph::ImageGraph(RawImage image, GraphRecipe recipe,
                       std::shared_ptr<const IccDisplayTransform> display_transform)
    : raw_image_(std::move(image)),
      source_bounds_(raw_image_->metadata().active_area), recipe_(recipe) {
    auto node = std::make_shared<RawUnpackNode>(*raw_image_);
    auto wb = std::make_shared<WhiteBalanceNode>(node, recipe.red_gain,
                                                 recipe.green_gain, recipe.blue_gain);
    auto exposure = std::make_shared<ExposureNode>(wb, recipe.exposure_stops);
    std::shared_ptr<const Node> linear = exposure;
    if (recipe.camera_color)
        linear = std::make_shared<CameraToWorkingNode>(linear, *recipe.camera_color);
    if (recipe.output_mode == OutputMode::SrgbPreview ||
        recipe.output_mode == OutputMode::IccDisplay)
        linear = std::make_shared<WorkingToSrgbNode>(linear);
    else if (recipe.output_mode != OutputMode::LegacyBounded)
        throw std::invalid_argument("unknown output mode");
    auto tone = std::make_shared<ToneCurveNode>(linear, recipe.tone_shoulder,
                                                recipe.tone_gamma);
    if (recipe.output_mode == OutputMode::IccDisplay)
        output_ = std::make_shared<IccDisplayNode>(tone, std::move(display_transform));
    else {
        if (display_transform)
            throw std::invalid_argument("ICC transform requires ICC display output mode");
        output_ = recipe.output_mode == OutputMode::SrgbPreview
                      ? std::static_pointer_cast<const Node>(std::make_shared<SrgbEncodeNode>(tone))
                      : std::static_pointer_cast<const Node>(std::make_shared<OutputClipNode>(tone));
    }
}

ImageGraph::ImageGraph(RasterImage image, GraphRecipe recipe,
                       std::shared_ptr<const IccDisplayTransform> display_transform)
    : ImageGraph(std::make_shared<RasterSourceNode>(image),
                 Rect{0, 0, image.width(), image.height()}, recipe,
                 std::move(display_transform)) {}

ImageGraph::ImageGraph(std::shared_ptr<const Node> source, Rect bounds,
                       GraphRecipe recipe,
                       std::shared_ptr<const IccDisplayTransform> display_transform)
    : source_bounds_(bounds), recipe_(recipe) {
    if (!source || !bounds.width || !bounds.height ||
        static_cast<std::uint64_t>(bounds.x) + bounds.width >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1 ||
        static_cast<std::uint64_t>(bounds.y) + bounds.height >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1)
        throw std::invalid_argument("scene-linear source and valid bounds are required");
    const auto descriptor = source->output_descriptor();
    if (descriptor != ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50) &&
        descriptor != ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65))
        throw std::invalid_argument("graph source must output a supported scene-linear space");
    if (recipe.red_gain != 1.0f || recipe.green_gain != 1.0f ||
        recipe.blue_gain != 1.0f || recipe.camera_color)
        throw std::invalid_argument("raster input does not accept RAW calibration controls");
    std::shared_ptr<const Node> linear =
        std::make_shared<ExposureNode>(source, recipe.exposure_stops);
    if (recipe.output_mode == OutputMode::SrgbPreview ||
        recipe.output_mode == OutputMode::IccDisplay)
        linear = std::make_shared<WorkingToSrgbNode>(linear);
    else if (recipe.output_mode != OutputMode::LegacyBounded)
        throw std::invalid_argument("unknown output mode");
    auto tone = std::make_shared<ToneCurveNode>(linear, recipe.tone_shoulder,
                                                recipe.tone_gamma);
    if (recipe.output_mode == OutputMode::IccDisplay)
        output_ = std::make_shared<IccDisplayNode>(tone, std::move(display_transform));
    else {
        if (display_transform)
            throw std::invalid_argument("ICC transform requires ICC display output mode");
        output_ = recipe.output_mode == OutputMode::SrgbPreview
                      ? std::static_pointer_cast<const Node>(std::make_shared<SrgbEncodeNode>(tone))
                      : std::static_pointer_cast<const Node>(std::make_shared<OutputClipNode>(tone));
    }
}

const RawImage& ImageGraph::image() const {
    if (!raw_image_)
        throw std::logic_error("graph has a raster source, not a RAW source");
    return *raw_image_;
}

void Renderer::render_tiles(const ImageGraph& graph, RenderRequest request,
                            const TileCallback& callback,
                            const CancellationToken* cancellation) const {
    render_tiles(graph.output(), graph.source_bounds(), request, callback, cancellation);
}

void Renderer::render_tiles(const Node& output, Rect source_bounds, RenderRequest request,
                            const TileCallback& callback,
                            const CancellationToken* cancellation) const {
    validate_rect(request_bounds(source_bounds, request.level), request.viewport);
    if (!output.supports_level(request.level))
        throw std::invalid_argument("node does not support this render level");
    if (!callback || !request.tile_size)
        throw std::invalid_argument("callback and tile size are required");
    if (cancellation && cancellation->is_cancelled()) throw RenderCancelled();
    const auto right = static_cast<std::uint64_t>(request.viewport.x) +
                       request.viewport.width;
    const auto bottom = static_cast<std::uint64_t>(request.viewport.y) +
                        request.viewport.height;
    for (std::uint64_t y = request.viewport.y; y < bottom; y += request.tile_size) {
        for (std::uint64_t x = request.viewport.x; x < right; x += request.tile_size) {
            if (cancellation && cancellation->is_cancelled()) throw RenderCancelled();
            Rect r{static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                   static_cast<std::uint32_t>(std::min<std::uint64_t>(request.tile_size, right - x)),
                   static_cast<std::uint32_t>(std::min<std::uint64_t>(request.tile_size, bottom - y))};
            callback(output.render_level(r, request.level));
        }
    }
}

void Renderer::render_tiles(const ImageGraph& graph, Rect viewport,
                            const TileCallback& callback, std::uint32_t tile_size,
                            const CancellationToken* cancellation) const {
    render_tiles(graph.output(), graph.source_bounds(), viewport, callback, tile_size,
                 cancellation);
}

Tile Renderer::render_image(const ImageGraph& graph, RenderRequest request,
                            const CancellationToken* cancellation) const {
    return render_image(graph.output(), graph.source_bounds(), request, cancellation);
}

Tile Renderer::render_image(const Node& output, Rect source_bounds, RenderRequest request,
                            const CancellationToken* cancellation) const {
    validate_rect(request_bounds(source_bounds, request.level), request.viewport);
    if (!output.supports_level(request.level))
        throw std::invalid_argument("node does not support this render level");
    if (cancellation && cancellation->is_cancelled()) throw RenderCancelled();
    Tile image{request.viewport,
               std::vector<float>(checked_elements(request.viewport.width,
                                                   request.viewport.height, 3)),
               output.output_descriptor()};
    render_tiles(output, source_bounds, request, [&](const Tile& tile) {
        validate_tile(tile, tile.bounds, image.descriptor);
        for (std::uint32_t row = 0; row < tile.bounds.height; ++row) {
            const auto src = static_cast<std::size_t>(row) * tile.bounds.width * 3;
            const auto dst = (static_cast<std::size_t>(tile.bounds.y - request.viewport.y + row) *
                              request.viewport.width + tile.bounds.x - request.viewport.x) * 3;
            std::memcpy(image.rgb.data() + dst, tile.rgb.data() + src,
                        static_cast<std::size_t>(tile.bounds.width) * 3 * sizeof(float));
        }
    }, cancellation);
    return image;
}

void Renderer::render_tiles(const Node& output, Rect source_bounds, Rect viewport,
                            const TileCallback& callback, std::uint32_t tile_size,
                            const CancellationToken* cancellation) const {
    render_tiles(output, source_bounds, RenderRequest{viewport, tile_size, {}},
                 callback, cancellation);
}

Tile Renderer::render_image(const ImageGraph& graph, Rect viewport,
                            std::uint32_t tile_size,
                            const CancellationToken* cancellation) const {
    return render_image(graph.output(), graph.source_bounds(), viewport, tile_size,
                        cancellation);
}

Tile Renderer::render_image(const Node& node, Rect source_bounds, Rect viewport,
                            std::uint32_t tile_size,
                            const CancellationToken* cancellation) const {
    return render_image(node, source_bounds, RenderRequest{viewport, tile_size, {}},
                        cancellation);
}

std::vector<float> Renderer::render_roi(const ImageGraph& graph, Rect viewport,
                                        std::uint32_t tile_size,
                                        const CancellationToken* cancellation) const {
    auto output = render_image(graph, viewport, tile_size, cancellation);
    return std::move(output.rgb);
}

} // namespace rawengine

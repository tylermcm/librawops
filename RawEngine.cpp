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

void validate_rect(const RawImage& image, Rect r) {
    const auto area = image.metadata().active_area;
    if (r.x < area.x || r.y < area.y ||
        static_cast<std::uint64_t>(r.x) + r.width >
            static_cast<std::uint64_t>(area.x) + area.width ||
        static_cast<std::uint64_t>(r.y) + r.height >
            static_cast<std::uint64_t>(area.y) + area.height)
        throw std::out_of_range("viewport is outside the active RAW area");
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

RawUnpackNode::RawUnpackNode(RawImage image) : image_(std::move(image)) {}

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
    if (input_->output_descriptor() != ImageDescriptor::camera_linear())
        throw std::invalid_argument("exposure requires camera-linear RGB input");
}

Tile ExposureNode::render(Rect r) const {
    Tile tile = input_->render(r);
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

ToneCurveNode::ToneCurveNode(std::shared_ptr<const Node> input,
                             float shoulder, float gamma)
    : input_(std::move(input)), shoulder_(shoulder), inverse_gamma_(1.0f / gamma) {
    if (!input_ || !std::isfinite(shoulder) || shoulder <= 0.0f ||
        !std::isfinite(gamma) || gamma <= 0.0f || !std::isfinite(inverse_gamma_))
        throw std::invalid_argument("tone shoulder and gamma must be finite and positive");
    const auto source = input_->output_descriptor();
    if (source != ImageDescriptor::camera_linear() &&
        source != ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50) &&
        source != ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65))
        throw std::invalid_argument("tone curve requires linear RGB input");
    descriptor_ = ImageDescriptor::tone_mapped();
    descriptor_.primaries = source.primaries;
    descriptor_.white_point = source.white_point;
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

ImageGraph::ImageGraph(RawImage image, GraphRecipe recipe)
    : image_(std::move(image)), recipe_(recipe) {
    auto node = std::make_shared<RawUnpackNode>(image_);
    auto wb = std::make_shared<WhiteBalanceNode>(node, recipe.red_gain,
                                                 recipe.green_gain, recipe.blue_gain);
    auto exposure = std::make_shared<ExposureNode>(wb, recipe.exposure_stops);
    std::shared_ptr<const Node> linear = exposure;
    if (recipe.camera_color)
        linear = std::make_shared<CameraToWorkingNode>(linear, *recipe.camera_color);
    auto tone = std::make_shared<ToneCurveNode>(linear, recipe.tone_shoulder,
                                                recipe.tone_gamma);
    output_ = std::make_shared<OutputClipNode>(tone);
}

void Renderer::render_tiles(const ImageGraph& graph, Rect viewport,
                            const TileCallback& callback, std::uint32_t tile_size) const {
    validate_rect(graph.image(), viewport);
    if (!callback || !tile_size) throw std::invalid_argument("callback and tile size are required");
    const auto right = static_cast<std::uint64_t>(viewport.x) + viewport.width;
    const auto bottom = static_cast<std::uint64_t>(viewport.y) + viewport.height;
    for (std::uint64_t y = viewport.y; y < bottom; y += tile_size) {
        for (std::uint64_t x = viewport.x; x < right; x += tile_size) {
            Rect r{static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                   static_cast<std::uint32_t>(std::min<std::uint64_t>(tile_size, right - x)),
                   static_cast<std::uint32_t>(std::min<std::uint64_t>(tile_size, bottom - y))};
            Tile tile = graph.output().render(r);
            callback(tile);
        }
    }
}

Tile Renderer::render_image(const ImageGraph& graph, Rect viewport,
                            std::uint32_t tile_size) const {
    validate_rect(graph.image(), viewport);
    Tile output{viewport, std::vector<float>(checked_elements(viewport.width, viewport.height, 3)),
                graph.output().output_descriptor()};
    render_tiles(graph, viewport, [&](const Tile& tile) {
        validate_tile(tile, tile.bounds, output.descriptor);
        for (std::uint32_t row = 0; row < tile.bounds.height; ++row) {
            const auto src = static_cast<std::size_t>(row) * tile.bounds.width * 3;
            const auto dst = (static_cast<std::size_t>(tile.bounds.y - viewport.y + row) *
                              viewport.width + tile.bounds.x - viewport.x) * 3;
            std::memcpy(output.rgb.data() + dst, tile.rgb.data() + src,
                        static_cast<std::size_t>(tile.bounds.width) * 3 * sizeof(float));
        }
    }, tile_size);
    return output;
}

std::vector<float> Renderer::render_roi(const ImageGraph& graph, Rect viewport,
                                        std::uint32_t tile_size) const {
    auto output = render_image(graph, viewport, tile_size);
    return std::move(output.rgb);
}

} // namespace rawengine

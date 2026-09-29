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
    if (static_cast<std::uint64_t>(r.x) + r.width > image.width ||
        static_cast<std::uint64_t>(r.y) + r.height > image.height)
        throw std::out_of_range("viewport is outside the RAW image");
}

void validate_tile(const Tile& tile, Rect requested, ImageDescriptor expected) {
    if (tile.bounds.x != requested.x || tile.bounds.y != requested.y ||
        tile.bounds.width != requested.width || tile.bounds.height != requested.height ||
        tile.descriptor != expected ||
        tile.rgb.size() != checked_elements(requested.width, requested.height, 3))
        throw std::domain_error("upstream node returned an invalid tile");
}

int cfa_color(BayerPattern pattern, std::uint32_t x, std::uint32_t y) {
    // R=0, G=1, B=2. The four patterns are their 2x2 top-left cells.
    constexpr int colors[4][2][2] = {
        {{0, 1}, {1, 2}}, {{2, 1}, {1, 0}},
        {{1, 0}, {2, 1}}, {{1, 2}, {0, 1}}
    };
    return colors[static_cast<int>(pattern)][y & 1u][x & 1u];
}

float sample(const RawImage& image, std::uint32_t x, std::uint32_t y) {
    const auto raw = (*image.bayer)[static_cast<std::size_t>(y) * image.width + x];
    const float v = (static_cast<float>(raw) - image.black_level) /
                    (image.white_level - image.black_level);
    return v; // Preserve values below black and above white until an explicit output operation.
}

void valid_gain(float gain) {
    if (!std::isfinite(gain) || gain <= 0.0f || gain > 65536.0f)
        throw std::invalid_argument("white balance gains must be finite and in (0, 65536]");
}

} // namespace

RawImage::RawImage(std::uint32_t w, std::uint32_t h,
                   std::vector<std::uint16_t> samples, BayerPattern p,
                   std::uint16_t black, std::uint16_t white)
    : width(w), height(h), black_level(black), white_level(white), pattern(p),
      bayer(std::make_shared<const std::vector<std::uint16_t>>(std::move(samples))) {
    if (!w || !h || white <= black || static_cast<int>(p) < 0 || static_cast<int>(p) > 3)
        throw std::invalid_argument("invalid RAW dimensions, levels, or Bayer pattern");
    if (bayer->size() != checked_elements(w, h, 1))
        throw std::invalid_argument("Bayer sample count does not match dimensions");
}

RawUnpackNode::RawUnpackNode(RawImage image) : image_(std::move(image)) {}

Tile RawUnpackNode::render(Rect r) const {
    validate_rect(image_, r);
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
                if (cfa_color(image_.pattern, x, y) == channel) {
                    tile.rgb[base + channel] = sample(image_, x, y);
                    continue;
                }
                float sum = 0.0f;
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    const auto yy = static_cast<std::int64_t>(y) + dy;
                    if (yy < 0 || yy >= image_.height) continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        const auto xx = static_cast<std::int64_t>(x) + dx;
                        if (xx < 0 || xx >= image_.width) continue;
                        if (cfa_color(image_.pattern, static_cast<std::uint32_t>(xx),
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

ToneCurveNode::ToneCurveNode(std::shared_ptr<const Node> input,
                             float shoulder, float gamma)
    : input_(std::move(input)), shoulder_(shoulder), inverse_gamma_(1.0f / gamma) {
    if (!input_ || !std::isfinite(shoulder) || shoulder <= 0.0f ||
        !std::isfinite(gamma) || gamma <= 0.0f || !std::isfinite(inverse_gamma_))
        throw std::invalid_argument("tone shoulder and gamma must be finite and positive");
    if (input_->output_descriptor() != ImageDescriptor::camera_linear())
        throw std::invalid_argument("tone curve requires camera-linear RGB input");
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
    if (!input_ || input_->output_descriptor() != ImageDescriptor::tone_mapped())
        throw std::invalid_argument("output clip requires tone-mapped RGB input");
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
    auto tone = std::make_shared<ToneCurveNode>(exposure, recipe.tone_shoulder,
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

#include "CoverageSource.hpp"
#include "Sha256.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace rawengine {
namespace {

static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4 &&
              std::numeric_limits<float>::radix == 2 && std::numeric_limits<float>::digits == 24);
static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == 8 &&
              std::numeric_limits<double>::radix == 2 && std::numeric_limits<double>::digits == 53);

void valid_bounds(Rect bounds) {
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    if (!bounds.width || !bounds.height || bounds.x > maximum - bounds.width ||
        bounds.y > maximum - bounds.height)
        throw std::invalid_argument("coverage source bounds are empty or overflow");
}

std::size_t count(std::uint32_t width, std::uint32_t height) {
    const auto elements = static_cast<std::uint64_t>(width) * height;
    if (elements > std::numeric_limits<std::size_t>::max() / sizeof(float) ||
        elements > std::vector<float>{}.max_size())
        throw std::invalid_argument("coverage source storage size overflows");
    return static_cast<std::size_t>(elements);
}

void contained(Rect extent, Rect request) {
    if (!request.width || !request.height || request.x < extent.x || request.y < extent.y ||
        request.width > extent.width || request.height > extent.height ||
        request.x - extent.x > extent.width - request.width ||
        request.y - extent.y > extent.height - request.height)
        throw std::invalid_argument("coverage request is outside its level extent");
}

void hash_u32(Sha256& hash, std::uint32_t word) {
    const std::uint8_t bytes[]{static_cast<std::uint8_t>(word),
        static_cast<std::uint8_t>(word >> 8), static_cast<std::uint8_t>(word >> 16),
        static_cast<std::uint8_t>(word >> 24)};
    hash.update(bytes, sizeof(bytes));
}

} // namespace

struct CoverageImage::State {
    CoverageMetadata metadata;
    std::vector<float> samples;
    mutable std::once_flag fingerprint_once;
    mutable std::array<std::uint8_t, 32> fingerprint{};
};

CoverageImage::CoverageImage(CoverageMetadata metadata, const std::vector<float>& input) {
    valid_bounds(metadata.bounds);
    const auto width = metadata.bounds.width;
    const auto height = metadata.bounds.height;
    const auto stride = metadata.row_stride_samples ? metadata.row_stride_samples : width;
    if (stride < width || input.size() != count(stride, height))
        throw std::invalid_argument("coverage source sample count differs from stride/height");
    // Validate visible bit patterns before allocating; padding is outside the source.
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto bits = std::bit_cast<std::uint32_t>(input[static_cast<std::size_t>(y)*stride+x]);
            const auto magnitude = bits & 0x7fffffffu;
            if (magnitude > 0x3f800000u || (magnitude && (bits & 0x80000000u)))
                throw std::invalid_argument("visible coverage source values must be finite and in [0,1]");
        }
    }
    auto state = std::make_shared<State>();
    state->metadata = {metadata.bounds, width};
    state->samples.resize(count(width, height));
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto bits = std::bit_cast<std::uint32_t>(input[static_cast<std::size_t>(y)*stride+x]);
            state->samples[static_cast<std::size_t>(y)*width+x] = std::bit_cast<float>(
                (bits & 0x7fffffffu) == 0 ? 0u : bits);
        }
    }
    state_ = std::move(state);
}

const CoverageMetadata& CoverageImage::metadata() const noexcept { return state_->metadata; }
const std::vector<float>& CoverageImage::samples() const noexcept { return state_->samples; }

std::array<std::uint8_t, 32> CoverageImage::fingerprint() const {
    std::call_once(state_->fingerprint_once, [&] {
        Sha256 hash;
        constexpr char tag[] = "librawops.source.coverage.f32.v1";
        hash.update(tag, sizeof(tag)); // Includes exactly one terminating NUL.
        const auto bounds = state_->metadata.bounds;
        hash_u32(hash, bounds.x); hash_u32(hash, bounds.y);
        hash_u32(hash, bounds.width); hash_u32(hash, bounds.height);
        for (float sample : state_->samples)
            hash_u32(hash, std::bit_cast<std::uint32_t>(sample));
        state_->fingerprint = hash.finish();
    });
    return state_->fingerprint;
}

std::array<std::uint8_t, 32> fingerprint_coverage_source(const CoverageImage& image) {
    return image.fingerprint();
}

CoverageSourceNode::CoverageSourceNode(CoverageImage image) : image_(std::move(image)) {}

bool CoverageSourceNode::supports_level(RenderLevel level) const noexcept {
    return (level.mip == 0 && (level.quality == RenderQuality::Final || level.quality == RenderQuality::Preview)) ||
           ((level.mip == 1 || level.mip == 2) && level.quality == RenderQuality::Preview);
}

Rect CoverageSourceNode::source_bounds() const noexcept { return image_.metadata().bounds; }

Rect CoverageSourceNode::output_bounds(RenderLevel level) const {
    if (!supports_level(level)) throw std::invalid_argument("unsupported coverage source render level");
    const auto native = source_bounds();
    if (level.mip == 0) return native;
    const auto scale = 1u << level.mip;
    return {0,0,native.width/scale+(native.width%scale != 0),
                native.height/scale+(native.height%scale != 0)};
}

Rect CoverageSourceNode::required_native_region(Rect output, RenderLevel level) const {
    contained(output_bounds(level), output);
    if (level.mip == 0) return output;
    const auto native = source_bounds();
    const auto scale = 1u << level.mip;
    const auto start_x = static_cast<std::uint64_t>(output.x)*scale;
    const auto start_y = static_cast<std::uint64_t>(output.y)*scale;
    const auto end_x = std::min<std::uint64_t>((static_cast<std::uint64_t>(output.x)+output.width)*scale,
                                              native.width);
    const auto end_y = std::min<std::uint64_t>((static_cast<std::uint64_t>(output.y)+output.height)*scale,
                                              native.height);
    return {static_cast<std::uint32_t>(native.x+start_x), static_cast<std::uint32_t>(native.y+start_y),
            static_cast<std::uint32_t>(end_x-start_x), static_cast<std::uint32_t>(end_y-start_y)};
}

std::array<std::uint8_t, 32> CoverageSourceNode::source_fingerprint() const { return image_.fingerprint(); }

CoverageTile CoverageSourceNode::render(Rect bounds) const { return render_level(bounds, {}); }

CoverageTile CoverageSourceNode::render_level(Rect bounds, RenderLevel level) const {
    // Validate level and request before allocation, also proving native mapping is safe.
    (void)required_native_region(bounds, level);
    CoverageTile output{bounds, std::vector<float>(count(bounds.width,bounds.height))};
    const auto native = source_bounds();
    const auto& input = image_.samples();
    const auto scale = 1u << level.mip;
    for (std::uint32_t y = 0; y < bounds.height; ++y) {
        const auto start_y = level.mip == 0 ? bounds.y-native.y+y :
            (static_cast<std::uint64_t>(bounds.y)+y)*scale;
        const auto end_y = std::min<std::uint64_t>(start_y+scale,native.height);
        for (std::uint32_t x = 0; x < bounds.width; ++x) {
            const auto target = static_cast<std::size_t>(y)*bounds.width+x;
            const auto start_x = level.mip == 0 ? bounds.x-native.x+x :
                (static_cast<std::uint64_t>(bounds.x)+x)*scale;
            if (level.mip == 0) {
                output.coverage[target] = input[static_cast<std::size_t>(start_y)*native.width+start_x];
                continue;
            }
            const auto end_x = std::min<std::uint64_t>(start_x+scale,native.width);
            double sum = 0;
            for (auto v = start_y; v < end_y; ++v)
                for (auto u = start_x; u < end_x; ++u)
                    sum += static_cast<double>(input[static_cast<std::size_t>(v)*native.width+u]);
            const auto actual_count = static_cast<double>((end_x-start_x)*(end_y-start_y));
            output.coverage[target] = static_cast<float>(sum/actual_count);
        }
    }
    return output;
}

} // namespace rawengine

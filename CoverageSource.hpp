#pragma once

#include "CoverageOps.hpp"

namespace rawengine {

struct CoverageMetadata {
    Rect bounds;
    std::uint32_t row_stride_samples = 0; // Caller layout; zero means width.
};

// Canonical tightly packed immutable snapshot; always copies visible input samples.
class RAWENGINE_API CoverageImage {
public:
    CoverageImage(CoverageMetadata metadata, const std::vector<float>& samples);
    const CoverageMetadata& metadata() const noexcept;
    const std::vector<float>& samples() const noexcept;
    std::array<std::uint8_t, 32> fingerprint() const;
private:
    struct State;
    std::shared_ptr<const State> state_;
};

RAWENGINE_API std::array<std::uint8_t, 32> fingerprint_coverage_source(const CoverageImage& image);

// Typed scalar source. It intentionally does not derive from the RGB-only Node.
class RAWENGINE_API CoverageSourceNode final {
public:
    explicit CoverageSourceNode(CoverageImage image);
    CoverageTile render(Rect bounds) const;
    CoverageTile render_level(Rect bounds, RenderLevel level) const;
    bool supports_level(RenderLevel level) const noexcept;
    Rect source_bounds() const noexcept;
    Rect output_bounds(RenderLevel level = {}) const;
    Rect required_native_region(Rect output, RenderLevel level = {}) const;
    std::array<std::uint8_t, 32> source_fingerprint() const;
private:
    CoverageImage image_;
};

} // namespace rawengine

#pragma once

#include "RawEngine.hpp"

namespace rawengine {

enum class IccRenderingIntent { Perceptual, RelativeColorimetric,
                                Saturation, AbsoluteColorimetric };

struct IccDisplayOptions {
    IccRenderingIntent intent = IccRenderingIntent::RelativeColorimetric;
    bool black_point_compensation = false;
};

// Available only when RAWENGINE_WITH_LCMS=ON. The profile must be an RGB
// display/output ICC profile. The exact bytes are hashed and retained.
RAWENGINE_API std::shared_ptr<const IccDisplayTransform> make_lcms_display_transform(
    std::vector<std::uint8_t> profile_bytes, IccDisplayOptions options = {});

// SHA-256 of the exact bytes, independent of ICC's separate internal profile ID.
RAWENGINE_API std::array<std::uint8_t, 32> sha256_bytes(
    const std::vector<std::uint8_t>& bytes);

} // namespace rawengine

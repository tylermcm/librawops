#pragma once

#include "RawEngine.hpp"

namespace rawengine {

enum class IccRenderingIntent { Perceptual, RelativeColorimetric,
                                Saturation, AbsoluteColorimetric };

struct IccDisplayOptions {
    IccRenderingIntent intent = IccRenderingIntent::RelativeColorimetric;
    bool black_point_compensation = false;
};

struct IccRasterMetadata {
    std::uint32_t width = 0, height = 0;
    std::uint32_t row_stride_pixels = 0; // 0 means tightly packed width.
    WorkingSpace working_space = WorkingSpace::LinearProPhotoD50;
};

// Digest of encoded visible RGB samples and geometry/working space. The ICC
// profile and conversion policy are independently bound by IccProfileIdentity.
RAWENGINE_API std::array<std::uint8_t, 32> fingerprint_icc_raster_source(
    IccRasterMetadata metadata, const std::vector<std::uint16_t>& pixels);

// Available only when RAWENGINE_WITH_LCMS=ON. The profile must be an RGB
// display/output ICC profile. The exact bytes are hashed and retained.
RAWENGINE_API std::shared_ptr<const IccDisplayTransform> make_lcms_display_transform(
    std::vector<std::uint8_t> profile_bytes, IccDisplayOptions options = {});

// Takes ownership of interleaved, profile-encoded RGB uint16 samples. Rendering
// converts only the requested tile to signed scene-linear float32. The caller
// must keep the exact input profile bytes in its edit/cache identity.
RAWENGINE_API std::shared_ptr<const Node> make_lcms_raster_source(
    IccRasterMetadata metadata, std::vector<std::uint16_t> pixels,
    std::vector<std::uint8_t> profile_bytes, IccDisplayOptions options = {});

// SHA-256 of the exact bytes, independent of ICC's separate internal profile ID.
RAWENGINE_API std::array<std::uint8_t, 32> sha256_bytes(
    const std::vector<std::uint8_t>& bytes);

} // namespace rawengine

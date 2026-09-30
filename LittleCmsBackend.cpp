#include "LittleCmsBackend.hpp"
#include "Sha256.hpp"

#include <lcms2.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace rawengine {
namespace {

int lcms_intent(IccRenderingIntent intent) {
    switch (intent) {
    case IccRenderingIntent::Perceptual: return INTENT_PERCEPTUAL;
    case IccRenderingIntent::RelativeColorimetric: return INTENT_RELATIVE_COLORIMETRIC;
    case IccRenderingIntent::Saturation: return INTENT_SATURATION;
    case IccRenderingIntent::AbsoluteColorimetric: return INTENT_ABSOLUTE_COLORIMETRIC;
    }
    throw std::invalid_argument("unknown ICC rendering intent");
}

const char* intent_name(IccRenderingIntent intent) {
    switch (intent) {
    case IccRenderingIntent::Perceptual: return "perceptual";
    case IccRenderingIntent::RelativeColorimetric: return "relative_colorimetric";
    case IccRenderingIntent::Saturation: return "saturation";
    case IccRenderingIntent::AbsoluteColorimetric: return "absolute_colorimetric";
    }
    throw std::invalid_argument("unknown ICC rendering intent");
}

class LittleCmsDisplayTransform final : public IccDisplayTransform {
public:
    LittleCmsDisplayTransform(std::vector<std::uint8_t> bytes, IccDisplayOptions options)
        : bytes_(std::move(bytes)), digest_(sha256_bytes(bytes_)) {
        if (bytes_.size() < 128 || bytes_.size() > 16 * 1024 * 1024 ||
            std::memcmp(bytes_.data() + 36, "acsp", 4) != 0)
            throw std::invalid_argument("ICC profile header or size is invalid");
        const int intent = lcms_intent(options.intent);
        identity_.profile_sha256 = digest_;
        identity_.intent = intent_name(options.intent);
        identity_.black_point_compensation = options.black_point_compensation;
        try {
            context_ = cmsCreateContext(nullptr, nullptr);
            if (!context_) throw std::runtime_error("LittleCMS context creation failed");
            output_ = cmsOpenProfileFromMemTHR(context_, bytes_.data(),
                                                static_cast<cmsUInt32Number>(bytes_.size()));
            if (!output_ || cmsGetColorSpace(output_) != cmsSigRgbData ||
                (cmsGetDeviceClass(output_) != cmsSigDisplayClass &&
                 cmsGetDeviceClass(output_) != cmsSigOutputClass))
                throw std::invalid_argument("ICC output must be an RGB display/output profile");

            cmsCIExyY white{0.3127, 0.3290, 1.0};
            cmsCIExyYTRIPLE primaries{{0.640, 0.330, 1.0},
                                      {0.300, 0.600, 1.0},
                                      {0.150, 0.060, 1.0}};
            cmsToneCurve* curves[3]{};
            for (auto& curve : curves) curve = cmsBuildGamma(context_, 1.0);
            if (curves[0] && curves[1] && curves[2])
                input_ = cmsCreateRGBProfileTHR(context_, &white, &primaries, curves);
            for (auto* curve : curves) if (curve) cmsFreeToneCurve(curve);
            if (!input_) throw std::runtime_error("linear sRGB profile creation failed");
            const auto flags = options.black_point_compensation
                                   ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0;
            transform_ = cmsCreateTransformTHR(context_, input_, TYPE_RGB_FLT,
                                                output_, TYPE_RGB_FLT, intent, flags);
            if (!transform_) throw std::invalid_argument("ICC display transform creation failed");
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~LittleCmsDisplayTransform() override { cleanup(); }

    std::array<std::uint8_t, 32> profile_sha256() const noexcept override {
        return digest_;
    }
    std::optional<IccProfileIdentity> output_icc_identity() const override {
        return identity_;
    }

    void apply(float* rgb, std::size_t pixels) const override {
        if (!pixels) return;
        if (!rgb || pixels > std::numeric_limits<cmsUInt32Number>::max() ||
            pixels > std::vector<float>().max_size() / 3)
            throw std::invalid_argument("ICC tile size is invalid");
        std::vector<float> converted(pixels * 3);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cmsDoTransform(transform_, rgb, converted.data(),
                           static_cast<cmsUInt32Number>(pixels));
        }
        for (std::size_t i = 0; i < converted.size(); ++i) {
            if (!std::isfinite(converted[i]))
                throw std::domain_error("LittleCMS returned non-finite RGB");
            rgb[i] = std::clamp(converted[i], 0.0f, 1.0f); // Explicit hard gamut policy.
        }
    }

private:
    void cleanup() noexcept {
        if (transform_) cmsDeleteTransform(transform_);
        if (input_) cmsCloseProfile(input_);
        if (output_) cmsCloseProfile(output_);
        if (context_) cmsDeleteContext(context_);
        transform_ = nullptr; input_ = nullptr; output_ = nullptr; context_ = nullptr;
    }

    std::vector<std::uint8_t> bytes_;
    std::array<std::uint8_t, 32> digest_;
    IccProfileIdentity identity_;
    cmsContext context_ = nullptr;
    cmsHPROFILE input_ = nullptr, output_ = nullptr;
    cmsHTRANSFORM transform_ = nullptr;
    mutable std::mutex mutex_;
};

class LittleCmsRasterSource final : public Node {
public:
    LittleCmsRasterSource(IccRasterMetadata metadata, std::vector<std::uint16_t> pixels,
                          std::vector<std::uint8_t> bytes, IccDisplayOptions options)
        : metadata_(metadata), pixels_(std::move(pixels)), bytes_(std::move(bytes)) {
        if (!metadata_.width || !metadata_.height ||
            metadata_.row_stride_pixels && metadata_.row_stride_pixels < metadata_.width)
            throw std::invalid_argument("invalid ICC raster dimensions or stride");
        if (!metadata_.row_stride_pixels) metadata_.row_stride_pixels = metadata_.width;
        const auto stride = static_cast<std::size_t>(metadata_.row_stride_pixels);
        const auto height = static_cast<std::size_t>(metadata_.height);
        if (stride > pixels_.max_size() / 3 / height ||
            pixels_.size() != stride * height * 3)
            throw std::invalid_argument("ICC raster sample count does not match stride");
        if (bytes_.size() < 128 || bytes_.size() > 16 * 1024 * 1024 ||
            std::memcmp(bytes_.data() + 36, "acsp", 4) != 0)
            throw std::invalid_argument("ICC profile header or size is invalid");
        fingerprint_ = fingerprint_icc_raster_source(metadata_, pixels_);
        identity_.profile_sha256 = sha256_bytes(bytes_);
        identity_.intent = intent_name(options.intent);
        identity_.black_point_compensation = options.black_point_compensation;
        const int intent = lcms_intent(options.intent);
        try {
            context_ = cmsCreateContext(nullptr, nullptr);
            if (!context_) throw std::runtime_error("LittleCMS context creation failed");
            input_ = cmsOpenProfileFromMemTHR(context_, bytes_.data(),
                                               static_cast<cmsUInt32Number>(bytes_.size()));
            if (!input_ || cmsGetColorSpace(input_) != cmsSigRgbData ||
                (cmsGetDeviceClass(input_) != cmsSigInputClass &&
                 cmsGetDeviceClass(input_) != cmsSigDisplayClass &&
                 cmsGetDeviceClass(input_) != cmsSigOutputClass))
                throw std::invalid_argument("ICC raster needs an RGB input/display/output profile");
            cmsCIExyY white{};
            cmsCIExyYTRIPLE primaries{};
            switch (metadata_.working_space) {
            case WorkingSpace::LinearProPhotoD50:
                white = {0.3457, 0.3585, 1.0};
                primaries = {{0.7347, 0.2653, 1.0}, {0.1596, 0.8404, 1.0},
                             {0.0366, 0.0001, 1.0}};
                break;
            case WorkingSpace::LinearRec2020D65:
                white = {0.3127, 0.3290, 1.0};
                primaries = {{0.708, 0.292, 1.0}, {0.170, 0.797, 1.0},
                             {0.131, 0.046, 1.0}};
                break;
            default: throw std::invalid_argument("unsupported ICC raster working space");
            }
            cmsToneCurve* curves[3]{};
            for (auto& curve : curves) curve = cmsBuildGamma(context_, 1.0);
            if (curves[0] && curves[1] && curves[2])
                output_ = cmsCreateRGBProfileTHR(context_, &white, &primaries, curves);
            for (auto* curve : curves) if (curve) cmsFreeToneCurve(curve);
            if (!output_) throw std::runtime_error("scene-linear ICC profile creation failed");
            const auto flags = options.black_point_compensation
                                   ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0;
            transform_ = cmsCreateTransformTHR(context_, input_, TYPE_RGB_16,
                                                output_, TYPE_RGB_FLT, intent, flags);
            if (!transform_) throw std::invalid_argument("ICC raster transform creation failed");
        } catch (...) { cleanup(); throw; }
    }

    ~LittleCmsRasterSource() override { cleanup(); }

    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::scene_linear(metadata_.working_space);
    }
    std::optional<IccProfileIdentity> input_icc_identity() const override {
        return identity_;
    }
    std::optional<std::array<std::uint8_t, 32>> source_fingerprint() const override {
        return fingerprint_;
    }
    std::optional<Rect> source_bounds() const override {
        return Rect{0, 0, metadata_.width, metadata_.height};
    }

    Tile render(Rect r) const override {
        if (r.x > metadata_.width || r.y > metadata_.height ||
            static_cast<std::uint64_t>(r.x) + r.width > metadata_.width ||
            static_cast<std::uint64_t>(r.y) + r.height > metadata_.height)
            throw std::out_of_range("ICC raster viewport is outside source");
        const auto count = static_cast<std::size_t>(r.width) * r.height;
        if (r.width && count / r.width != r.height ||
            count > std::vector<float>().max_size() / 3 ||
            count > std::numeric_limits<cmsUInt32Number>::max())
            throw std::length_error("ICC raster tile is too large");
        Tile tile{r, std::vector<float>(count * 3), output_descriptor()};
        if (!count) return tile;
        std::vector<std::uint16_t> input(count * 3);
        for (std::uint32_t row = 0; row < r.height; ++row) {
            const auto src = (static_cast<std::size_t>(r.y + row) *
                              metadata_.row_stride_pixels + r.x) * 3;
            const auto dst = static_cast<std::size_t>(row) * r.width * 3;
            std::copy_n(pixels_.data() + src, static_cast<std::size_t>(r.width) * 3,
                        input.data() + dst);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cmsDoTransform(transform_, input.data(), tile.rgb.data(),
                           static_cast<cmsUInt32Number>(count));
        }
        if (!std::all_of(tile.rgb.begin(), tile.rgb.end(),
                         [](float v) { return std::isfinite(v); }))
            throw std::domain_error("LittleCMS returned non-finite scene-linear RGB");
        return tile;
    }

private:
    void cleanup() noexcept {
        if (transform_) cmsDeleteTransform(transform_);
        if (input_) cmsCloseProfile(input_);
        if (output_) cmsCloseProfile(output_);
        if (context_) cmsDeleteContext(context_);
    }
    IccRasterMetadata metadata_;
    std::vector<std::uint16_t> pixels_;
    std::array<std::uint8_t, 32> fingerprint_{};
    std::vector<std::uint8_t> bytes_;
    IccProfileIdentity identity_;
    cmsContext context_ = nullptr;
    cmsHPROFILE input_ = nullptr, output_ = nullptr;
    cmsHTRANSFORM transform_ = nullptr;
    mutable std::mutex mutex_;
};

} // namespace

std::array<std::uint8_t, 32> sha256_bytes(const std::vector<std::uint8_t>& bytes) {
    Sha256 hash;
    hash.update(bytes.data(), bytes.size());
    return hash.finish();
}

std::shared_ptr<const IccDisplayTransform> make_lcms_display_transform(
    std::vector<std::uint8_t> profile_bytes, IccDisplayOptions options) {
    return std::make_shared<LittleCmsDisplayTransform>(std::move(profile_bytes), options);
}

std::shared_ptr<const Node> make_lcms_raster_source(
    IccRasterMetadata metadata, std::vector<std::uint16_t> pixels,
    std::vector<std::uint8_t> profile_bytes, IccDisplayOptions options) {
    return std::make_shared<LittleCmsRasterSource>(metadata, std::move(pixels),
                                                    std::move(profile_bytes), options);
}

} // namespace rawengine

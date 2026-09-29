#include "LittleCmsBackend.hpp"

#include <lcms2.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace rawengine {
namespace {

constexpr std::uint32_t round_constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

int lcms_intent(IccRenderingIntent intent) {
    switch (intent) {
    case IccRenderingIntent::Perceptual: return INTENT_PERCEPTUAL;
    case IccRenderingIntent::RelativeColorimetric: return INTENT_RELATIVE_COLORIMETRIC;
    case IccRenderingIntent::Saturation: return INTENT_SATURATION;
    case IccRenderingIntent::AbsoluteColorimetric: return INTENT_ABSOLUTE_COLORIMETRIC;
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
    std::vector<std::uint8_t> bytes_;
    cmsContext context_ = nullptr;
    cmsHPROFILE input_ = nullptr, output_ = nullptr;
    cmsHTRANSFORM transform_ = nullptr;
    mutable std::mutex mutex_;
};

} // namespace

std::array<std::uint8_t, 32> sha256_bytes(const std::vector<std::uint8_t>& bytes) {
    // FIPS 180-4 SHA-256; original implementation, no third-party hash code.
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<std::uint8_t> padded(bytes);
    padded.push_back(0x80);
    while (padded.size() % 64 != 56) padded.push_back(0);
    const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8;
    for (int i = 7; i >= 0; --i)
        padded.push_back(static_cast<std::uint8_t>(bit_length >> (i * 8)));
    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        std::uint32_t w[64]{};
        for (int i = 0; i < 16; ++i) {
            const auto base = offset + static_cast<std::size_t>(i) * 4;
            w[i] = (std::uint32_t(padded[base]) << 24) |
                   (std::uint32_t(padded[base + 1]) << 16) |
                   (std::uint32_t(padded[base + 2]) << 8) | padded[base + 3];
        }
        for (int i = 16; i < 64; ++i) {
            const auto s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const auto s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto a = h[0], b = h[1], c = h[2], d = h[3];
        auto e = h[4], f = h[5], g = h[6], q = h[7];
        for (int i = 0; i < 64; ++i) {
            const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choice = (e & f) ^ (~e & g);
            const auto t1 = q + s1 + choice + round_constants[i] + w[i];
            const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + majority;
            q = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += q;
    }
    std::array<std::uint8_t, 32> digest{};
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j)
            digest[static_cast<std::size_t>(i) * 4 + j] =
                static_cast<std::uint8_t>(h[i] >> ((3 - j) * 8));
    return digest;
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

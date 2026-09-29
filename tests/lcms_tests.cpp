#include "LittleCmsBackend.hpp"

#include <lcms2.h>

#include <array>
#include <cmath>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace rawengine;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint8_t> standard_srgb_profile() {
    cmsHPROFILE profile = cmsCreate_sRGBProfile();
    if (!profile) throw std::runtime_error("test profile creation failed");
    cmsUInt32Number size = 0;
    if (!cmsSaveProfileToMem(profile, nullptr, &size) || !size) {
        cmsCloseProfile(profile);
        throw std::runtime_error("test profile size query failed");
    }
    std::vector<std::uint8_t> bytes(size);
    const bool saved = cmsSaveProfileToMem(profile, bytes.data(), &size) != 0;
    cmsCloseProfile(profile);
    if (!saved) throw std::runtime_error("test profile serialization failed");
    bytes.resize(size);
    return bytes;
}

void rejects_bad_profile() {
    try {
        make_lcms_display_transform(std::vector<std::uint8_t>(128, 0));
    } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid ICC profile was accepted");
}

void rejects_invalid_options(const std::vector<std::uint8_t>& bytes) {
    try {
        make_lcms_display_transform(
            bytes, {static_cast<IccRenderingIntent>(99), false});
    } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid ICC intent was accepted");
}

} // namespace

int main() {
    try {
        const std::vector<std::uint8_t> abc{'a', 'b', 'c'};
        const std::array<std::uint8_t, 32> expected{
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
            0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
            0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
            0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
        require(sha256_bytes(abc) == expected, "SHA-256 known vector mismatch");
        rejects_bad_profile();

        auto bytes = standard_srgb_profile();
        rejects_invalid_options(bytes);
        auto transform = make_lcms_display_transform(bytes);
        require(transform->profile_sha256() == sha256_bytes(bytes),
                "output profile digest does not hash exact bytes");
        float rgb[3]{0.0f, 0.18f, 1.0f};
        transform->apply(rgb, 1);
        require(std::abs(rgb[0]) < 0.005f &&
                std::abs(rgb[1] - 0.461356f) < 0.02f &&
                std::abs(rgb[2] - 1.0f) < 0.005f,
                "real sRGB ICC transform disagrees with reference transfer");
        try {
            transform->apply(nullptr, 1);
            throw std::runtime_error("null ICC tile was accepted");
        } catch (const std::invalid_argument&) {}

        GraphRecipe recipe;
        recipe.output_mode = OutputMode::IccDisplay;
        RasterImage raster({7, 5, 0, WorkingSpace::LinearRec2020D65},
                           std::vector<float>(7 * 5 * 3, 0.5f));
        ImageGraph graph(raster, recipe, transform);
        Renderer renderer;
        const auto full = renderer.render_image(graph, {0, 0, 7, 5}, 7);
        const auto roi = renderer.render_image(graph, {1, 1, 3, 3}, 2);
        require(full.descriptor == roi.descriptor &&
                full.descriptor.profile_sha256 == transform->profile_sha256(),
                "real ICC render lost profile identity");
        for (float value : roi.rgb)
            require(std::isfinite(value) && value >= 0.0f && value <= 1.0f,
                    "real ICC render returned unbounded output");
        for (std::uint32_t y = 0; y < 3; ++y)
            for (std::uint32_t x = 0; x < 3; ++x)
                for (std::uint32_t c = 0; c < 3; ++c)
                    require(std::abs(roi.rgb[(y * 3 + x) * 3 + c] -
                                     full.rgb[((y + 1) * 7 + x + 1) * 3 + c]) < 1e-6f,
                            "real ICC tiled ROI differs from full render");

        std::vector<std::future<void>> tasks;
        for (int task = 0; task < 8; ++task)
            tasks.push_back(std::async(std::launch::async, [transform] {
                for (int repeat = 0; repeat < 32; ++repeat) {
                    float sample[3]{0.0f, 0.18f, 1.0f};
                    transform->apply(sample, 1);
                    require(std::abs(sample[1] - 0.461356f) < 0.02f,
                            "concurrent ICC transform changed result");
                }
            }));
        for (auto& task : tasks) task.get();

        auto bpc = make_lcms_display_transform(
            bytes, {IccRenderingIntent::RelativeColorimetric, true});
        float bpc_sample[3]{0.5f, 0.5f, 0.5f};
        bpc->apply(bpc_sample, 1);
        require(std::isfinite(bpc_sample[0]) && bpc_sample[0] >= 0.0f &&
                bpc_sample[0] <= 1.0f, "BPC transform failed");
        std::cout << "LittleCMS core backend tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

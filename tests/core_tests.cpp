#include "RawEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace rawengine;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void near(float actual, float expected, const char* message) {
    if (std::abs(actual - expected) > 2e-6f * std::max(1.0f, std::abs(expected)))
        throw std::runtime_error(message);
}

template <class F>
void rejects(F&& function, const char* message) {
    try { function(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}

RawImage color_bayer(std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint16_t> samples(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            samples[static_cast<std::size_t>(y) * width + x] =
                y % 2 ? (x % 2 ? 3000 : 2000) : (x % 2 ? 2000 : 1000);
        }
    }
    return RawImage(width, height, std::move(samples));
}

void test_signed_sensor_values() {
    std::vector<std::uint16_t> samples(16, 500);
    samples[0] = 0;     // Below the declared black level.
    samples[10] = 2000; // Above the declared white level, at a red site.
    auto raw = std::make_shared<RawUnpackNode>(
        RawImage(4, 4, std::move(samples), BayerPattern::RGGB, 100, 1000));
    const Tile linear = raw->render({0, 0, 4, 4});
    require(linear.domain == PixelDomain::CameraLinearRGB, "RAW tile domain is wrong");
    near(linear.rgb[0], -100.0f / 900.0f, "negative sensor detail was clipped");
    near(linear.rgb[(2 * 4 + 2) * 3], 1900.0f / 900.0f,
         "over-white sensor detail was clipped");

    ToneCurveNode tone(raw, 0.25f, 1.0f);
    const Tile mapped = tone.render({0, 0, 4, 4});
    require(mapped.domain == PixelDomain::ToneMappedUnmanagedRGB,
            "tone tile must have a distinct domain");
    require(std::all_of(mapped.rgb.begin(), mapped.rgb.end(),
                        [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; }),
            "explicit tone output must be finite and bounded");
    near(mapped.rgb[0], 0.0f, "tone output should clip negative samples");
    rejects([&] { ExposureNode invalid(std::make_shared<ToneCurveNode>(tone), 1.0f); },
            "linear exposure accepted tone-mapped input");
}

void test_gain_exposure_and_roi() {
    const RawImage raw = color_bayer(7, 5);
    auto unpack = std::make_shared<RawUnpackNode>(raw);
    auto wb = std::make_shared<WhiteBalanceNode>(unpack, 2.0f, 1.0f, 0.5f);
    ExposureNode exposure(wb, 1.0f);
    const Tile tile = exposure.render({0, 0, 7, 5});
    require(tile.domain == PixelDomain::CameraLinearRGB, "gain/exposure changed domain");
    near(tile.rgb[0], 4000.0f / 65535.0f, "red gain/exposure mismatch");
    near(tile.rgb[1], 4000.0f / 65535.0f, "green gain/exposure mismatch");
    near(tile.rgb[2], 3000.0f / 65535.0f, "blue gain/exposure mismatch");

    GraphRecipe recipe;
    recipe.red_gain = 2.0f;
    recipe.blue_gain = 0.5f;
    recipe.exposure_stops = 1.0f;
    ImageGraph graph(raw, recipe);
    Renderer renderer;
    const auto full = renderer.render_roi(graph, {0, 0, 7, 5}, 7);
    const auto roi = renderer.render_roi(graph, {2, 1, 3, 3}, 2);
    for (std::uint32_t y = 0; y < 3; ++y)
        for (std::uint32_t x = 0; x < 3; ++x)
            for (std::uint32_t c = 0; c < 3; ++c)
                near(roi[(y * 3 + x) * 3 + c], full[((y + 1) * 7 + x + 2) * 3 + c],
                     "tiled ROI differs from full render");
}

void test_invalid_input() {
    rejects([] { RawImage bad(2, 2, {1, 2, 3}); }, "short Bayer buffer accepted");
    rejects([] { RawImage bad(2, 2, {1, 2, 3, 4}, BayerPattern::RGGB, 10, 10); },
            "invalid black/white levels accepted");
    rejects([] { WhiteBalanceNode bad(nullptr, 1, 1, 1); }, "null node accepted");
    rejects([] { ExposureNode bad(nullptr, 1); }, "null exposure node accepted");
}

} // namespace

int main() {
    try {
        test_signed_sensor_values();
        test_gain_exposure_and_roi();
        test_invalid_input();
        std::cout << "RawEngine core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

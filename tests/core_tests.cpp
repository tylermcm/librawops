#include "RawEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
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

template <class F>
void rejects_domain(F&& function, const char* message) {
    try { function(); }
    catch (const std::domain_error&) { return; }
    throw std::runtime_error(message);
}

class IncorrectTileNode final : public Node {
public:
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::camera_linear();
    }
    Tile render(Rect bounds) const override {
        return {bounds, std::vector<float>(static_cast<std::size_t>(bounds.width) *
                                           bounds.height * 3), ImageDescriptor::tone_mapped()};
    }
};

class NonFiniteToneNode final : public Node {
public:
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::tone_mapped();
    }
    Tile render(Rect bounds) const override {
        return {bounds, std::vector<float>(static_cast<std::size_t>(bounds.width) *
                                           bounds.height * 3,
                                           std::numeric_limits<float>::quiet_NaN()),
                ImageDescriptor::tone_mapped()};
    }
};

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
    require(linear.descriptor == ImageDescriptor::camera_linear(), "RAW tile descriptor is wrong");
    near(linear.rgb[0], -100.0f / 900.0f, "negative sensor detail was clipped");
    near(linear.rgb[(2 * 4 + 2) * 3], 1900.0f / 900.0f,
         "over-white sensor detail was clipped");

    ToneCurveNode tone(raw, 0.25f, 1.0f);
    const Tile mapped = tone.render({0, 0, 4, 4});
    require(mapped.descriptor == ImageDescriptor::tone_mapped(),
            "tone tile must have a distinct descriptor");
    require(mapped.rgb[0] < 0.0f, "tone node clipped negative intermediate data");
    OutputClipNode clip(std::make_shared<ToneCurveNode>(tone));
    const Tile bounded = clip.render({0, 0, 4, 4});
    require(bounded.descriptor == ImageDescriptor::bounded_output(),
            "output tile must have a bounded descriptor");
    require(std::all_of(bounded.rgb.begin(), bounded.rgb.end(),
                        [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; }),
            "explicit output clip must be finite and bounded");
    near(bounded.rgb[0], 0.0f, "output clip should clamp negative samples");
    rejects([&] { ExposureNode invalid(std::make_shared<ToneCurveNode>(tone), 1.0f); },
            "linear exposure accepted tone-mapped input");
}

void test_gain_exposure_and_roi() {
    const RawImage raw = color_bayer(7, 5);
    auto unpack = std::make_shared<RawUnpackNode>(raw);
    auto wb = std::make_shared<WhiteBalanceNode>(unpack, 2.0f, 1.0f, 0.5f);
    ExposureNode exposure(wb, 1.0f);
    const Tile tile = exposure.render({0, 0, 7, 5});
    require(tile.descriptor == ImageDescriptor::camera_linear(),
            "gain/exposure changed descriptor");
    near(tile.rgb[0], 4000.0f / 65535.0f, "red gain/exposure mismatch");
    near(tile.rgb[1], 4000.0f / 65535.0f, "green gain/exposure mismatch");
    near(tile.rgb[2], 3000.0f / 65535.0f, "blue gain/exposure mismatch");

    GraphRecipe recipe;
    recipe.red_gain = 2.0f;
    recipe.blue_gain = 0.5f;
    recipe.exposure_stops = 1.0f;
    ImageGraph graph(raw, recipe);
    require(graph.output().output_descriptor() == ImageDescriptor::bounded_output(),
            "graph output descriptor is wrong");
    Renderer renderer;
    const auto full = renderer.render_roi(graph, {0, 0, 7, 5}, 7);
    const Tile rendered = renderer.render_image(graph, {2, 1, 3, 3}, 2);
    require(rendered.descriptor == ImageDescriptor::bounded_output(),
            "materialized render lost its output descriptor");
    const auto& roi = rendered.rgb;
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
    rejects([] {
        ToneCurveNode bad(std::make_shared<RawUnpackNode>(color_bayer(2, 2)),
                          0.25f, std::numeric_limits<float>::denorm_min());
    }, "tone curve accepted a gamma whose reciprocal overflows");
    rejects([] {
        OutputClipNode bad(std::make_shared<RawUnpackNode>(color_bayer(2, 2)));
    }, "output clip accepted linear input");
    rejects_domain([] {
        ExposureNode exposure(std::make_shared<IncorrectTileNode>(), 1.0f);
        exposure.render({0, 0, 1, 1});
    }, "upstream descriptor mismatch was accepted");
    rejects_domain([] {
        OutputClipNode clip(std::make_shared<NonFiniteToneNode>());
        clip.render({0, 0, 1, 1});
    }, "non-finite output value was accepted");
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

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

template <class F>
void rejects_bounds(F&& function, const char* message) {
    try { function(); }
    catch (const std::out_of_range&) { return; }
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

class ConstantCameraNode final : public Node {
public:
    explicit ConstantCameraNode(std::array<float, 3> pixel) : pixel_(pixel) {}
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::camera_linear();
    }
    Tile render(Rect bounds) const override {
        Tile tile{bounds, std::vector<float>(static_cast<std::size_t>(bounds.width) *
                                             bounds.height * 3)};
        for (std::size_t i = 0; i < tile.rgb.size(); i += 3)
            std::copy(pixel_.begin(), pixel_.end(), tile.rgb.begin() + i);
        return tile;
    }
private:
    std::array<float, 3> pixel_;
};

class ConstantToneSrgbNode final : public Node {
public:
    explicit ConstantToneSrgbNode(std::array<float, 3> pixel) : pixel_(pixel) {}
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::display_linear_srgb();
    }
    Tile render(Rect bounds) const override {
        Tile tile{bounds, std::vector<float>(static_cast<std::size_t>(bounds.width) *
                                             bounds.height * 3), output_descriptor()};
        for (std::size_t i = 0; i < tile.rgb.size(); i += 3)
            std::copy(pixel_.begin(), pixel_.end(), tile.rgb.begin() + i);
        return tile;
    }
private:
    std::array<float, 3> pixel_;
};

class MockIccTransform final : public IccDisplayTransform {
public:
    explicit MockIccTransform(bool bad = false, bool zero_digest = false)
        : bad_(bad), zero_digest_(zero_digest) {}
    std::array<std::uint8_t, 32> profile_sha256() const noexcept override {
        std::array<std::uint8_t, 32> digest{};
        if (!zero_digest_) digest[0] = 0xAB;
        return digest;
    }
    void apply(float* rgb, std::size_t pixels) const override {
        for (std::size_t i = 0; i < pixels * 3; ++i)
            rgb[i] = bad_ ? std::numeric_limits<float>::quiet_NaN()
                          : std::clamp(rgb[i], 0.0f, 1.0f);
    }
private:
    bool bad_, zero_digest_;
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

void test_decoded_raw_metadata() {
    RawMetadata metadata;
    metadata.width = 4;
    metadata.height = 4;
    metadata.row_stride_samples = 6;
    metadata.active_area = {1, 1, 2, 2};
    metadata.cfa_phase_x = 1;
    metadata.black_levels = {100, 200, 300, 400};
    metadata.white_levels = {1100, 1200, 1300, 1400};
    std::vector<std::uint16_t> samples(24, 65535); // Poison padding and inactive pixels.
    for (std::uint32_t y = 1; y <= 2; ++y)
        for (std::uint32_t x = 1; x <= 2; ++x) {
            const auto site = (y & 1u) * 2u + ((x + 1u) & 1u);
            samples[static_cast<std::size_t>(y) * 6 + x] =
                static_cast<std::uint16_t>(metadata.black_levels[site] + 500);
        }
    RawImage raw(metadata, std::move(samples));
    require(raw.metadata().active_area.x == 1 && raw.metadata().row_stride_samples == 6,
            "RAW metadata was not retained");
    const Tile active = RawUnpackNode(raw).render({1, 1, 2, 2});
    for (float value : active.rgb)
        near(value, 0.5f, "stride, phase, per-site level, or active-area halo is wrong");
    rejects_bounds([&] { RawUnpackNode(raw).render({0, 0, 1, 1}); },
                   "inactive sensor area was rendered");

    RawMetadata phase;
    phase.width = 2;
    phase.height = 2;
    phase.cfa_phase_x = 1;
    const Tile shifted = RawUnpackNode(RawImage(phase, {100, 200, 300, 400}))
                             .render({0, 0, 2, 2});
    near(shifted.rgb[0], 200.0f / 65535.0f, "CFA phase did not move red site");
    near(shifted.rgb[1], 100.0f / 65535.0f, "CFA phase did not move green site");
    near(shifted.rgb[2], 300.0f / 65535.0f, "CFA phase did not move blue site");
}

void test_camera_color_transform() {
    const float d50_x = static_cast<float>(0.3457 / 0.3585);
    const float d50_z = static_cast<float>((1.0 - 0.3457 - 0.3585) / 0.3585);
    CameraColorTransform identity{{1, 0, 0, 0, 1, 0, 0, 0, 1},
                                   WorkingSpace::LinearProPhotoD50};
    auto neutral = std::make_shared<ConstantCameraNode>(
        std::array<float, 3>{d50_x, 1.0f, d50_z});
    CameraToWorkingNode prophoto(neutral, identity);
    const Tile p = prophoto.render({0, 0, 1, 1});
    require(p.descriptor == ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50),
            "ProPhoto descriptor is wrong");
    for (float channel : p.rgb) near(channel, 1.0f, "D50 did not map to ProPhoto neutral");

    identity.target = WorkingSpace::LinearRec2020D65;
    CameraToWorkingNode rec2020(neutral, identity);
    const Tile r = rec2020.render({0, 0, 1, 1});
    require(r.descriptor == ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65),
            "Rec.2020 descriptor is wrong");
    for (float channel : r.rgb) near(channel, 1.0f, "D50 did not adapt to Rec.2020 neutral");

    auto xyz = std::make_shared<ConstantCameraNode>(
        std::array<float, 3>{0.5f, 0.25f, 0.1f});
    identity.target = WorkingSpace::LinearProPhotoD50;
    const Tile colored = CameraToWorkingNode(xyz, identity).render({0, 0, 1, 1});
    // Independent rounded XYZ->linear ROMM matrix from the ICC specification.
    require(std::abs(colored.rgb[0] - 0.60399f) < 0.001f &&
            std::abs(colored.rgb[1] - 0.1068f) < 0.001f &&
            std::abs(colored.rgb[2] - 0.12123f) < 0.001f,
            "ProPhoto conversion disagrees with ICC reference matrix");
    identity.target = WorkingSpace::LinearRec2020D65;
    const Tile rec_colored = CameraToWorkingNode(xyz, identity).render({0, 0, 1, 1});
    // Independent W3C reference: D50->D65 Bradford, then XYZ->linear Rec.2020.
    require(std::abs(rec_colored.rgb[0] - 0.7016773f) < 0.001f &&
            std::abs(rec_colored.rgb[1] - 0.0718773f) < 0.001f &&
            std::abs(rec_colored.rgb[2] - 0.1244141f) < 0.001f,
            "Rec.2020 conversion disagrees with W3C reference matrices");
    identity.target = WorkingSpace::LinearProPhotoD50;

    const Tile over = CameraToWorkingNode(
        std::make_shared<ConstantCameraNode>(
            std::array<float, 3>{2 * d50_x, 2.0f, 2 * d50_z}), identity)
                          .render({0, 0, 1, 1});
    for (float channel : over.rgb) near(channel, 2.0f, "transform clipped over-white RGB");
    const Tile negative = CameraToWorkingNode(
        std::make_shared<ConstantCameraNode>(
            std::array<float, 3>{-d50_x, -1.0f, -d50_z}), identity)
                              .render({0, 0, 1, 1});
    for (float channel : negative.rgb) near(channel, -1.0f, "transform clipped negative RGB");

    GraphRecipe recipe;
    recipe.camera_color = CameraColorTransform{{d50_x, 0, 0, 0, 1, 0, 0, 0, d50_z},
                                                WorkingSpace::LinearRec2020D65};
    ImageGraph graph(color_bayer(7, 5), recipe);
    const auto descriptor = graph.output().output_descriptor();
    require(descriptor.domain == PixelDomain::BoundedUnmanagedRGB &&
            descriptor.primaries == ColorPrimaries::Rec2020 &&
            descriptor.white_point == WhitePoint::D65,
            "graph dropped working-space metadata");
    Renderer renderer;
    const auto full = renderer.render_image(graph, {0, 0, 7, 5}, 7);
    const auto crop = renderer.render_image(graph, {1, 1, 3, 3}, 2);
    require(full.descriptor == crop.descriptor, "ROI descriptor differs from full render");
    for (std::uint32_t y = 0; y < 3; ++y)
        for (std::uint32_t x = 0; x < 3; ++x)
            for (std::uint32_t c = 0; c < 3; ++c)
                near(crop.rgb[(y * 3 + x) * 3 + c],
                     full.rgb[((y + 1) * 7 + x + 1) * 3 + c],
                     "color transform tiled ROI differs from full render");

    CameraColorTransform singular{};
    rejects([&] { CameraToWorkingNode bad(neutral, singular); },
            "singular camera matrix accepted");
    identity.camera_to_xyz_d50[0] = std::numeric_limits<double>::quiet_NaN();
    rejects([&] { CameraToWorkingNode bad(neutral, identity); },
            "non-finite camera matrix accepted");
}

void test_scene_linear_raster_source() {
    RasterMetadata metadata{7, 5, 9, WorkingSpace::LinearProPhotoD50};
    std::vector<float> pixels(static_cast<std::size_t>(9) * 5 * 3, 99.0f);
    for (std::uint32_t y = 0; y < 5; ++y)
        for (std::uint32_t x = 0; x < 7; ++x) {
            const auto i = (static_cast<std::size_t>(y) * 9 + x) * 3;
            pixels[i] = x == 0 ? -0.25f : static_cast<float>(x) / 10.0f;
            pixels[i + 1] = 1.5f;
            pixels[i + 2] = static_cast<float>(y) / 10.0f;
        }
    RasterImage image(metadata, pixels);
    RasterSourceNode source(image);
    const Tile tile = source.render({2, 1, 3, 2});
    require(tile.descriptor == ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50),
            "raster source descriptor is wrong");
    near(tile.rgb[0], 0.2f, "raster ROI ignored x offset or stride");
    near(tile.rgb[1], 1.5f, "raster source clipped over-white input");
    near(tile.rgb[(3 * 3) + 2], 0.2f, "raster ROI ignored y offset");
    near(source.render({0, 0, 1, 1}).rgb[0], -0.25f,
         "raster source clipped negative input");

    GraphRecipe recipe;
    recipe.exposure_stops = 1.0f;
    ImageGraph graph(image, recipe);
    require(graph.source_bounds().width == 7 && graph.source_bounds().height == 5,
            "raster graph source bounds are wrong");
    const auto descriptor = graph.output().output_descriptor();
    require(descriptor.domain == PixelDomain::BoundedUnmanagedRGB &&
            descriptor.primaries == ColorPrimaries::ProPhoto &&
            descriptor.white_point == WhitePoint::D50,
            "raster graph lost working-space metadata");
    Renderer renderer;
    const auto full = renderer.render_image(graph, {0, 0, 7, 5}, 7);
    const auto crop = renderer.render_image(graph, {2, 1, 3, 2}, 2);
    for (std::uint32_t y = 0; y < 2; ++y)
        for (std::uint32_t x = 0; x < 3; ++x)
            for (std::uint32_t c = 0; c < 3; ++c)
                near(crop.rgb[(y * 3 + x) * 3 + c],
                     full.rgb[((y + 1) * 7 + x + 2) * 3 + c],
                     "raster tiled ROI differs from full render");
    near(full.rgb[0], 0.0f, "raster final clip did not clamp negative channel");
    require(full.rgb[1] <= 1.0f, "raster final clip did not bound over-white channel");
    rejects_bounds([&] { renderer.render_image(graph, {6, 4, 2, 1}); },
                   "raster render accepted out-of-bounds viewport");

    recipe.red_gain = 2.0f;
    rejects([&] { ImageGraph bad(image, recipe); },
            "raster graph accepted RAW white-balance gains");
    recipe.red_gain = 1.0f;
    recipe.camera_color = CameraColorTransform{};
    rejects([&] { ImageGraph bad(image, recipe); },
            "raster graph accepted a RAW camera matrix");
    rejects([&] { RasterImage bad({7, 5, 6, WorkingSpace::LinearProPhotoD50}, pixels); },
            "raster source accepted short stride");
    pixels[0] = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { RasterImage bad(metadata, pixels); },
            "raster source accepted non-finite pixels");
}

void test_working_space_conversion() {
    std::vector<float> samples{
        0.5f, 0.5f, 0.5f,  // neutral
        1.0f, 0.0f, 0.0f,  // wide-gamut red
        -0.25f, 0.5f, 2.0f, // signed shadow and highlight headroom
        0.17f, 0.63f, 0.91f};
    for (float red : {-0.25f, 0.0f, 0.18f, 0.5f, 1.0f, 1.5f, 2.0f})
        for (float green : {-0.25f, 0.0f, 0.18f, 0.5f, 1.0f, 1.5f, 2.0f})
            for (float blue : {-0.25f, 0.0f, 0.18f, 0.5f, 1.0f, 1.5f, 2.0f}) {
                samples.push_back(red);
                samples.push_back(green);
                samples.push_back(blue);
            }
    const auto width = static_cast<std::uint32_t>(samples.size() / 3);
    auto pro = std::make_shared<RasterSourceNode>(RasterImage(
        {width, 1, 0, WorkingSpace::LinearProPhotoD50}, samples));
    auto rec = std::make_shared<WorkingSpaceConvertNode>(
        pro, WorkingSpace::LinearRec2020D65);
    WorkingSpaceConvertNode back(rec, WorkingSpace::LinearProPhotoD50);
    const Tile converted = rec->render({0, 0, width, 1});
    const Tile roundtrip = back.render({0, 0, width, 1});
    require(converted.descriptor == ImageDescriptor::scene_linear(
                WorkingSpace::LinearRec2020D65) &&
            roundtrip.descriptor == ImageDescriptor::scene_linear(
                WorkingSpace::LinearProPhotoD50),
            "working-space conversion descriptor is wrong");
    for (int channel = 0; channel < 3; ++channel)
        near(converted.rgb[channel], 0.5f, "D50/D65 conversion changed neutral");
    for (std::size_t i = 0; i < samples.size(); ++i)
        require(std::abs(roundtrip.rgb[i] - samples[i]) < 1e-5f,
                "working-space float32 round trip lost color or headroom");
    const Tile direct_preview = WorkingToSrgbNode(pro).render({0, 0, width, 1});
    const Tile converted_preview = WorkingToSrgbNode(rec).render({0, 0, width, 1});
    for (std::size_t i = 0; i < samples.size(); ++i)
        require(std::abs(direct_preview.rgb[i] - converted_preview.rgb[i]) < 1e-5f,
                "working-space conversion changed linear sRGB output");
    require(converted.rgb[8] > 1.0f && converted.rgb[6] < 0.0f,
            "working-space conversion clipped signed/over-range data");

    WorkingSpaceConvertNode identity(pro, WorkingSpace::LinearProPhotoD50);
    require(identity.render({0, 0, width, 1}).rgb == samples,
            "same-space conversion changed source values");
    require(RasterMetadata{}.working_space == WorkingSpace::LinearProPhotoD50 &&
            CameraColorTransform{}.target == WorkingSpace::LinearProPhotoD50,
            "canonical working-space defaults changed");
    rejects([&] {
        WorkingSpaceConvertNode bad(std::make_shared<ConstantCameraNode>(
            std::array<float, 3>{1, 1, 1}), WorkingSpace::LinearRec2020D65);
    }, "working-space conversion accepted camera RGB");
    rejects([&] {
        WorkingSpaceConvertNode bad(pro, static_cast<WorkingSpace>(99));
    }, "working-space conversion accepted unknown target");
}

void test_srgb_preview() {
    auto rec_white = std::make_shared<RasterSourceNode>(RasterImage(
        {1, 1, 0, WorkingSpace::LinearRec2020D65}, {0.5f, 0.5f, 0.5f}));
    const Tile white = WorkingToSrgbNode(rec_white).render({0, 0, 1, 1});
    require(white.descriptor == ImageDescriptor::linear_srgb(),
            "linear sRGB descriptor is wrong");
    auto preview_linear = std::make_shared<WorkingToSrgbNode>(rec_white);
    require(ToneCurveNode(preview_linear, 0.25f, 1.0f).output_descriptor() ==
                ImageDescriptor::display_linear_srgb(),
            "preview tone stage is not tagged display-linear sRGB");
    for (float channel : white.rgb)
        near(channel, 0.5f, "Rec.2020 neutral changed during linear sRGB conversion");

    auto rec_red = std::make_shared<RasterSourceNode>(RasterImage(
        {1, 1, 0, WorkingSpace::LinearRec2020D65}, {1.0f, 0.0f, 0.0f}));
    const Tile converted = WorkingToSrgbNode(rec_red).render({0, 0, 1, 1});
    // W3C linear Rec.2020->XYZ D65->linear sRGB matrix reference.
    require(std::abs(converted.rgb[0] - 1.660491f) < 0.001f &&
            std::abs(converted.rgb[1] + 0.124550f) < 0.001f &&
            std::abs(converted.rgb[2] + 0.018151f) < 0.001f,
            "linear sRGB conversion clipped or disagrees with W3C matrices");

    auto pro_white = std::make_shared<RasterSourceNode>(RasterImage(
        {1, 1, 0, WorkingSpace::LinearProPhotoD50}, {0.5f, 0.5f, 0.5f}));
    for (float channel : WorkingToSrgbNode(pro_white).render({0, 0, 1, 1}).rgb)
        near(channel, 0.5f, "ProPhoto neutral did not adapt to sRGB neutral");
    auto pro_red = std::make_shared<RasterSourceNode>(RasterImage(
        {1, 1, 0, WorkingSpace::LinearProPhotoD50}, {1.0f, 0.0f, 0.0f}));
    const Tile adapted = WorkingToSrgbNode(pro_red).render({0, 0, 1, 1});
    // Rounded ICC ROMM red XYZ, then W3C D50->D65 and XYZ->linear sRGB.
    require(std::abs(adapted.rgb[0] - 2.03438f) < 0.005f &&
            std::abs(adapted.rgb[1] + 0.22872f) < 0.005f &&
            std::abs(adapted.rgb[2] + 0.00858f) < 0.005f,
            "ProPhoto-to-sRGB adaptation disagrees with independent reference");

    auto tone = std::make_shared<ConstantToneSrgbNode>(
        std::array<float, 3>{0.0031308f, 0.18f, 1.2f});
    const Tile encoded = SrgbEncodeNode(tone).render({0, 0, 1, 1});
    require(encoded.descriptor == ImageDescriptor::srgb_output(),
            "sRGB output descriptor is wrong");
    near(encoded.rgb[0], 0.04045f, "sRGB linear branch is wrong");
    near(encoded.rgb[1], 0.461356f, "sRGB power branch is wrong");
    near(encoded.rgb[2], 1.0f, "sRGB output did not clip over-range input");
    const Tile negative = SrgbEncodeNode(std::make_shared<ConstantToneSrgbNode>(
        std::array<float, 3>{-1.0f, 0.0f, 0.0f})).render({0, 0, 1, 1});
    near(negative.rgb[0], 0.0f, "sRGB output did not clip negative input");

    std::vector<float> raster(7 * 5 * 3, 0.25f);
    GraphRecipe recipe;
    recipe.output_mode = OutputMode::SrgbPreview;
    ImageGraph graph(RasterImage({7, 5, 0, WorkingSpace::LinearRec2020D65},
                                std::move(raster)), recipe);
    require(graph.output().output_descriptor() == ImageDescriptor::srgb_output(),
            "preview graph output was not tagged sRGB");
    Renderer renderer;
    const Tile full = renderer.render_image(graph, {0, 0, 7, 5}, 7);
    const Tile roi = renderer.render_image(graph, {1, 1, 3, 3}, 2);
    for (std::uint32_t y = 0; y < 3; ++y)
        for (std::uint32_t x = 0; x < 3; ++x)
            for (std::uint32_t c = 0; c < 3; ++c)
                near(roi.rgb[(y * 3 + x) * 3 + c],
                     full.rgb[((y + 1) * 7 + x + 1) * 3 + c],
                     "tiled sRGB preview differs from full render");
    rejects([&] { ImageGraph bad(color_bayer(2, 2), recipe); },
            "RAW sRGB preview accepted unknown camera color space");
    recipe.camera_color = CameraColorTransform{{1, 0, 0, 0, 1, 0, 0, 0, 1},
                                               WorkingSpace::LinearRec2020D65};
    require(ImageGraph(color_bayer(2, 2), recipe).output().output_descriptor() ==
                ImageDescriptor::srgb_output(),
            "calibrated RAW sRGB preview failed");
    rejects_domain([] {
        SrgbEncodeNode bad(std::make_shared<ConstantToneSrgbNode>(
            std::array<float, 3>{std::numeric_limits<float>::quiet_NaN(), 0, 0}));
        bad.render({0, 0, 1, 1});
    }, "sRGB output accepted non-finite input");
}

void test_icc_adapter_boundary() {
    auto input = std::make_shared<ConstantToneSrgbNode>(
        std::array<float, 3>{-0.25f, 0.4f, 1.25f});
    auto transform = std::make_shared<MockIccTransform>();
    IccDisplayNode node(input, transform);
    const auto tile = node.render({0, 0, 2, 1});
    require(tile.descriptor == ImageDescriptor::icc_display(transform->profile_sha256()),
            "ICC output lost exact profile identity");
    near(tile.rgb[0], 0.0f, "ICC adapter did not run");
    near(tile.rgb[1], 0.4f, "ICC adapter changed in-gamut channel");
    near(tile.rgb[2], 1.0f, "ICC adapter did not bound output");
    rejects([&] { IccDisplayNode bad(input, nullptr); },
            "ICC node accepted missing backend");
    rejects([&] { IccDisplayNode bad(input, std::make_shared<MockIccTransform>(false, true)); },
            "ICC node accepted absent profile identity");
    rejects([&] { IccDisplayNode bad(std::make_shared<ConstantCameraNode>(
        std::array<float, 3>{1, 1, 1}), transform); },
            "ICC node accepted camera-linear input");
    rejects_domain([&] { IccDisplayNode bad(input, std::make_shared<MockIccTransform>(true));
                         bad.render({0, 0, 1, 1}); },
                   "ICC node accepted non-finite backend output");
    rejects_domain([&] { IccDisplayNode bad(std::make_shared<ConstantToneSrgbNode>(
        std::array<float, 3>{std::numeric_limits<float>::quiet_NaN(), 0, 0}), transform);
                         bad.render({0, 0, 1, 1}); },
                   "ICC backend received non-finite input");

    GraphRecipe recipe;
    recipe.output_mode = OutputMode::IccDisplay;
    auto raster = RasterImage({7, 5, 0, WorkingSpace::LinearRec2020D65},
                              std::vector<float>(7 * 5 * 3, 0.5f));
    ImageGraph graph(raster, recipe, transform);
    Renderer renderer;
    const auto full = renderer.render_image(graph, {0, 0, 7, 5}, 7);
    const auto roi = renderer.render_image(graph, {1, 1, 3, 3}, 2);
    require(full.descriptor == roi.descriptor &&
            roi.descriptor == ImageDescriptor::icc_display(transform->profile_sha256()),
            "ICC graph lost profile identity across tiled ROI");
    for (std::uint32_t y = 0; y < 3; ++y)
        for (std::uint32_t x = 0; x < 3; ++x)
            for (std::uint32_t c = 0; c < 3; ++c)
                near(roi.rgb[(y * 3 + x) * 3 + c],
                     full.rgb[((y + 1) * 7 + x + 1) * 3 + c],
                     "ICC tiled ROI differs from full render");
    rejects([&] { ImageGraph bad(raster, recipe); },
            "ICC graph accepted missing backend");
    recipe.output_mode = OutputMode::LegacyBounded;
    rejects([&] { ImageGraph bad(raster, recipe, transform); },
            "non-ICC graph silently ignored backend");
}

void test_invalid_input() {
    rejects([] { RawImage bad(2, 2, {1, 2, 3}); }, "short Bayer buffer accepted");
    rejects([] { RawImage bad(2, 2, {1, 2, 3, 4}, BayerPattern::RGGB, 10, 10); },
            "invalid black/white levels accepted");
    rejects([] { WhiteBalanceNode bad(nullptr, 1, 1, 1); }, "null node accepted");
    rejects([] { ExposureNode bad(nullptr, 1); }, "null exposure node accepted");
    rejects([] {
        RawMetadata m; m.width = 2; m.height = 2; m.row_stride_samples = 1;
        RawImage bad(m, {1, 2, 3, 4});
    }, "short RAW stride accepted");
    rejects([] {
        RawMetadata m; m.width = 2; m.height = 2; m.active_area = {1, 1, 2, 2};
        RawImage bad(m, {1, 2, 3, 4});
    }, "out-of-bounds active area accepted");
    rejects([] {
        RawMetadata m; m.width = 2; m.height = 2; m.white_levels[2] = 0;
        RawImage bad(m, {1, 2, 3, 4});
    }, "invalid site levels accepted");
    rejects([] {
        RawMetadata m; m.width = 2; m.height = 2; m.cfa_phase_x = 2;
        RawImage bad(m, {1, 2, 3, 4});
    }, "invalid CFA phase accepted");
    rejects([] {
        RawMetadata m; m.width = 2; m.height = 2; m.row_stride_samples = 3;
        RawImage bad(m, {1, 2, 3, 4});
    }, "padded RAW with insufficient samples accepted");
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
        test_decoded_raw_metadata();
        test_camera_color_transform();
        test_scene_linear_raster_source();
        test_working_space_conversion();
        test_srgb_preview();
        test_icc_adapter_boundary();
        test_invalid_input();
        std::cout << "RawEngine core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

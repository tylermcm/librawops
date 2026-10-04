#include "LittleCmsBackend.hpp"
#include "EditGraph.hpp"

#include <lcms2.h>

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
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

std::vector<std::uint8_t> checked_in_srgb_profile() {
    std::ifstream file(RAWENGINE_TEST_PROFILE_PATH, std::ios::binary);
    if (!file) throw std::runtime_error("checked-in ICC profile is missing");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t> synthetic_rgb_profile(cmsCIExyY white,
                                                 cmsCIExyYTRIPLE primaries,
                                                 double gamma) {
    cmsToneCurve* curves[3]{};
    for (auto& curve : curves) curve = cmsBuildGamma(nullptr, gamma);
    if (!curves[0] || !curves[1] || !curves[2])
        throw std::runtime_error("test gamma creation failed");
    cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves);
    for (auto* curve : curves) cmsFreeToneCurve(curve);
    if (!profile) throw std::runtime_error("test RGB profile creation failed");
    cmsUInt32Number size = 0;
    if (!cmsSaveProfileToMem(profile, nullptr, &size) || !size) {
        cmsCloseProfile(profile);
        throw std::runtime_error("test RGB profile size query failed");
    }
    std::vector<std::uint8_t> bytes(size);
    const bool saved = cmsSaveProfileToMem(profile, bytes.data(), &size) != 0;
    cmsCloseProfile(profile);
    if (!saved) throw std::runtime_error("test RGB profile serialization failed");
    bytes.resize(size);
    return bytes;
}

void test_import_roundtrip(const std::vector<std::uint8_t>& profile,
                           WorkingSpace target) {
    constexpr std::uint32_t width = 11, height = 7, stride = 13;
    std::vector<std::uint16_t> encoded(stride * height * 3);
    std::vector<float> original(width * height * 3);
    auto to_profile = make_lcms_display_transform(profile);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            float rgb[3]{0.08f + x * 0.067f, 0.12f + y * 0.085f,
                         0.14f + (x + y) * 0.032f};
            for (int c = 0; c < 3; ++c) original[(y * width + x) * 3 + c] = rgb[c];
            to_profile->apply(rgb, 1);
            for (int c = 0; c < 3; ++c)
                encoded[(y * stride + x) * 3 + c] =
                    static_cast<std::uint16_t>(std::lround(rgb[c] * 65535.0f));
        }
    }
    auto source = make_lcms_raster_source({width, height, stride, target},
                                           encoded, profile);
    require(source->output_descriptor() == ImageDescriptor::scene_linear(target),
            "ICC raster source has wrong scene-linear descriptor");
    auto srgb = WorkingToSrgbNode(source);
    const auto full = srgb.render({0, 0, width, height});
    const auto roi = srgb.render({2, 1, 5, 4});
    for (std::uint32_t y = 0; y < roi.bounds.height; ++y)
        for (std::uint32_t x = 0; x < roi.bounds.width; ++x)
            for (int c = 0; c < 3; ++c) {
                const auto index = ((y + 1) * width + x + 2) * 3 + c;
                const auto roi_index = (y * roi.bounds.width + x) * 3 + c;
                require(std::abs(roi.rgb[roi_index] - full.rgb[index]) < 1e-6f,
                        "ICC raster ROI differs from full conversion");
            }
    for (std::size_t i = 0; i < original.size(); ++i)
        require(std::abs(full.rgb[i] - original[i]) < 0.006f,
                "ICC profile output/import round-trip exceeded tolerance");
    GraphRecipe recipe;
    recipe.output_mode = OutputMode::SrgbPreview;
    ImageGraph graph(source, {0, 0, width, height}, recipe);
    const auto graph_roi = Renderer().render_image(graph, {2, 1, 5, 4}, 2);
    require(graph_roi.descriptor == ImageDescriptor::srgb_output() &&
            graph_roi.rgb.size() == 5 * 4 * 3,
            "ICC raster source cannot feed the demand-driven graph");
    try {
        source->render({width, 0, 1, 1});
        throw std::runtime_error("out-of-bounds ICC raster ROI was accepted");
    } catch (const std::out_of_range&) {}
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

void test_saved_display_binding(const std::vector<std::uint8_t>& profile, WorkingSpace space) {
    const Rect bounds{0, 0, 7, 5};
    std::vector<std::uint16_t> samples(7 * 5 * 3);
    for (unsigned i = 0; i < samples.size(); ++i) samples[i] = std::uint16_t(i * 7919 % 65536);
    auto input = make_lcms_raster_source({7, 5, 0, space}, samples, profile);
    auto transform = make_lcms_display_transform(profile);
    auto linear = std::make_shared<WorkingToSrgbNode>(input);
    auto tone = std::make_shared<ToneCurveNode>(linear, .456789123f, 1.23456789f);
    IccDisplayNode direct(tone, transform);
    const auto working = space == WorkingSpace::LinearProPhotoD50
        ? EditDomain::SceneLinearProPhotoD50 : EditDomain::SceneLinearRec2020D65;
    EditSource source; source.id = "98000000-0000-0000-0000-000000000001";
    source.kind = EditSourceKind::IccRasterU16; source.working_space = space;
    source.content_sha256 = *input->source_fingerprint(); source.icc_input = input->input_icc_identity();
    EditManifest manifest; manifest.working_space = space; manifest.sources = {source};
    manifest.output_profile = transform->output_icc_identity();
    EditOperation convert; convert.id = "98000000-0000-0000-0000-000000000002";
    convert.type_id = "rawengine.working_to_srgb"; convert.processing_version = 2;
    convert.input_domain = working; convert.output_domain = EditDomain::SceneLinearSrgb;
    convert.inputs = {{"image", source.id}};
    EditOperation mapped; mapped.id = "98000000-0000-0000-0000-000000000003";
    mapped.type_id = "rawengine.tone_curve"; mapped.processing_version = 2;
    mapped.input_domain = EditDomain::SceneLinearSrgb; mapped.output_domain = EditDomain::DisplayLinearSrgb;
    mapped.inputs = {{"image", convert.id}};
    mapped.parameters = {{"shoulder", EditValue{.456789123}}, {"gamma", EditValue{1.23456789}}};
    EditOperation display; display.id = "98000000-0000-0000-0000-000000000004";
    display.type_id = "rawengine.icc_display"; display.processing_version = 2;
    display.input_domain = EditDomain::DisplayLinearSrgb; display.output_domain = EditDomain::DisplayEncodedIcc;
    display.inputs = {{"image", mapped.id}};
    manifest.operations = {convert, mapped, display}; manifest.output_id = display.id;
    const std::vector<BoundEditSource> bindings{{source, input, bounds}};
    auto cache = std::make_shared<TileCache>(1 << 20);
    auto parsed = parse_edit_manifest(serialize_edit_manifest(manifest));
    ExecutableEditGraph graph(parsed, bindings, transform, cache);
    auto same = [&](const Tile& actual, const Tile& expected) {
        require(actual.bounds.x == expected.bounds.x && actual.bounds.y == expected.bounds.y &&
                actual.bounds.width == expected.bounds.width && actual.bounds.height == expected.bounds.height &&
                actual.descriptor == expected.descriptor && actual.rgb.size() == expected.rgb.size(),
                "real ICC saved/direct tile metadata differs");
        require(!std::memcmp(actual.rgb.data(), expected.rgb.data(), actual.rgb.size() * sizeof(float)),
                "real ICC saved/direct pixels differ");
    };
    for (const auto roi : {bounds, Rect{1, 1, 4, 3}, Rect{6, 4, 1, 1}}) {
        const auto expected = direct.render(roi);
        same(graph.output().render(roi), expected);
        const auto misses = cache->stats().misses;
        same(graph.output().render(roi), expected);
        require(cache->stats().misses == misses, "real ICC saved warm cache missed");
        const auto regions = graph.required_source_regions(roi);
        require(regions.size() == 1 && regions.at(source.id).x == roi.x && regions.at(source.id).y == roi.y &&
                regions.at(source.id).width == roi.width && regions.at(source.id).height == roi.height,
                "real ICC saved source footprint differs");
    }
    EditHistory history(parsed, bindings, {4, 1 << 20}, transform, cache);
    auto changed = parsed; changed.operations[1].parameters["shoulder"] = EditValue{.789123456};
    const auto first = history.current()->id, second = history.commit(changed);
    auto restored = EditHistory::restore(history.serialize(), bindings, transform, cache);
    same(restored->revision(first)->graph->output().render(bounds), direct.render(bounds));
    IccDisplayNode changed_direct(std::make_shared<ToneCurveNode>(linear, .789123456f, 1.23456789f), transform);
    same(restored->revision(second)->graph->output().render(bounds), changed_direct.render(bounds));
    auto encoded = parsed;
    encoded.output_profile.reset();
    encoded.operations.back().type_id = "rawengine.srgb_encode";
    encoded.operations.back().output_domain = EditDomain::DisplayEncodedSrgb;
    const auto third = history.commit(encoded);
    require(history.undo() == second && history.redo() == third, "mixed ICC history navigation differs");
    auto mixed = EditHistory::restore(history.serialize(), bindings, transform, cache);
    SrgbEncodeNode srgb(tone);
    same(mixed->revision(third)->graph->output().render(bounds), srgb.render(bounds));
    same(mixed->revision(first)->graph->output().render(bounds), direct.render(bounds));
    for (const auto level : {RenderLevel{1, RenderQuality::Preview}, RenderLevel{2, RenderQuality::Preview}}) {
        require(!graph.output().supports_level(level), "real ICC saved graph gained reduced support");
        try {
            graph.output().render_level({0, 0, 1, 1}, level);
            throw std::runtime_error("real ICC saved graph rendered reduced output");
        } catch (const std::invalid_argument&) {}
    }
    std::cout << "Real ICC saved/direct/cache/footprint/history binding verified in space "
              << unsigned(space) << '\n';
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
        const auto real_srgb = checked_in_srgb_profile();
        require(real_srgb.size() > 128, "real ICC fixture is empty");
        const cmsCIExyY d65{0.3127, 0.3290, 1.0};
        const cmsCIExyYTRIPLE p3{{0.680, 0.320, 1.0}, {0.265, 0.690, 1.0},
                                  {0.150, 0.060, 1.0}};
        const cmsCIExyYTRIPLE adobe_rgb{{0.640, 0.330, 1.0},
                                        {0.210, 0.710, 1.0},
                                        {0.150, 0.060, 1.0}};
        const auto synthetic_p3 = synthetic_rgb_profile(d65, p3, 2.2);
        const auto synthetic_adobe = synthetic_rgb_profile(d65, adobe_rgb, 2.2);
        for (const auto& profile : {bytes, real_srgb, synthetic_p3, synthetic_adobe}) {
            test_import_roundtrip(profile, WorkingSpace::LinearProPhotoD50);
            test_import_roundtrip(profile, WorkingSpace::LinearRec2020D65);
            test_saved_display_binding(profile, WorkingSpace::LinearProPhotoD50);
            test_saved_display_binding(profile, WorkingSpace::LinearRec2020D65);
        }
        try {
            make_lcms_raster_source({2, 2, 1}, std::vector<std::uint16_t>(12), bytes);
            throw std::runtime_error("invalid ICC raster stride was accepted");
        } catch (const std::invalid_argument&) {}
        try {
            make_lcms_raster_source({1, 1, 0}, std::vector<std::uint16_t>(3),
                                    std::vector<std::uint8_t>(128));
            throw std::runtime_error("invalid ICC raster profile was accepted");
        } catch (const std::invalid_argument&) {}
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

        auto icc_source = make_lcms_raster_source(
            {7, 5, 0, WorkingSpace::LinearProPhotoD50},
            std::vector<std::uint16_t>(7 * 5 * 3, 32768), real_srgb);
        const auto icc_padded = fingerprint_icc_raster_source(
            {2, 1, 3, WorkingSpace::LinearProPhotoD50},
            {1, 2, 3, 4, 5, 6, 99, 99, 99});
        const auto icc_packed = fingerprint_icc_raster_source(
            {2, 1, 0, WorkingSpace::LinearProPhotoD50},
            {1, 2, 3, 4, 5, 6});
        require(icc_padded == icc_packed, "ICC raster padding changed fingerprint");
        require(icc_packed != fingerprint_icc_raster_source(
                    {2, 1, 0, WorkingSpace::LinearProPhotoD50},
                    {1, 2, 3, 4, 5, 7}),
                "ICC raster pixel change did not change fingerprint");
        EditSource source_record;
        source_record.id = "00000000-0000-0000-0000-000000000001";
        source_record.kind = EditSourceKind::IccRasterU16;
        source_record.working_space = WorkingSpace::LinearProPhotoD50;
        source_record.content_sha256 = *icc_source->source_fingerprint();
        ResizeNode icc_resize(icc_source, {0, 0, 7, 5}, 3, 2, ResizeFilter::Area);
        require(icc_resize.render({0, 0, 3, 2}).rgb.size() == 18,
                "native area resize rejected ICC-imported working pixels");
        require(!icc_resize.supports_level({1, RenderQuality::Preview}),
                "resize bypassed ICC reduced-input gate");
        OrientationNode icc_orientation(icc_source, {0, 0, 7, 5}, 1, true);
        require(icc_orientation.render({0, 0, 5, 7}).rgb.size() == 105 &&
                !icc_orientation.supports_level({1, RenderQuality::Preview}), "ICC orientation changed native/reduced gates");
        try {
            icc_resize.render_level({0, 0, 1, 1}, {1, RenderQuality::Preview});
            throw std::runtime_error("ICC resized preview was accepted");
        } catch (const std::invalid_argument&) {}
        source_record.icc_input = icc_source->input_icc_identity();
        require(source_record.icc_input.has_value(),
                "LittleCMS source did not expose its input policy");
        auto icc_output = make_lcms_display_transform(real_srgb);
        require(icc_output->output_icc_identity().has_value(),
                "LittleCMS output did not expose its policy");
        EditManifest edit;
        edit.sources.push_back(source_record);
        edit.output_profile = icc_output->output_icc_identity();
        EditOperation convert;
        convert.id = "00000000-0000-0000-0000-000000000002";
        convert.type_id = "rawengine.working_to_srgb";
        convert.processing_version = 2;
        convert.input_domain = EditDomain::SceneLinearProPhotoD50;
        convert.output_domain = EditDomain::SceneLinearSrgb;
        convert.inputs.emplace("image", source_record.id);
        edit.operations.push_back(convert);
        EditOperation tone_op;
        tone_op.id = "00000000-0000-0000-0000-000000000003";
        tone_op.type_id = "rawengine.tone_curve";
        tone_op.processing_version = 2;
        tone_op.input_domain = EditDomain::SceneLinearSrgb;
        tone_op.output_domain = EditDomain::DisplayLinearSrgb;
        tone_op.parameters = {{"shoulder", EditValue{0.25}}, {"gamma", EditValue{1.0}}};
        tone_op.inputs.emplace("image", convert.id);
        edit.operations.push_back(tone_op);
        EditOperation display_op;
        display_op.id = "00000000-0000-0000-0000-000000000004";
        display_op.type_id = "rawengine.icc_display";
        display_op.processing_version = 2;
        display_op.input_domain = EditDomain::DisplayLinearSrgb;
        display_op.output_domain = EditDomain::DisplayEncodedIcc;
        display_op.inputs.emplace("image", tone_op.id);
        edit.operations.push_back(display_op);
        edit.output_id = display_op.id;
        auto binding = BoundEditSource{source_record, icc_source, {0, 0, 7, 5}};
        auto executable = ExecutableEditGraph(
            parse_edit_manifest(serialize_edit_manifest(edit)), {binding}, icc_output);
        const auto output_tile = Renderer().render_image(executable, {1, 1, 4, 3}, 2);
        require(output_tile.descriptor.profile_sha256 ==
                    icc_output->profile_sha256() && output_tile.rgb.size() == 36,
                "manifest ICC graph failed to render a bounded ROI");
        auto wrong_input = edit;
        wrong_input.sources[0].icc_input->profile_sha256[0] ^= 1;
        auto wrong_binding = binding;
        wrong_binding.identity = wrong_input.sources[0];
        try {
            ExecutableEditGraph(wrong_input, {wrong_binding}, icc_output);
            throw std::runtime_error("ICC source policy mismatch was accepted");
        } catch (const std::invalid_argument&) {}
        auto wrong_pixels = edit;
        wrong_pixels.sources[0].content_sha256[0] ^= 1;
        auto wrong_pixels_binding = binding;
        wrong_pixels_binding.identity = wrong_pixels.sources[0];
        try {
            ExecutableEditGraph(wrong_pixels, {wrong_pixels_binding}, icc_output);
            throw std::runtime_error("ICC raster content mismatch was accepted");
        } catch (const std::invalid_argument&) {}
        auto wrong_output = edit;
        wrong_output.output_profile->black_point_compensation = true;
        try {
            ExecutableEditGraph(wrong_output, {binding}, icc_output);
            throw std::runtime_error("ICC output policy mismatch was accepted");
        } catch (const std::invalid_argument&) {}
        std::cout << "LittleCMS core backend tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

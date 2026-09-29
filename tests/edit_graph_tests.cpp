#include "EditGraph.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace rawengine;

namespace {

constexpr const char* source_id = "00000000-0000-0000-0000-000000000001";
constexpr const char* exposure_id = "00000000-0000-0000-0000-000000000010";
constexpr const char* future_id = "00000000-0000-0000-0000-000000000020";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename F>
void rejects(F&& action, const char* message) {
    try { action(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}

IccProfileIdentity icc_identity(std::uint8_t marker) {
    IccProfileIdentity result;
    result.profile_sha256.fill(marker);
    result.intent = "relative_colorimetric";
    return result;
}

EditManifest example() {
    EditManifest manifest;
    EditSource source;
    source.id = source_id;
    source.kind = EditSourceKind::IccRasterU16;
    source.working_space = WorkingSpace::LinearProPhotoD50;
    source.content_sha256.fill(0xa5);
    source.icc_input = icc_identity(0x1f);
    manifest.sources.push_back(source);

    EditOperation exposure;
    exposure.id = exposure_id;
    exposure.type_id = "rawengine.exposure";
    exposure.processing_version = 2;
    exposure.parameters.emplace("stops", EditValue{1.25});
    exposure.inputs.emplace("image", source_id);
    manifest.operations.push_back(exposure);

    EditOperation future;
    future.id = future_id;
    future.type_id = "vendor.future.denoise";
    future.schema_version = 7;
    future.processing_version = 2;
    future.opacity = 0.75;
    future.output_domain = EditDomain::DisplayEncodedIcc;
    future.parameters.emplace("radius", EditValue{std::int64_t{3}});
    future.parameters.emplace("strategy", EditValue{std::string("edge-aware")});
    future.inputs.emplace("image", exposure_id);
    future.extra_fields.emplace("future_payload", EditValue{EditValue::Object{
        {"labels", EditValue{EditValue::Array{EditValue{std::string("café")},
                                                EditValue{true}}}},
        {"nested", EditValue{EditValue::Object{{"key", EditValue{nullptr}}}}}}});
    manifest.operations.push_back(future);
    manifest.output_id = future_id;
    manifest.output_profile = icc_identity(0x3a);
    manifest.extra_fields.emplace("future_manifest_field", EditValue{std::string("kept")});
    return manifest;
}

std::string uuid(unsigned index) {
    std::string result = "00000000-0000-0000-0000-000000000000";
    constexpr char hex[] = "0123456789abcdef";
    result[34] = hex[(index >> 4) & 15];
    result[35] = hex[index & 15];
    return result;
}

EditOperation operation(unsigned id, std::string type, EditDomain input,
                        EditDomain output, std::string upstream,
                        EditValue::Object parameters = {}) {
    EditOperation op;
    op.id = uuid(id);
    op.type_id = std::move(type);
    op.processing_version = kCurrentEditProcessingVersion;
    op.input_domain = input;
    op.output_domain = output;
    op.inputs.emplace("image", std::move(upstream));
    op.parameters = std::move(parameters);
    return op;
}

void same_tile(const Tile& actual, const Tile& expected, const char* message) {
    require(actual.descriptor == expected.descriptor &&
            actual.bounds.x == expected.bounds.x &&
            actual.bounds.y == expected.bounds.y &&
            actual.bounds.width == expected.bounds.width &&
            actual.bounds.height == expected.bounds.height &&
            actual.rgb.size() == expected.rgb.size(), message);
    for (std::size_t i = 0; i < actual.rgb.size(); ++i)
        require(std::abs(actual.rgb[i] - expected.rgb[i]) < 1e-6f, message);
}

void test_executable_raw() {
    constexpr std::uint32_t width = 13, height = 9;
    std::vector<std::uint16_t> samples(width * height);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            samples[y * width + x] = static_cast<std::uint16_t>(1000 + x * 1900 + y * 2300);
    RawImage raw(width, height, std::move(samples));
    GraphRecipe recipe;
    recipe.red_gain = 1.15f;
    recipe.green_gain = 0.95f;
    recipe.blue_gain = 1.3f;
    recipe.exposure_stops = 0.75f;
    recipe.tone_shoulder = 0.35f;
    recipe.tone_gamma = 1.1f;
    recipe.output_mode = OutputMode::SrgbPreview;
    CameraColorTransform camera;
    camera.camera_to_xyz_d50 = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    recipe.camera_color = camera;
    ImageGraph fixed(raw, recipe);

    EditSource source;
    source.id = uuid(1);
    source.kind = EditSourceKind::DecodedBayerU16;
    source.content_sha256.fill(0x51);
    EditManifest manifest;
    manifest.sources.push_back(source);
    manifest.operations.push_back(operation(2, "rawengine.white_balance",
        EditDomain::CameraLinear, EditDomain::CameraLinear, source.id,
        {{"red_gain", EditValue{static_cast<double>(recipe.red_gain)}},
         {"green_gain", EditValue{static_cast<double>(recipe.green_gain)}},
         {"blue_gain", EditValue{static_cast<double>(recipe.blue_gain)}}}));
    manifest.operations.push_back(operation(3, "rawengine.exposure",
        EditDomain::CameraLinear, EditDomain::CameraLinear, uuid(2),
        {{"stops", EditValue{static_cast<double>(recipe.exposure_stops)}}}));
    EditValue::Array matrix;
    for (double value : camera.camera_to_xyz_d50) matrix.emplace_back(value);
    manifest.operations.push_back(operation(4, "rawengine.camera_to_working",
        EditDomain::CameraLinear, EditDomain::SceneLinearProPhotoD50, uuid(3),
        {{"matrix", EditValue{std::move(matrix)}}}));
    manifest.operations.push_back(operation(5, "rawengine.working_to_srgb",
        EditDomain::SceneLinearProPhotoD50, EditDomain::SceneLinearSrgb, uuid(4)));
    manifest.operations.push_back(operation(6, "rawengine.tone_curve",
        EditDomain::SceneLinearSrgb, EditDomain::DisplayLinearSrgb, uuid(5),
        {{"shoulder", EditValue{static_cast<double>(recipe.tone_shoulder)}},
         {"gamma", EditValue{static_cast<double>(recipe.tone_gamma)}}}));
    manifest.operations.push_back(operation(7, "rawengine.srgb_encode",
        EditDomain::DisplayLinearSrgb, EditDomain::DisplayEncodedSrgb, uuid(6)));
    manifest.output_id = uuid(7);
    std::reverse(manifest.operations.begin(), manifest.operations.end());
    const auto serialized = serialize_edit_manifest(manifest);
    auto graph = ExecutableEditGraph(parse_edit_manifest(serialized),
        {{source, std::make_shared<RawUnpackNode>(raw), {0, 0, width, height}}});
    Renderer renderer;
    for (Rect roi : {Rect{0, 0, width, height}, Rect{2, 1, 7, 6}, Rect{12, 8, 1, 1}})
        same_tile(renderer.render_image(graph, roi, 3),
                  renderer.render_image(fixed, roi, 3),
                  "manifest RAW render differs from fixed graph");

    auto wrong_source = source;
    wrong_source.content_sha256[0] ^= 1;
    rejects([&] {
        ExecutableEditGraph(manifest,
            {{wrong_source, std::make_shared<RawUnpackNode>(raw), {0, 0, width, height}}});
    }, "wrong source fingerprint was accepted");
    auto wrong_domain = manifest;
    wrong_domain.operations.front().input_domain = EditDomain::CameraLinear;
    rejects([&] {
        ExecutableEditGraph(wrong_domain,
            {{source, std::make_shared<RawUnpackNode>(raw), {0, 0, width, height}}});
    }, "wrong declared input domain was accepted");
    auto unknown = manifest;
    unknown.operations.front().type_id = "vendor.unknown.operation";
    require(serialize_edit_manifest(parse_edit_manifest(serialize_edit_manifest(unknown))) ==
            serialize_edit_manifest(unknown), "unknown operation did not survive save");
    rejects([&] {
        ExecutableEditGraph(unknown,
            {{source, std::make_shared<RawUnpackNode>(raw), {0, 0, width, height}}});
    }, "unknown operation executed");
    auto wrong_version = manifest;
    wrong_version.operations.front().processing_version = kLegacyRec2020ProcessingVersion;
    rejects([&] {
        ExecutableEditGraph(wrong_version,
            {{source, std::make_shared<RawUnpackNode>(raw), {0, 0, width, height}}});
    }, "mixed processing versions were accepted");

    auto old_source = source;
    auto old_recipe = recipe;
    old_recipe.camera_color->target = WorkingSpace::LinearRec2020D65;
    ImageGraph old_fixed(raw, old_recipe);
    auto old_manifest = snapshot_legacy_recipe(old_source, old_recipe,
        LegacyRecipeEra::ImplicitRec2020, uuid(8));
    auto old_graph = ExecutableEditGraph(parse_edit_manifest(
        serialize_edit_manifest(old_manifest)),
        {{old_source, std::make_shared<RawUnpackNode>(raw), {0, 0, width, height}}});
    same_tile(renderer.render_image(old_graph, {1, 2, 8, 5}, 3),
              renderer.render_image(old_fixed, {1, 2, 8, 5}, 3),
              "legacy Rec.2020 snapshot replay differs from fixed graph");
}

void test_executable_raster() {
    constexpr std::uint32_t width = 8, height = 6;
    std::vector<float> pixels(width * height * 3);
    for (std::size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = static_cast<float>(static_cast<int>(i % 17) - 3) / 12.0f;
    RasterImage image({width, height, 0, WorkingSpace::LinearProPhotoD50}, pixels);
    GraphRecipe recipe;
    recipe.exposure_stops = -0.5f;
    recipe.tone_shoulder = 0.42f;
    recipe.tone_gamma = 0.9f;
    ImageGraph fixed(image, recipe);
    EditSource source;
    source.id = uuid(10);
    source.kind = EditSourceKind::SceneLinearRasterF32;
    source.working_space = WorkingSpace::LinearProPhotoD50;
    source.content_sha256.fill(0x77);
    EditManifest manifest;
    manifest.sources.push_back(source);
    manifest.operations.push_back(operation(11, "rawengine.exposure",
        EditDomain::SceneLinearProPhotoD50, EditDomain::SceneLinearProPhotoD50,
        source.id, {{"stops", EditValue{static_cast<double>(recipe.exposure_stops)}}}));
    manifest.operations.push_back(operation(12, "rawengine.tone_curve",
        EditDomain::SceneLinearProPhotoD50, EditDomain::ToneMappedUnmanaged, uuid(11),
        {{"shoulder", EditValue{static_cast<double>(recipe.tone_shoulder)}},
         {"gamma", EditValue{static_cast<double>(recipe.tone_gamma)}}}));
    manifest.operations.push_back(operation(13, "rawengine.output_clip",
        EditDomain::ToneMappedUnmanaged, EditDomain::UnmanagedBounded, uuid(12)));
    manifest.output_id = uuid(13);
    auto bound = BoundEditSource{source, std::make_shared<RasterSourceNode>(image),
                                 {0, 0, width, height}};
    auto graph = ExecutableEditGraph(manifest, {bound});
    Renderer renderer;
    same_tile(renderer.render_image(graph, {1, 1, 6, 4}, 2),
              renderer.render_image(fixed, {1, 1, 6, 4}, 2),
              "manifest raster render differs from fixed graph");
    std::size_t streamed_pixels = 0;
    renderer.render_tiles(graph, {0, 0, width, height},
        [&](const Tile& tile) { streamed_pixels += tile.bounds.width * tile.bounds.height; }, 3);
    require(streamed_pixels == width * height,
            "manifest graph streaming did not cover the viewport exactly");
    auto bypass = manifest;
    bypass.operations[0].enabled = false;
    GraphRecipe no_exposure = recipe;
    no_exposure.exposure_stops = 0.0f;
    ImageGraph bypass_fixed(image, no_exposure);
    auto bypass_graph = ExecutableEditGraph(bypass, {bound});
    same_tile(renderer.render_image(bypass_graph, {0, 0, width, height}, 3),
              renderer.render_image(bypass_fixed, {0, 0, width, height}, 3),
              "disabled operation did not bypass input");
    auto bad_blend = manifest;
    bad_blend.operations[0].blend_mode = "multiply";
    rejects([&] { ExecutableEditGraph(bad_blend, {bound}); },
            "unsupported blend was silently ignored");
    auto bad_mask = manifest;
    bad_mask.operations[0].masks.emplace("coverage", source.id);
    rejects([&] { ExecutableEditGraph(bad_mask, {bound}); },
            "unsupported mask was silently ignored");

    RasterImage rec_image({width, height, 0, WorkingSpace::LinearRec2020D65}, pixels);
    auto rec_source = source;
    rec_source.working_space = WorkingSpace::LinearRec2020D65;
    auto convert = EditManifest{};
    convert.sources.push_back(rec_source);
    convert.operations.push_back(operation(14, "rawengine.working_space_convert",
        EditDomain::SceneLinearRec2020D65, EditDomain::SceneLinearProPhotoD50,
        rec_source.id));
    convert.output_id = uuid(14);
    auto rec_node = std::make_shared<RasterSourceNode>(rec_image);
    auto convert_graph = ExecutableEditGraph(convert,
        {{rec_source, rec_node, {0, 0, width, height}}});
    same_tile(renderer.render_image(convert_graph, {1, 1, 5, 3}, 2),
              WorkingSpaceConvertNode(rec_node, WorkingSpace::LinearProPhotoD50)
                  .render({1, 1, 5, 3}),
              "manifest working-space conversion differs from direct node");
}

} // namespace

int main() {
    try {
        auto manifest = example();
        const auto encoded = serialize_edit_manifest(manifest);
        const auto parsed = parse_edit_manifest(encoded);
        require(serialize_edit_manifest(parsed) == encoded,
                "edit manifest canonical round trip changed bytes");
        require(parsed.operations[1].extra_fields == manifest.operations[1].extra_fields &&
                parsed.extra_fields == manifest.extra_fields,
                "unknown operation/root fields were lost");
        require(parsed.sources[0].icc_input == manifest.sources[0].icc_input &&
                parsed.output_profile == manifest.output_profile,
                "ICC input/output identities were lost");
        auto prior_v1 = encoded;
        const auto version_field = prior_v1.find("\"format_version\":2");
        require(version_field != std::string::npos, "format version is missing");
        prior_v1.replace(version_field, std::string("\"format_version\":2").size(),
                         "\"format_version\":1");
        const auto source_space = prior_v1.find(",\"working_space\":\"linear_prophoto_d50\"");
        require(source_space != std::string::npos, "source working space is missing");
        prior_v1.erase(source_space,
            std::string(",\"working_space\":\"linear_prophoto_d50\"").size());
        const auto migrated = parse_edit_manifest(prior_v1);
        require(migrated.format_version == 2 &&
                migrated.sources[0].working_space == WorkingSpace::LinearProPhotoD50 &&
                serialize_edit_manifest(migrated) == encoded,
                "format-v1 raster source migration changed its declared color space");
        auto reordered = manifest;
        std::reverse(reordered.operations.begin(), reordered.operations.end());
        require(serialize_edit_manifest(reordered) == encoded,
                "operation insertion order changed canonical JSON");

        auto policy = manifest;
        policy.sources[0].icc_input->black_point_compensation = true;
        require(serialize_edit_manifest(policy) != encoded,
                "input ICC BPC was omitted from serialized identity");
        policy = manifest;
        policy.output_profile->intent = "perceptual";
        require(serialize_edit_manifest(policy) != encoded,
                "output ICC intent was omitted from serialized identity");
        policy = manifest;
        policy.sources[0].icc_input->profile_sha256[0] ^= 1;
        require(serialize_edit_manifest(policy) != encoded,
                "input ICC profile digest was omitted from serialized identity");
        policy = manifest;
        policy.output_profile.reset();
        rejects([&] { validate_edit_manifest(policy); },
                "ICC output without profile identity was accepted");

        auto invalid = manifest;
        invalid.operations[0].inputs["image"] = future_id;
        rejects([&] { validate_edit_manifest(invalid); }, "cycle was accepted");
        invalid = manifest;
        invalid.operations[0].inputs["image"] = "00000000-0000-0000-0000-000000000099";
        rejects([&] { validate_edit_manifest(invalid); }, "dangling edge was accepted");
        invalid = manifest;
        invalid.sources[0].icc_input.reset();
        rejects([&] { validate_edit_manifest(invalid); }, "ICC source without profile was accepted");
        invalid = manifest;
        invalid.sources[0].working_space.reset();
        rejects([&] { validate_edit_manifest(invalid); },
                "raster source without declared working space was accepted");
        invalid = manifest;
        invalid.operations[0].parameters["stops"] =
            EditValue{std::numeric_limits<double>::quiet_NaN()};
        rejects([&] { serialize_edit_manifest(invalid); }, "non-finite parameter was accepted");
        invalid = manifest;
        invalid.operations[0].parameters["stops"] = EditValue{std::string("1.25")};
        rejects([&] { validate_edit_manifest(invalid); }, "wrong parameter type was accepted");
        invalid = manifest;
        invalid.operations[0].id = source_id;
        rejects([&] { validate_edit_manifest(invalid); }, "duplicate UUID was accepted");

        auto missing_version = encoded;
        const auto position = missing_version.rfind(",\"processing_version\":2");
        require(position != std::string::npos, "processing version not serialized");
        missing_version.erase(position, std::string(",\"processing_version\":2").size());
        rejects([&] { parse_edit_manifest(missing_version); },
                "missing processing version was silently defaulted");
        auto missing_space = encoded;
        const auto space = missing_space.rfind(",\"working_space\":\"linear_prophoto_d50\"");
        require(space != std::string::npos, "working space not serialized");
        missing_space.erase(space, std::string(",\"working_space\":\"linear_prophoto_d50\"").size());
        rejects([&] { parse_edit_manifest(missing_space); },
                "missing working space was silently defaulted");
        rejects([&] { parse_edit_manifest("{\"format_version\":1,\"format_version\":1}"); },
                "duplicate JSON key was accepted");
        rejects([&] { parse_edit_manifest(std::string("{\"bad\":\"") + '\xff' + "\"}"); },
                "invalid UTF-8 was accepted");
        rejects([&] { parse_edit_manifest("{\"bad\":\"\\uD800\"}"); },
                "unpaired surrogate was accepted");

        EditSource legacy_source;
        legacy_source.id = source_id;
        legacy_source.kind = EditSourceKind::DecodedBayerU16;
        legacy_source.content_sha256.fill(0xa5);
        GraphRecipe recipe;
        recipe.exposure_stops = 1.0f;
        auto old = snapshot_legacy_recipe(legacy_source, recipe,
                                          LegacyRecipeEra::ImplicitRec2020, exposure_id);
        auto newer = snapshot_legacy_recipe(legacy_source, recipe,
                                            LegacyRecipeEra::ImplicitProPhoto, exposure_id);
        require(old.processing_version == 1 &&
                old.working_space == WorkingSpace::LinearRec2020D65 &&
                newer.processing_version == 2 &&
                newer.working_space == WorkingSpace::LinearProPhotoD50 &&
                serialize_edit_manifest(old) != serialize_edit_manifest(newer),
                "legacy implicit Rec.2020 was silently reinterpreted");
        require(serialize_edit_manifest(parse_edit_manifest(serialize_edit_manifest(old))) ==
                    serialize_edit_manifest(old), "legacy snapshot did not round trip");
        rejects([&] {
            snapshot_legacy_recipe(legacy_source, recipe,
                static_cast<LegacyRecipeEra>(99), exposure_id);
        }, "unknown legacy provenance was accepted");
        recipe.output_mode = OutputMode::IccDisplay;
        rejects([&] {
            snapshot_legacy_recipe(legacy_source, recipe,
                LegacyRecipeEra::ImplicitRec2020, exposure_id);
        }, "legacy ICC output without profile identity was accepted");
        recipe.output_mode = OutputMode::LegacyBounded;
        recipe.camera_color = CameraColorTransform{};
        rejects([&] {
            snapshot_legacy_recipe(legacy_source, recipe,
                LegacyRecipeEra::ImplicitRec2020, exposure_id);
        }, "conflicting legacy camera target was accepted");
        test_executable_raw();
        test_executable_raster();
        std::cout << "Edit graph format tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

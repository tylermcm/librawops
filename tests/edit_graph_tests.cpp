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
        std::cout << "Edit graph format tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

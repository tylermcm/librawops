#include "EditGraph.hpp"
#include "TileScheduler.hpp"

#include <algorithm>
#include <cmath>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
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
    source.content_sha256 = fingerprint_raw_source(raw);
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
    auto altered_samples = raw.samples();
    altered_samples[0] ^= 1;
    RawImage altered_raw(raw.metadata(), std::move(altered_samples));
    rejects([&] {
        ExecutableEditGraph(manifest,
            {{source, std::make_shared<RawUnpackNode>(altered_raw), {0, 0, width, height}}});
    }, "changed RAW samples were accepted under a saved fingerprint");
    rejects([&] {
        ExecutableEditGraph(manifest,
            {{source, std::make_shared<RawUnpackNode>(raw), {0, 0, width - 1, height}}});
    }, "incorrect runtime RAW bounds were accepted");
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
    const auto old_region = old_graph.required_source_region({1, 2, 8, 5});
    require(old_region.x == 0 && old_region.y == 1 &&
            old_region.width == 10 && old_region.height == 7,
            "legacy RAW chain did not plan the demosaic sensor halo");
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
    source.content_sha256 = fingerprint_raster_source(image);
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
    auto cache = std::make_shared<TileCache>(16 * 1024);
    auto cached = ExecutableEditGraph(manifest, {bound}, nullptr, cache);
    const Rect cached_roi{0, 0, width, height};
    const RenderRequest native_request{cached_roi, 2, {}};
    same_tile(renderer.render_image(graph, native_request),
              renderer.render_image(graph, cached_roi, 2),
              "typed native render differs from legacy render");
    std::vector<Tile> requested_tiles;
    renderer.render_tiles(graph, native_request,
        [&](const Tile& tile) { requested_tiles.push_back(tile); });
    require(requested_tiles.size() == 12,
            "typed native request did not preserve tile partitioning");
    auto unsupported_request = native_request;
    unsupported_request.level.mip = 1;
    rejects([&] { renderer.render_image(graph, unsupported_request); },
            "reduced mip rendered before its sampling contract exists");
    unsupported_request = native_request;
    unsupported_request.level.quality = RenderQuality::Preview;
    rejects([&] { renderer.render_image(graph, unsupported_request); },
            "preview quality rendered before its processing contract exists");
    same_tile(renderer.render_image(cached, cached_roi, 2),
              renderer.render_image(graph, cached_roi, 2),
              "cold cached graph differs from uncached graph");
    const auto cold = cache->stats();
    renderer.render_image(cached, cached_roi, 2);
    const auto warm = cache->stats();
    require(warm.hits > cold.hits && warm.misses == cold.misses &&
            warm.used_bytes <= 16 * 1024,
            "warm graph did not reuse bounded cached tiles");
    auto late_edit = manifest;
    late_edit.operations[1].parameters["shoulder"] = EditValue{0.55};
    auto revised = ExecutableEditGraph(late_edit, {bound}, nullptr, cache);
    auto uncached_revised = ExecutableEditGraph(late_edit, {bound});
    same_tile(renderer.render_image(revised, cached_roi, 2),
              renderer.render_image(uncached_revised, cached_roi, 2),
              "late edit reused stale output");
    require(cache->stats().hits > warm.hits,
            "late edit did not reuse unchanged upstream tiles");
    auto early_edit = manifest;
    early_edit.operations[0].parameters["stops"] = EditValue{1.5};
    auto early_graph = ExecutableEditGraph(early_edit, {bound}, nullptr, cache);
    same_tile(renderer.render_image(early_graph, cached_roi, 2),
              renderer.render_image(ExecutableEditGraph(early_edit, {bound}),
                                    cached_roi, 2),
              "early edit reused stale downstream tiles");
    auto changed_pixels = pixels;
    changed_pixels[0] += 0.5f;
    RasterImage changed_image({width, height, 0, WorkingSpace::LinearProPhotoD50},
                              changed_pixels);
    auto changed_source = source;
    changed_source.content_sha256 = fingerprint_raster_source(changed_image);
    auto changed_manifest = manifest;
    changed_manifest.sources[0] = changed_source;
    auto changed_bound = BoundEditSource{changed_source,
        std::make_shared<RasterSourceNode>(changed_image), {0, 0, width, height}};
    auto changed_graph = ExecutableEditGraph(changed_manifest, {changed_bound}, nullptr, cache);
    same_tile(renderer.render_image(changed_graph, cached_roi, 2),
              renderer.render_image(ExecutableEditGraph(changed_manifest, {changed_bound}),
                                    cached_roi, 2),
              "changed source reused stale cached tiles");
    auto tiny_cache = std::make_shared<TileCache>(700);
    auto tiny_graph = ExecutableEditGraph(manifest, {bound}, nullptr, tiny_cache);
    same_tile(renderer.render_image(tiny_graph, cached_roi, 2),
              renderer.render_image(graph, cached_roi, 2),
              "evicting cache changed graph output");
    require(tiny_cache->stats().used_bytes <= 700,
            "tile cache exceeded its byte budget");
    TileCache level_cache(4096);
    std::array<std::uint8_t, 32> level_signature{};
    level_signature[0] = 17;
    const Rect level_roi{0, 0, 1, 1};
    level_cache.render(*bound.node, level_signature, level_roi);
    level_cache.render(*bound.node, level_signature, level_roi);
    require(level_cache.stats().entries == 1 && level_cache.stats().hits == 1,
            "native cache level did not hit its own entry");
    level_cache.render(*bound.node, level_signature, level_roi,
                       {0, RenderQuality::Preview});
    level_cache.render(*bound.node, level_signature, level_roi,
                       {1, RenderQuality::Preview});
    require(level_cache.stats().entries == 3 && level_cache.stats().misses == 3,
            "mip or quality level aliased another cache entry");
    rejects([&] { level_cache.render(*bound.node, level_signature, level_roi,
                                     {32, RenderQuality::Final}); },
            "invalid cache mip level was accepted");
    cache->clear();
    require(cache->stats().entries == 0 && cache->stats().used_bytes == 0,
            "tile cache clear did not invalidate entries");
    CancellationToken pre_cancelled;
    pre_cancelled.cancel();
    try {
        renderer.render_image(cached, cached_roi, 2, &pre_cancelled);
        throw std::runtime_error("pre-cancelled materialized render completed");
    } catch (const RenderCancelled&) {}
    CancellationToken between_tiles;
    std::size_t delivered = 0;
    try {
        renderer.render_tiles(cached, cached_roi,
            [&](const Tile&) { ++delivered; between_tiles.cancel(); },
            2, &between_tiles);
        throw std::runtime_error("streaming render continued after cancellation");
    } catch (const RenderCancelled&) {}
    require(delivered == 1, "cancellation delivered more than one tile");
    TileScheduler concurrent_scheduler(3, 8);
    std::vector<std::future<Tile>> requests;
    for (Rect roi : {Rect{0, 0, 4, 3}, Rect{2, 1, 4, 3}, Rect{4, 2, 4, 3}})
        requests.push_back(concurrent_scheduler.submit(
            cached.output_handle(), cached.source_bounds(), roi,
            RenderPriority::Normal, 2));
    std::size_t index = 0;
    for (Rect roi : {Rect{0, 0, 4, 3}, Rect{2, 1, 4, 3}, Rect{4, 2, 4, 3}})
        same_tile(requests[index++].get(), renderer.render_image(graph, roi, 2),
                  "concurrent cached ROI differs from serial render");
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
    rec_source.content_sha256 = fingerprint_raster_source(rec_image);
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

void test_raster_mip_preview() {
    constexpr std::uint32_t width = 7, height = 5, stride = 9;
    std::vector<float> pixels(stride * height * 3, 12345.0f);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto index = (static_cast<std::size_t>(y) * stride + x) * 3;
            pixels[index] = static_cast<float>(x + 10 * y);
            pixels[index + 1] = -static_cast<float>(2 * x + 5 * y);
            pixels[index + 2] = static_cast<float>(1.25 * x + 20 * y);
        }
    }
    RasterImage image({width, height, stride, WorkingSpace::LinearProPhotoD50},
                      std::move(pixels));
    auto node = std::make_shared<RasterSourceNode>(image);
    const Rect source_bounds{0, 0, width, height};
    const Rect preview_bounds{0, 0, 4, 3};
    const RenderLevel preview_level{1, RenderQuality::Preview};
    Renderer renderer;
    const auto full = renderer.render_image(
        *node, source_bounds, RenderRequest{preview_bounds, 8, preview_level});
    require(full.descriptor == ImageDescriptor::scene_linear(
                WorkingSpace::LinearProPhotoD50) &&
            std::abs(full.rgb[0] - 5.5f) < 1e-6f &&
            std::abs(full.rgb[1] + 3.5f) < 1e-6f &&
            std::abs(full.rgb[3 * 3] - 11.0f) < 1e-6f &&
            std::abs(full.rgb[(2 * 4 + 0) * 3] - 40.5f) < 1e-6f &&
            std::abs(full.rgb[(2 * 4 + 3) * 3] - 46.0f) < 1e-6f &&
            std::abs(full.rgb[(2 * 4 + 3) * 3 + 2] - 87.5f) < 1e-6f,
            "reduced raster preview changed box averages or clipped scene-linear values");
    for (std::uint32_t tile_size : {1u, 2u, 3u}) {
        const auto tiled = renderer.render_image(
            *node, source_bounds,
            RenderRequest{preview_bounds, tile_size, preview_level});
        same_tile(tiled, full, "reduced raster full/tiled output differs");
        require(tiled.rgb == full.rgb,
                "reduced raster pixel values depend on tile boundaries");
    }
    const Rect crop{1, 1, 2, 2};
    const auto cropped = renderer.render_image(
        *node, source_bounds, RenderRequest{crop, 1, preview_level});
    for (std::uint32_t y = 0; y < crop.height; ++y)
        for (std::uint32_t x = 0; x < crop.width; ++x)
            for (std::uint32_t channel = 0; channel < 3; ++channel)
                require(cropped.rgb[(y * crop.width + x) * 3 + channel] ==
                        full.rgb[((crop.y + y) * preview_bounds.width + crop.x + x) * 3 +
                                 channel],
                        "cropped reduced preview differs from full output");
    const RenderLevel second_level{2, RenderQuality::Preview};
    const Rect second_bounds{0, 0, 2, 2};
    const auto second_full = renderer.render_image(
        *node, source_bounds, RenderRequest{second_bounds, 8, second_level});
    require(std::abs(second_full.rgb[0] - 16.5f) < 1e-6f &&
            std::abs(second_full.rgb[3] - 20.0f) < 1e-6f &&
            std::abs(second_full.rgb[6] - 41.5f) < 1e-6f &&
            std::abs(second_full.rgb[9] - 45.0f) < 1e-6f,
            "mip 2 did not average direct clipped 4x4 source footprints");
    for (std::uint32_t tile_size : {1u, 2u}) {
        const auto tiled = renderer.render_image(
            *node, source_bounds,
            RenderRequest{second_bounds, tile_size, second_level});
        same_tile(tiled, second_full, "mip 2 tiled output differs");
        require(tiled.rgb == second_full.rgb, "mip 2 has a tile seam");
    }

    EditSource source;
    source.id = uuid(40);
    source.kind = EditSourceKind::SceneLinearRasterF32;
    source.working_space = WorkingSpace::LinearProPhotoD50;
    source.content_sha256 = fingerprint_raster_source(image);
    EditManifest manifest;
    manifest.sources.push_back(source);
    manifest.output_id = source.id;
    auto cache = std::make_shared<TileCache>(16 * 1024);
    ExecutableEditGraph graph(manifest,
        {{source, node, source_bounds}}, nullptr, cache);
    const RenderRequest request{preview_bounds, 2, preview_level};
    const auto full_source_region = graph.required_source_region(preview_bounds,
                                                                  preview_level);
    require(full_source_region.x == 0 && full_source_region.y == 0 &&
            full_source_region.width == width && full_source_region.height == height,
            "reduced full preview did not plan odd source edges");
    same_tile(renderer.render_image(graph, request), full,
              "cached source-only graph changed reduced preview pixels");
    const auto cold = cache->stats();
    same_tile(renderer.render_image(graph, request), full,
              "warm reduced preview changed pixels");
    require(cache->stats().hits > cold.hits && cache->stats().misses == cold.misses,
            "reduced preview did not reuse its own cached tiles");
    const auto before_second = cache->stats();
    const RenderRequest second_request{second_bounds, 1, second_level};
    same_tile(renderer.render_image(graph, second_request), second_full,
              "cached mip 2 source changed pixels");
    require(cache->stats().misses > before_second.misses,
            "mip 2 source reused mip 1 cache entries");
    const auto second_crop_region = graph.required_source_region({1, 0, 1, 2},
                                                                  second_level);
    require(second_crop_region.x == 4 && second_crop_region.y == 0 &&
            second_crop_region.width == 3 && second_crop_region.height == 5,
            "mip 2 ROI did not map to clipped 4x4 footprints");
    const auto native = renderer.render_image(
        graph, RenderRequest{preview_bounds, 2, {}});
    require(native.rgb[0] == 0.0f && full.rgb[0] == 5.5f &&
            cache->stats().entries > cold.entries,
            "native final reused reduced preview cache entries");
    TileScheduler scheduler(1, 2);
    same_tile(scheduler.submit(graph.output_handle(), graph.source_bounds(), request).get(),
              full, "scheduled reduced preview differs from direct render");
    same_tile(scheduler.submit(graph.output_handle(), graph.source_bounds(),
                               second_request).get(), second_full,
              "scheduled mip 2 differs from direct render");
    auto unsupported = request;
    unsupported.level.mip = 3;
    rejects([&] { renderer.render_image(graph, unsupported); },
            "unsupported third mip rendered");
    unsupported = request;
    unsupported.level.quality = RenderQuality::Final;
    rejects([&] { scheduler.submit(graph.output_handle(), graph.source_bounds(), unsupported); },
            "scheduler queued unsupported reduced final quality");
    unsupported = RenderRequest{{4, 0, 1, 1}, 2, preview_level};
    try {
        renderer.render_image(graph, unsupported);
        throw std::runtime_error("reduced viewport outside scaled bounds rendered");
    } catch (const std::out_of_range&) {}
    rejects([&] { scheduler.submit(graph.output_handle(), graph.source_bounds(), unsupported); },
            "scheduler queued a viewport outside reduced bounds");
    EditManifest edited = manifest;
    edited.operations.push_back(operation(41, "rawengine.exposure",
        EditDomain::SceneLinearProPhotoD50, EditDomain::SceneLinearProPhotoD50,
        source.id, {{"stops", EditValue{1.0}}}));
    edited.operations.push_back(operation(42, "rawengine.working_space_convert",
        EditDomain::SceneLinearProPhotoD50, EditDomain::SceneLinearRec2020D65,
        uuid(41)));
    edited.output_id = uuid(42);
    ExecutableEditGraph edited_graph(edited,
        {{source, node, source_bounds}}, nullptr, cache);
    const auto planned_crop = edited_graph.required_source_region(crop, preview_level);
    require(planned_crop.x == 2 && planned_crop.y == 2 &&
            planned_crop.width == 4 && planned_crop.height == 3,
            "edited reduced ROI did not map to its source footprint");
    auto exposure = std::make_shared<ExposureNode>(node, 1.0f);
    WorkingSpaceConvertNode converted(exposure, WorkingSpace::LinearRec2020D65);
    const auto expected_edited = renderer.render_image(
        converted, source_bounds, RenderRequest{preview_bounds, 8, preview_level});
    const auto before_edit = cache->stats();
    for (std::uint32_t tile_size : {1u, 2u, 3u, 8u}) {
        const auto actual = renderer.render_image(
            edited_graph, RenderRequest{preview_bounds, tile_size, preview_level});
        same_tile(actual, expected_edited,
                  "reduced exposure/conversion graph differs from direct nodes");
        require(actual.rgb == expected_edited.rgb,
                "reduced exposure/conversion changes with tile size");
    }
    require(cache->stats().hits > before_edit.hits,
            "edited preview did not reuse upstream reduced tiles");
    const auto second_edited = renderer.render_image(
        edited_graph, second_request);
    const auto second_expected = renderer.render_image(
        converted, source_bounds, RenderRequest{second_bounds, 8, second_level});
    same_tile(second_edited, second_expected,
              "mip 2 point-edit chain differs from direct nodes");
    const auto edited_crop = renderer.render_image(
        edited_graph, RenderRequest{crop, 1, preview_level});
    for (std::uint32_t y = 0; y < crop.height; ++y)
        for (std::uint32_t x = 0; x < crop.width; ++x)
            for (std::uint32_t channel = 0; channel < 3; ++channel)
                require(edited_crop.rgb[(y * crop.width + x) * 3 + channel] ==
                        expected_edited.rgb[((crop.y + y) * preview_bounds.width +
                                             crop.x + x) * 3 + channel],
                        "cropped edited preview differs from full output");
    const auto native_edited = renderer.render_image(
        edited_graph, RenderRequest{preview_bounds, 2, {}});
    same_tile(native_edited, converted.render(preview_bounds),
              "reduced stage cache changed native final output");
    auto revised_edit = edited;
    revised_edit.operations[0].parameters["stops"] = EditValue{2.0};
    ExecutableEditGraph revised_graph(revised_edit,
        {{source, node, source_bounds}}, nullptr, cache);
    auto revised_exposure = std::make_shared<ExposureNode>(node, 2.0f);
    WorkingSpaceConvertNode revised_convert(revised_exposure,
        WorkingSpace::LinearRec2020D65);
    const auto revised_expected = renderer.render_image(
        revised_convert, source_bounds, RenderRequest{preview_bounds, 8, preview_level});
    const auto before_revision = cache->stats();
    same_tile(renderer.render_image(revised_graph, request), revised_expected,
              "late exposure edit reused stale reduced output");
    require(cache->stats().hits > before_revision.hits,
            "late exposure edit failed to reuse reduced source tiles");

    EditManifest blur_edit = manifest;
    blur_edit.operations.push_back(operation(41, "rawengine.box_blur",
        EditDomain::SceneLinearProPhotoD50, EditDomain::SceneLinearProPhotoD50,
        source.id, {{"radius", EditValue{std::int64_t{1}}}}));
    blur_edit.output_id = uuid(41);
    ExecutableEditGraph blur_graph(blur_edit,
        {{source, node, source_bounds}}, nullptr, cache);
    BoxBlurNode blur(node, source_bounds, 1);
    const auto blurred = renderer.render_image(
        blur, source_bounds, RenderRequest{preview_bounds, 8, preview_level});
    require(std::abs(blurred.rgb[0] - 16.5f) < 1e-6f &&
            std::abs(blurred.rgb[(2 * 4 + 3) * 3] - 37.75f) < 1e-6f,
            "reduced blur did not average neighboring reduced pixels");
    for (std::uint32_t tile_size : {1u, 2u, 3u, 8u}) {
        const auto tiled = renderer.render_image(
            blur_graph, RenderRequest{preview_bounds, tile_size, preview_level});
        same_tile(tiled, blurred, "reduced blur full/tiled output differs");
        require(tiled.rgb == blurred.rgb, "reduced blur has a tile seam");
    }
    const Rect blurred_crop{0, 0, 1, 1};
    const auto planned_blur = blur_graph.required_source_region(blurred_crop,
                                                                preview_level);
    require(planned_blur.x == 0 && planned_blur.y == 0 &&
            planned_blur.width == 4 && planned_blur.height == 4,
            "reduced blur halo did not map to full source pixels");
    const auto blurred_roi = renderer.render_image(
        blur_graph, RenderRequest{blurred_crop, 1, preview_level});
    require(blurred_roi.rgb[0] == blurred.rgb[0],
            "reduced blur crop differs from full preview");
    auto wider_blur = blur_edit;
    wider_blur.operations[0].parameters["radius"] = EditValue{std::int64_t{2}};
    ExecutableEditGraph wider_graph(wider_blur,
        {{source, node, source_bounds}}, nullptr, cache);
    BoxBlurNode direct_wider(node, source_bounds, 2);
    const auto wider_expected = renderer.render_image(
        direct_wider, source_bounds, RenderRequest{preview_bounds, 8, preview_level});
    same_tile(renderer.render_image(wider_graph, request), wider_expected,
              "changed reduced blur radius reused stale output");
    require(wider_expected.rgb != blurred.rgb,
            "reduced blur radius change did not affect fixture");
    same_tile(renderer.render_image(blur_graph, second_request),
              renderer.render_image(blur, source_bounds,
                  RenderRequest{second_bounds, 8, second_level}),
              "mip 2 blur differs from direct reduced operation");
    auto blurred_chain = blur_edit;
    blurred_chain.operations.push_back(operation(42, "rawengine.exposure",
        EditDomain::SceneLinearProPhotoD50, EditDomain::SceneLinearProPhotoD50,
        uuid(41), {{"stops", EditValue{1.0}}}));
    blurred_chain.operations.push_back(operation(43, "rawengine.working_space_convert",
        EditDomain::SceneLinearProPhotoD50, EditDomain::SceneLinearRec2020D65,
        uuid(42)));
    blurred_chain.output_id = uuid(43);
    ExecutableEditGraph chain_graph(blurred_chain,
        {{source, node, source_bounds}}, nullptr, cache);
    auto chain_blur = std::make_shared<BoxBlurNode>(node, source_bounds, 1);
    auto chain_exposure = std::make_shared<ExposureNode>(chain_blur, 1.0f);
    WorkingSpaceConvertNode chain_convert(chain_exposure,
        WorkingSpace::LinearRec2020D65);
    const auto chain_expected = renderer.render_image(
        chain_convert, source_bounds, RenderRequest{preview_bounds, 8, preview_level});
    same_tile(renderer.render_image(chain_graph, request), chain_expected,
              "reduced blur and point-edit chain differs from direct nodes");
    const auto chain_region = chain_graph.required_source_region(blurred_crop,
                                                                 preview_level);
    require(chain_region.x == planned_blur.x && chain_region.y == planned_blur.y &&
            chain_region.width == planned_blur.width &&
            chain_region.height == planned_blur.height,
            "reduced blur halo was lost through downstream point edits");
}

void test_canonical_fingerprints() {
    RawMetadata a;
    a.width = 3; a.height = 2; a.row_stride_samples = 4;
    a.active_area = {1, 0, 2, 2};
    RawImage padded(a, {900, 1, 2, 800, 901, 3, 4, 801});
    a.row_stride_samples = 3;
    RawImage packed(a, {700, 1, 2, 701, 3, 4});
    require(fingerprint_raw_source(padded) == fingerprint_raw_source(packed),
            "RAW padding or samples outside active area changed fingerprint");
    constexpr std::array<std::uint8_t, 32> python_sha256{
        0xf3, 0x7c, 0xe7, 0xb0, 0x42, 0x5b, 0xf7, 0xb1,
        0x4e, 0x6d, 0x32, 0x3b, 0x6e, 0xb7, 0xd6, 0x88,
        0x3f, 0xde, 0x75, 0x2b, 0x8c, 0xfa, 0x53, 0x8e,
        0x8f, 0x70, 0xe0, 0xb7, 0xb0, 0xf8, 0x11, 0xa7};
    require(fingerprint_raw_source(packed) == python_sha256,
            "canonical RAW fingerprint disagrees with independent SHA-256 oracle");
    a.black_levels[0] = 1;
    RawImage changed_metadata(a, {700, 1, 2, 701, 3, 4});
    require(fingerprint_raw_source(packed) != fingerprint_raw_source(changed_metadata),
            "RAW rendering metadata did not change fingerprint");
    RasterImage float_padded({2, 2, 3, WorkingSpace::LinearProPhotoD50},
                             {1, 2, 3, 4, 5, 6, 99, 99, 99,
                              7, 8, 9, 10, 11, 12, 99, 99, 99});
    RasterImage float_packed({2, 2, 0, WorkingSpace::LinearProPhotoD50},
                             {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
    require(fingerprint_raster_source(float_padded) == fingerprint_raster_source(float_packed),
            "raster padding changed fingerprint");
    RasterImage changed_pixel({2, 2, 0, WorkingSpace::LinearProPhotoD50},
                              {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13});
    require(fingerprint_raster_source(float_packed) != fingerprint_raster_source(changed_pixel),
            "raster pixel change did not change fingerprint");
}

void test_box_blur_halo() {
    constexpr std::uint32_t width = 7, height = 5;
    std::vector<float> pixels(width * height * 3);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            for (int channel = 0; channel < 3; ++channel)
                pixels[(y * width + x) * 3 + channel] =
                    static_cast<float>(static_cast<int>((x * 17 + y * 23 + channel * 7) % 41)
                                       - 9) / 19.0f;
    RasterImage image({width, height, 0, WorkingSpace::LinearProPhotoD50}, pixels);
    auto node = std::make_shared<RasterSourceNode>(image);
    const Rect bounds{0, 0, width, height};
    BoxBlurNode blur(node, bounds, 1);
    const auto halo = blur.input_region({2, 1, 3, 3}, bounds);
    require(halo.x == 1 && halo.y == 0 && halo.width == 5 && halo.height == 5,
            "box blur did not request its upstream halo");
    const auto edge_halo = blur.input_region({0, 0, 2, 2}, bounds);
    require(edge_halo.x == 0 && edge_halo.y == 0 &&
            edge_halo.width == 3 && edge_halo.height == 3,
            "box blur halo crossed source bounds");
    Renderer renderer;
    const auto full = blur.render(bounds);
    for (auto tile_size : {1u, 2u, 3u, 8u})
        same_tile(renderer.render_image(blur, bounds, bounds, tile_size), full,
                  "box blur has a tiled seam");
    const Rect crop{2, 1, 3, 3};
    const auto cropped = blur.render(crop);
    for (std::uint32_t y = 0; y < crop.height; ++y)
        for (std::uint32_t x = 0; x < crop.width; ++x)
            for (int channel = 0; channel < 3; ++channel)
                require(cropped.rgb[(y * crop.width + x) * 3 + channel] ==
                            full.rgb[((y + crop.y) * width + x + crop.x) * 3 + channel],
                        "box blur ROI differs from full render");
    EditSource source;
    source.id = uuid(40);
    source.kind = EditSourceKind::SceneLinearRasterF32;
    source.working_space = WorkingSpace::LinearProPhotoD50;
    source.content_sha256 = fingerprint_raster_source(image);
    EditManifest manifest;
    manifest.sources.push_back(source);
    manifest.operations.push_back(operation(41, "rawengine.box_blur",
        EditDomain::SceneLinearProPhotoD50, EditDomain::SceneLinearProPhotoD50,
        source.id, {{"radius", EditValue{std::int64_t{1}}}}));
    manifest.output_id = uuid(41);
    auto cache = std::make_shared<TileCache>(32 * 1024);
    auto graph = ExecutableEditGraph(parse_edit_manifest(serialize_edit_manifest(manifest)),
        {{source, node, bounds}}, nullptr, cache);
    same_tile(renderer.render_image(graph, bounds, 2), full,
              "manifest box blur differs from direct full render");
    same_tile(renderer.render_image(graph, bounds, 2), full,
              "cached box blur differs from direct full render");
    require(graph.output().input_region(crop, bounds).width == halo.width,
            "cached blur lost its halo contract");
    auto wider = manifest;
    wider.operations[0].parameters["radius"] = EditValue{std::int64_t{2}};
    auto wider_graph = ExecutableEditGraph(wider, {{source, node, bounds}}, nullptr, cache);
    const auto wider_expected = BoxBlurNode(node, bounds, 2).render(bounds);
    same_tile(renderer.render_image(wider_graph, bounds, 2), wider_expected,
              "changed blur radius reused stale cached tiles");
    require(wider_expected.rgb != full.rgb,
            "blur radius change did not affect the fixture");
    auto chain = manifest;
    chain.operations.push_back(operation(42, "rawengine.box_blur",
        EditDomain::SceneLinearProPhotoD50, EditDomain::SceneLinearProPhotoD50,
        uuid(41), {{"radius", EditValue{std::int64_t{2}}}}));
    chain.operations.push_back(operation(43, "rawengine.exposure",
        EditDomain::SceneLinearProPhotoD50, EditDomain::SceneLinearProPhotoD50,
        uuid(42), {{"stops", EditValue{0.5}}}));
    chain.output_id = uuid(43);
    auto chained = ExecutableEditGraph(chain, {{source, node, bounds}}, nullptr, cache);
    const Rect chained_roi{2, 1, 2, 2};
    const auto planned = chained.required_source_region(chained_roi);
    require(planned.x == 0 && planned.y == 0 &&
            planned.width == 7 && planned.height == 5,
            "chained blur radii did not compose their source halos");
    const auto chained_full = chained.output().render(bounds);
    for (auto tile_size : {1u, 2u, 3u})
        same_tile(renderer.render_image(chained, bounds, tile_size), chained_full,
                  "chained blur and point operation have a tiled seam");
    const auto chained_crop = chained.output().render(chained_roi);
    for (std::uint32_t y = 0; y < chained_roi.height; ++y)
        for (std::uint32_t x = 0; x < chained_roi.width; ++x)
            for (int channel = 0; channel < 3; ++channel)
                require(chained_crop.rgb[(y * chained_roi.width + x) * 3 + channel] ==
                            chained_full.rgb[((y + chained_roi.y) * width + x +
                                              chained_roi.x) * 3 + channel],
                        "chained blur cropped output differs from full render");
    auto invalid = manifest;
    invalid.operations[0].parameters["radius"] = EditValue{std::int64_t{0}};
    rejects([&] { validate_edit_manifest(invalid); },
            "zero box-blur radius was accepted");
    invalid.operations[0].parameters["radius"] = EditValue{1.5};
    rejects([&] { validate_edit_manifest(invalid); },
            "fractional box-blur radius was accepted");
}

struct SchedulerProbeState {
    std::promise<void> entered, release;
    std::shared_future<void> release_future = release.get_future().share();
    std::mutex mutex;
    std::vector<std::uint32_t> order;
};

class SchedulerProbeNode final : public Node {
public:
    explicit SchedulerProbeNode(std::shared_ptr<SchedulerProbeState> state)
        : state_(std::move(state)) {}
    Tile render(Rect bounds) const override {
        if (bounds.x == 0) {
            state_->entered.set_value();
            state_->release_future.wait();
        }
        {
            std::lock_guard lock(state_->mutex);
            state_->order.push_back(bounds.x);
        }
        return Tile{bounds, std::vector<float>(3, static_cast<float>(bounds.x)),
                    ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50)};
    }
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50);
    }
private:
    std::shared_ptr<SchedulerProbeState> state_;
};

void test_scheduler() {
    auto state = std::make_shared<SchedulerProbeState>();
    auto entered = state->entered.get_future();
    auto node = std::make_shared<SchedulerProbeNode>(state);
    TileScheduler scheduler(1, 2);
    const Rect source_bounds{0, 0, 4, 1};
    auto first = scheduler.submit(node, source_bounds, {0, 0, 1, 1});
    entered.wait();
    auto background = scheduler.submit(node, source_bounds, {1, 0, 1, 1},
                                       RenderPriority::Background);
    auto interactive = scheduler.submit(node, source_bounds, {2, 0, 1, 1},
                                        RenderPriority::Interactive);
    try {
        scheduler.submit(node, source_bounds, {3, 0, 1, 1});
        throw std::runtime_error("scheduler accepted a request beyond its queue budget");
    } catch (const std::length_error&) {}
    state->release.set_value();
    require(first.get().rgb[0] == 0 && interactive.get().rgb[0] == 2 &&
            background.get().rgb[0] == 1,
            "scheduled render returned incorrect tiles");
    require(state->order == std::vector<std::uint32_t>({0, 2, 1}),
            "scheduler did not prioritize queued interactive work");
    const RenderRequest typed_request{{3, 0, 1, 1}, 1, {}};
    require(scheduler.submit(node, source_bounds, typed_request).get().rgb[0] == 3,
            "scheduler typed request differed from native render");
    require(scheduler.submit_latest("typed-viewport", node, source_bounds,
                                    typed_request).get().rgb[0] == 3,
            "scheduler typed latest request differed from native render");
    auto unsupported_request = typed_request;
    unsupported_request.level.mip = 1;
    rejects([&] { scheduler.submit(node, source_bounds, unsupported_request); },
            "scheduler queued an unsupported reduced mip");
    auto cancelled = std::make_shared<CancellationToken>();
    cancelled->cancel();
    auto aborted = scheduler.submit(node, source_bounds, {3, 0, 1, 1},
                                    RenderPriority::Normal, 256, cancelled);
    try {
        aborted.get();
        throw std::runtime_error("scheduled cancelled render completed");
    } catch (const RenderCancelled&) {}
    auto clear_state = std::make_shared<SchedulerProbeState>();
    auto clear_entered = clear_state->entered.get_future();
    auto clear_node = std::make_shared<SchedulerProbeNode>(clear_state);
    TileCache clear_cache(1024);
    std::array<std::uint8_t, 32> signature{};
    signature[0] = 1;
    auto in_flight = std::async(std::launch::async, [&] {
        return clear_cache.render(*clear_node, signature, {0, 0, 1, 1});
    });
    clear_entered.wait();
    clear_cache.clear();
    clear_state->release.set_value();
    require(in_flight.get().rgb.size() == 3 && clear_cache.stats().entries == 0,
            "clear allowed an in-flight tile to refill the cache");

    auto queued_state = std::make_shared<SchedulerProbeState>();
    auto queued_entered = queued_state->entered.get_future();
    auto queued_node = std::make_shared<SchedulerProbeNode>(queued_state);
    TileScheduler latest_queued(1, 1);
    auto blocking = latest_queued.submit(queued_node, source_bounds, {0, 0, 1, 1});
    queued_entered.wait();
    auto obsolete = latest_queued.submit_latest("viewport", queued_node,
        source_bounds, {1, 0, 1, 1});
    auto newest = latest_queued.submit_latest("viewport", queued_node,
        source_bounds, {2, 0, 1, 1});
    try {
        obsolete.get();
        throw std::runtime_error("superseded queued viewport completed");
    } catch (const RenderCancelled&) {}
    queued_state->release.set_value();
    require(blocking.get().rgb[0] == 0 && newest.get().rgb[0] == 2 &&
            queued_state->order == std::vector<std::uint32_t>({0, 2}),
            "new viewport did not replace queued obsolete work");

    auto running_state = std::make_shared<SchedulerProbeState>();
    auto running_entered = running_state->entered.get_future();
    auto running_node = std::make_shared<SchedulerProbeNode>(running_state);
    TileScheduler latest_running(1, 1);
    auto active = latest_running.submit_latest("viewport", running_node,
        source_bounds, {0, 0, 1, 1});
    running_entered.wait();
    auto replacement = latest_running.submit_latest("viewport", running_node,
        source_bounds, {1, 0, 1, 1});
    running_state->release.set_value();
    try {
        active.get();
        throw std::runtime_error("superseded running viewport completed");
    } catch (const RenderCancelled&) {}
    require(replacement.get().rgb[0] == 1,
            "replacement viewport did not complete after active cancellation");
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
        test_raster_mip_preview();
        test_canonical_fingerprints();
        test_box_blur_halo();
        test_scheduler();
        std::cout << "Edit graph format tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

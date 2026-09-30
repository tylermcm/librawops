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

void test_srgb_mip_preview() {
    constexpr std::uint32_t width = 7, height = 5, stride = 9;
    const Rect bounds{0, 0, width, height};
    Renderer renderer;
    for (auto space : {WorkingSpace::LinearProPhotoD50, WorkingSpace::LinearRec2020D65}) {
        std::vector<float> pixels(stride * height * 3, 12345.0f);
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto i = (y * stride + x) * 3;
                pixels[i] = (x + y) % 2 ? 2.0f : -0.1f;
                pixels[i + 1] = 0.01f * x + 0.03f * y;
                pixels[i + 2] = 0.1f + 0.2f * x;
            }
        RasterImage image({width, height, stride, space}, pixels);
        auto node = std::make_shared<RasterSourceNode>(image);
        GraphRecipe recipe;
        recipe.output_mode = OutputMode::SrgbPreview;
        recipe.exposure_stops = 0.5f;
        recipe.tone_shoulder = 0.8f;
        recipe.tone_gamma = 1.3f;
        ImageGraph fixed(image, recipe);
        EditSource source;
        source.id = uuid(50);
        source.kind = EditSourceKind::SceneLinearRasterF32;
        source.working_space = space;
        source.content_sha256 = fingerprint_raster_source(image);
        const auto domain = space == WorkingSpace::LinearProPhotoD50
            ? EditDomain::SceneLinearProPhotoD50 : EditDomain::SceneLinearRec2020D65;
        EditManifest manifest;
        manifest.working_space = space;
        manifest.sources.push_back(source);
        manifest.operations.push_back(operation(51, "rawengine.exposure", domain, domain,
            source.id, {{"stops", EditValue{0.5}}}));
        manifest.operations.push_back(operation(52, "rawengine.working_to_srgb",
            domain, EditDomain::SceneLinearSrgb, uuid(51)));
        manifest.operations.push_back(operation(53, "rawengine.tone_curve",
            EditDomain::SceneLinearSrgb, EditDomain::DisplayLinearSrgb, uuid(52),
            {{"shoulder", EditValue{0.8}}, {"gamma", EditValue{1.3}}}));
        manifest.operations.push_back(operation(54, "rawengine.srgb_encode",
            EditDomain::DisplayLinearSrgb, EditDomain::DisplayEncodedSrgb, uuid(53)));
        manifest.output_id = uuid(54);
        auto cache = std::make_shared<TileCache>(1024 * 1024);
        ExecutableEditGraph graph(parse_edit_manifest(serialize_edit_manifest(manifest)),
            {{source, node, bounds}}, nullptr, cache);
        const auto native = renderer.render_image(fixed, bounds, 2);
        for (std::uint32_t mip : {1u, 2u}) {
            const std::uint32_t scale = 1u << mip;
            const Rect reduced{0, 0, (width + scale - 1) / scale,
                                     (height + scale - 1) / scale};
            const RenderLevel level{mip, RenderQuality::Preview};
            const RenderRequest request{reduced, 2, level};
            // Full-resolution rendering of a separately averaged linear source is
            // the reference: reduction precedes all nonlinear output operations.
            std::vector<float> averaged(reduced.width * reduced.height * 3);
            std::vector<float> encoded_average(averaged.size());
            for (std::uint32_t y = 0; y < reduced.height; ++y)
                for (std::uint32_t x = 0; x < reduced.width; ++x) {
                    double sum[3]{}, encoded_sum[3]{};
                    std::uint32_t count = 0;
                    for (std::uint32_t sy = y * scale; sy < std::min(height, (y + 1) * scale); ++sy)
                        for (std::uint32_t sx = x * scale; sx < std::min(width, (x + 1) * scale); ++sx) {
                            ++count;
                            for (std::uint32_t c = 0; c < 3; ++c) {
                                sum[c] += pixels[(sy * stride + sx) * 3 + c];
                                encoded_sum[c] += native.rgb[(sy * width + sx) * 3 + c];
                            }
                        }
                    for (std::uint32_t c = 0; c < 3; ++c) {
                        averaged[(y * reduced.width + x) * 3 + c] = static_cast<float>(sum[c] / count);
                        encoded_average[(y * reduced.width + x) * 3 + c] = static_cast<float>(encoded_sum[c] / count);
                    }
                }
            ImageGraph reference(RasterImage({reduced.width, reduced.height, 0, space}, averaged), recipe);
            const auto expected = renderer.render_image(reference, reduced, 8);
            const auto before_level = cache->stats();
            const auto actual = renderer.render_image(graph, request);
            require(cache->stats().misses > before_level.misses,
                    "sRGB mip reused another level's cache entries");
            same_tile(actual, expected, "sRGB preview differs from linear-reduced full-resolution reference");
            require(actual.descriptor == ImageDescriptor::srgb_output(), "preview lost sRGB descriptor");
            double difference = 0;
            for (std::size_t i = 0; i < actual.rgb.size(); ++i) {
                require(std::isfinite(actual.rgb[i]) && actual.rgb[i] >= 0 && actual.rgb[i] <= 1,
                        "sRGB preview output is unbounded");
                difference = std::max(difference, std::abs(static_cast<double>(actual.rgb[i] - encoded_average[i])));
            }
            require(difference > 0.01, "fixture failed to distinguish reduction before nonlinear encoding");
            std::cout << "sRGB mip " << mip << " space " << static_cast<int>(space)
                      << " max difference from averaged native encoded output: " << difference << '\n';
            const auto cold = cache->stats();
            same_tile(renderer.render_image(graph, request), actual, "warm sRGB preview changed");
            require(cache->stats().hits > cold.hits && cache->stats().misses == cold.misses,
                    "warm sRGB preview failed to reuse cache");
            for (std::uint32_t tile_size : {1u, 3u, 8u}) {
                const auto tiled = renderer.render_image(graph, RenderRequest{reduced, tile_size, level});
                require(tiled.rgb == actual.rgb, "sRGB preview has a tile seam");
                same_tile(renderer.render_image(fixed, RenderRequest{reduced, tile_size, level}),
                          expected, "fixed sRGB preview differs from manifest graph");
            }
            const Rect crop{1, 0, 1, reduced.height};
            const auto cropped = renderer.render_image(graph, RenderRequest{crop, 1, level});
            for (std::uint32_t y = 0; y < crop.height; ++y)
                for (std::uint32_t c = 0; c < 3; ++c)
                    require(cropped.rgb[y * 3 + c] == actual.rgb[(y * reduced.width + 1) * 3 + c],
                            "sRGB preview crop differs");
            const auto planned = graph.required_source_region(crop, level);
            require(planned.x == scale && planned.y == 0 &&
                    planned.width == std::min(scale, width - scale) && planned.height == height,
                    "sRGB preview source-region mapping differs");
            TileScheduler scheduler(1, 2);
            same_tile(scheduler.submit(graph.output_handle(), bounds, request).get(), expected,
                      "scheduled sRGB preview differs");
            // Native tiles sharing reduced coordinates must have distinct identities.
            same_tile(renderer.render_image(graph, RenderRequest{reduced, 2, {}}),
                      renderer.render_image(fixed, reduced, 2), "sRGB reduced cache leaked into native final");
            auto revised = manifest;
            revised.operations[2].parameters["shoulder"] = EditValue{1.6};
            ExecutableEditGraph changed(revised, {{source, node, bounds}}, nullptr, cache);
            auto changed_recipe = recipe;
            changed_recipe.tone_shoulder = 1.6f;
            ImageGraph changed_reference(RasterImage({reduced.width, reduced.height, 0, space}, averaged), changed_recipe);
            const auto before = cache->stats();
            const auto changed_pixels = renderer.render_image(changed, request);
            same_tile(changed_pixels, renderer.render_image(changed_reference, reduced, 8),
                      "tone revision reused stale sRGB preview");
            require(changed_pixels.rgb != actual.rgb && cache->stats().hits > before.hits,
                    "tone revision failed to change output and reuse upstream cache");
            rejects([&] { renderer.render_image(graph, RenderRequest{reduced, 2, {mip, RenderQuality::Final}}); },
                    "sRGB chain accepted reduced final quality");
            auto blurred_manifest = manifest;
            blurred_manifest.operations.insert(blurred_manifest.operations.begin(),
                operation(55, "rawengine.box_blur", domain, domain, source.id,
                    {{"radius", EditValue{std::int64_t{1}}}}));
            blurred_manifest.operations[1].inputs["image"] = uuid(55);
            ExecutableEditGraph blurred(blurred_manifest, {{source, node, bounds}}, nullptr, cache);
            auto direct_blur = std::make_shared<BoxBlurNode>(node, bounds, 1);
            auto direct_exposure = std::make_shared<ExposureNode>(direct_blur, recipe.exposure_stops);
            auto direct_conversion = std::make_shared<WorkingToSrgbNode>(direct_exposure);
            auto direct_tone = std::make_shared<ToneCurveNode>(direct_conversion, recipe.tone_shoulder, recipe.tone_gamma);
            SrgbEncodeNode direct_encoded(direct_tone);
            const auto expected_blur = renderer.render_image(direct_encoded, bounds, RenderRequest{reduced, 8, level});
            const auto tiled_blur = renderer.render_image(blurred, RenderRequest{reduced, 1, level});
            same_tile(tiled_blur, expected_blur, "blur-to-sRGB preview differs across tiles");
            const auto halo = blurred.required_source_region({0, 0, 1, 1}, level);
            require(halo.x == 0 && halo.y == 0 && halo.width == std::min(width, 2 * scale) &&
                    halo.height == std::min(height, 2 * scale), "sRGB chain lost upstream reduced blur halo");
        }
        rejects([&] { renderer.render_image(graph, RenderRequest{{0, 0, 1, 1}, 1, {3, RenderQuality::Preview}}); },
                "sRGB chain accepted unsupported mip");
        auto linear_srgb = std::make_shared<WorkingToSrgbNode>(node);
        auto tone = std::make_shared<ToneCurveNode>(linear_srgb, 1.0f, 1.0f);
        SrgbEncodeNode encoded(tone);
        auto working_tone = std::make_shared<ToneCurveNode>(node, 1.0f, 1.0f);
        OutputClipNode legacy(working_tone);
        for (const Node* stage : {static_cast<const Node*>(linear_srgb.get()),
                                 static_cast<const Node*>(tone.get()), static_cast<const Node*>(&encoded)}) {
            rejects([&] { stage->render_level({0, 0, 1, 1}, {1, RenderQuality::Final}); },
                    "output stage accepted reduced final");
            rejects([&] { stage->input_region_level({0, 0, 1, 1}, bounds, {3, RenderQuality::Preview}); },
                    "output stage planned unsupported mip");
        }
        require(!legacy.supports_level({1, RenderQuality::Preview}) &&
                !working_tone->supports_level({1, RenderQuality::Preview}),
                "unmanaged output gained reduced support");
    }
}

void test_orientation_geometry() {
    Renderer renderer;
    auto rect_equal = [](Rect a, Rect b) { return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height; };
    for (auto space : {WorkingSpace::LinearProPhotoD50, WorkingSpace::LinearRec2020D65}) {
        std::vector<float> pixels(7 * 5 * 3);
        for (unsigned y = 0; y < 5; ++y)
            for (unsigned x = 0; x < 7; ++x) {
                const auto i = (y * 7 + x) * 3;
                pixels[i] = static_cast<float>(10 * y + x);
                pixels[i + 1] = -static_cast<float>(x) / 4;
                pixels[i + 2] = 1.25f + static_cast<float>(y) / 2;
            }
        RasterImage image({7, 5, 0, space}, pixels);
        auto node = std::make_shared<RasterSourceNode>(image);
        EditSource source; source.id = uuid(100); source.kind = EditSourceKind::SceneLinearRasterF32;
        source.working_space = space; source.content_sha256 = fingerprint_raster_source(image);
        const Rect original{0, 0, 7, 5};
        const std::vector<BoundEditSource> bindings{{source, node, original}};
        const auto domain = space == WorkingSpace::LinearProPhotoD50 ? EditDomain::SceneLinearProPhotoD50 : EditDomain::SceneLinearRec2020D65;
        for (unsigned turns = 0; turns < 4; ++turns)
            for (bool horizontal : {false, true})
                for (bool vertical : {false, true}) {
                    const Rect extent = turns % 2 ? Rect{0, 0, 5, 7} : original;
                    auto orient = operation(101, "rawengine.orientation", domain, domain, source.id,
                        {{"quarter_turns", EditValue{static_cast<std::int64_t>(turns)}},
                         {"flip_horizontal", EditValue{horizontal}}, {"flip_vertical", EditValue{vertical}}});
                    EditManifest manifest; manifest.working_space = space; manifest.sources = {source};
                    manifest.operations = {orient}; manifest.output_id = orient.id;
                    auto cache = std::make_shared<TileCache>(1024 * 1024);
                    ExecutableEditGraph graph(parse_edit_manifest(serialize_edit_manifest(manifest)), bindings, nullptr, cache);
                    require(rect_equal(graph.output_bounds(), extent) && rect_equal(graph.source_bounds(), original), "orientation extents are wrong");
                    // Forward scatter independently of the implementation's inverse map.
                    auto destination = [&](unsigned x, unsigned y) {
                        unsigned dx = x, dy = y;
                        if (turns == 1) { dx = 4 - y; dy = x; }
                        if (turns == 2) { dx = 6 - x; dy = 4 - y; }
                        if (turns == 3) { dx = y; dy = 6 - x; }
                        if (horizontal) dx = extent.width - 1 - dx;
                        if (vertical) dy = extent.height - 1 - dy;
                        return std::pair{dx, dy};
                    };
                    Tile expected{extent, std::vector<float>(pixels.size()), node->output_descriptor()};
                    for (unsigned y = 0; y < 5; ++y)
                        for (unsigned x = 0; x < 7; ++x) {
                            const auto [dx, dy] = destination(x, y);
                            for (unsigned c = 0; c < 3; ++c) expected.rgb[(dy * extent.width + dx) * 3 + c] = pixels[(y * 7 + x) * 3 + c];
                        }
                    const auto full = renderer.render_image(graph, extent, 16);
                    require(full.rgb == expected.rgb, "orientation differs from forward scatter");
                    for (std::size_t i = 0; i < full.rgb.size(); ++i)
                        require(std::signbit(full.rgb[i]) == std::signbit(expected.rgb[i]), "orientation lost signed zero");
                    for (unsigned tile : {1u, 2u, 3u}) require(renderer.render_image(graph, extent, tile).rgb == expected.rgb, "orientation tile seam");
                    const auto before = cache->stats(); renderer.render_image(graph, extent, 16);
                    require(cache->stats().hits == before.hits + 1 && cache->stats().misses == before.misses, "warm orientation failed");
                    TileScheduler scheduler(1, 2);
                    same_tile(scheduler.submit(graph.output_handle(), extent, extent).get(), expected, "scheduled orientation differs");
                    for (unsigned mip : {0u, 1u, 2u}) {
                        const auto scale = 1u << mip;
                        const RenderLevel level{mip, mip ? RenderQuality::Preview : RenderQuality::Final};
                        const Rect reduced{0, 0, (extent.width + scale - 1) / scale, (extent.height + scale - 1) / scale};
                        const auto rendered = renderer.render_image(graph, RenderRequest{reduced, 16, level});
                        for (unsigned y = 0; y < reduced.height; ++y)
                            for (unsigned x = 0; x < reduced.width; ++x)
                                for (unsigned c = 0; c < 3; ++c) {
                                    double sum = 0; unsigned count = 0;
                                    for (unsigned iy = y * scale; iy < std::min(extent.height, (y + 1) * scale); ++iy)
                                        for (unsigned ix = x * scale; ix < std::min(extent.width, (x + 1) * scale); ++ix) {
                                            sum += expected.rgb[(iy * extent.width + ix) * 3 + c]; ++count;
                                        }
                                    require(rendered.rgb[(y * reduced.width + x) * 3 + c] == static_cast<float>(sum / count), "orientation preview average is wrong");
                                }
                        for (unsigned tile : {1u, 2u}) require(renderer.render_image(graph, RenderRequest{reduced, tile, level}).rgb == rendered.rgb, "orientation preview seam");
                        const Rect roi{reduced.width - 1, reduced.height - 1, 1, 1};
                        unsigned minx = 7, miny = 5, maxx = 0, maxy = 0;
                        for (unsigned y = 0; y < 5; ++y)
                            for (unsigned x = 0; x < 7; ++x) {
                                const auto [dx, dy] = destination(x, y);
                                if (dx / scale == roi.x && dy / scale == roi.y) {
                                    minx = std::min(minx, x); miny = std::min(miny, y); maxx = std::max(maxx, x); maxy = std::max(maxy, y);
                                }
                            }
                        require(rect_equal(graph.required_source_region(roi, level), {minx, miny, maxx - minx + 1, maxy - miny + 1}), "orientation source footprint wrong");
                        const auto roi_result = renderer.render_image(graph, RenderRequest{roi, 1, level});
                        for (unsigned c = 0; c < 3; ++c) require(roi_result.rgb[c] == rendered.rgb[rendered.rgb.size() - 3 + c], "orientation ROI differs");
                        require(renderer.render_image(graph, RenderRequest{{reduced.width, reduced.height, 0, 0}, 1, level}).rgb.empty(), "empty orientation emitted pixels");
                        same_tile(scheduler.submit(graph.output_handle(), graph.output_bounds(), RenderRequest{reduced, 2, level}).get(), rendered, "scheduled orientation preview differs");
                    }
                    auto revised = manifest; revised.operations[0].parameters["flip_horizontal"] = EditValue{!horizontal};
                    const auto changed = renderer.render_image(ExecutableEditGraph(revised, bindings, nullptr, cache), extent, 16);
                    require(changed.rgb != full.rgb, "orientation revision reused stale output");
                    same_tile(changed, renderer.render_image(ExecutableEditGraph(revised, bindings), extent, 16), "orientation cache identity wrong");
                    auto disabled = manifest; disabled.operations[0].enabled = false;
                    ExecutableEditGraph bypass(disabled, bindings);
                    require(rect_equal(bypass.output_bounds(), original) && renderer.render_image(bypass, original).rgb == pixels, "disabled orientation changed extent/samples");
                }
        auto crop = operation(102, "rawengine.crop", domain, domain, source.id,
            {{"x", EditValue{std::int64_t{1}}}, {"y", EditValue{std::int64_t{1}}},
             {"width", EditValue{std::int64_t{5}}}, {"height", EditValue{std::int64_t{3}}}});
        auto orient = operation(103, "rawengine.orientation", domain, domain, crop.id,
            {{"quarter_turns", EditValue{std::int64_t{1}}}, {"flip_horizontal", EditValue{false}}, {"flip_vertical", EditValue{true}}});
        auto resize = operation(104, "rawengine.resize", domain, domain, orient.id,
            {{"width", EditValue{std::int64_t{5}}}, {"height", EditValue{std::int64_t{7}}}, {"filter", EditValue{std::string("area")}}});
        auto blur = operation(105, "rawengine.box_blur", domain, domain, resize.id, {{"radius", EditValue{std::int64_t{1}}}});
        EditManifest composed; composed.working_space = space; composed.sources = {source}; composed.operations = {blur, resize, orient, crop}; composed.output_id = blur.id;
        ExecutableEditGraph composition(composed, bindings);
        for (unsigned mip : {0u, 1u, 2u}) {
            const auto scale = 1u << mip; const RenderLevel level{mip, mip ? RenderQuality::Preview : RenderQuality::Final};
            const Rect extent{0, 0, (5 + scale - 1) / scale, (7 + scale - 1) / scale};
            same_tile(renderer.render_image(composition, RenderRequest{extent, 1, level}), renderer.render_image(composition, RenderRequest{extent, 16, level}), "crop/orientation/resize/blur seams");
            require(rect_equal(composition.required_source_region(extent, level), {1, 1, 5, 3}), "composed orientation source bounds wrong");
        }
        auto second = source; second.id = uuid(106);
        auto other_crop = crop; other_crop.id = uuid(107); other_crop.inputs["image"] = second.id;
        auto other_orientation = orient; other_orientation.id = uuid(108); other_orientation.inputs["image"] = other_crop.id;
        other_orientation.parameters["quarter_turns"] = EditValue{std::int64_t{3}};
        other_orientation.parameters["flip_vertical"] = EditValue{false};
        auto mix = operation(109, "rawengine.linear_mix", domain, domain, source.id, {{"amount", EditValue{0.25}}});
        mix.inputs = {{"base", orient.id}, {"layer", other_orientation.id}};
        auto branches = composed; branches.sources.push_back(second);
        branches.operations = {mix, other_orientation, other_crop, orient, crop}; branches.output_id = mix.id;
        auto branch_bindings = bindings; branch_bindings.push_back({second, node, original});
        ExecutableEditGraph branch_graph(branches, branch_bindings);
        const auto regions = branch_graph.required_source_regions({0, 0, 1, 1}, {1, RenderQuality::Preview});
        require(regions.size() == 2 && rect_equal(regions.at(source.id), {4, 2, 2, 2}) && rect_equal(regions.at(second.id), {4, 1, 2, 2}),
                "orientation confused separate-source preview footprints");
        same_tile(renderer.render_image(branch_graph, RenderRequest{{0, 0, 2, 3}, 1, {1, RenderQuality::Preview}}),
                  renderer.render_image(branch_graph, RenderRequest{{0, 0, 2, 3}, 16, {1, RenderQuality::Preview}}), "oriented mix preview seams");
        std::shared_ptr<const Node> round_trip = node; Rect round_bounds = original;
        for (unsigned i = 0; i < 4; ++i) {
            auto turn = std::make_shared<OrientationNode>(round_trip, round_bounds, 1);
            round_bounds = turn->output_bounds(); round_trip = std::move(turn);
        }
        require(round_trip->render(original).rgb == pixels, "four quarter turns failed native round trip");
        auto invalid = composed; invalid.operations[2].parameters["quarter_turns"] = EditValue{std::int64_t{4}};
        rejects([&] { ExecutableEditGraph bad(invalid, bindings); }, "invalid quarter turns accepted");
        invalid = composed; invalid.operations[2].parameters["flip_horizontal"] = EditValue{std::int64_t{1}};
        rejects([&] { ExecutableEditGraph bad(invalid, bindings); }, "integer flip accepted");
        invalid = composed; invalid.operations[2].parameters["quarter_turns"] = EditValue{1.0};
        rejects([&] { ExecutableEditGraph bad(invalid, bindings); }, "floating quarter turns accepted");
        rejects([&] { composition.required_source_regions({0, 0, 1, 1}, {0, RenderQuality::Preview}); }, "native orientation preview quality accepted");
        rejects([&] { OrientationNode bad(nullptr, original, 1); }, "null orientation input accepted");
        OrientationNode nonzero(node, {2, 1, 3, 2}, 1);
        require(rect_equal(nonzero.input_region({0, 0, 1, 1}, {2, 1, 3, 2}), {2, 2, 1, 1}), "nonzero-origin orientation mapping wrong");
        const auto maximum = std::numeric_limits<std::uint32_t>::max();
        OrientationNode extreme(node, {maximum, maximum, 1, 1}, 3, true, true);
        require(rect_equal(extreme.input_region({0, 0, 1, 1}, {maximum, maximum, 1, 1}), {maximum, maximum, 1, 1}), "orientation origin overflowed");
    }
    auto raw = std::make_shared<RawUnpackNode>(RawImage(2, 2, std::vector<std::uint16_t>(4, 32768)));
    CameraColorTransform calibration; calibration.camera_to_xyz_d50 = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    auto camera = std::make_shared<CameraToWorkingNode>(raw, calibration);
    OrientationNode raw_orientation(camera, {0, 0, 2, 2}, 1);
    require(!raw_orientation.supports_level({1, RenderQuality::Preview}), "orientation enabled an unbounded camera preview");
}

void test_resize_geometry() {
    Renderer renderer;
    auto same_rect = [](Rect a, Rect b) {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    };
    for (auto space : {WorkingSpace::LinearProPhotoD50, WorkingSpace::LinearRec2020D65}) {
        std::vector<float> pixels(7 * 5 * 3);
        for (unsigned y = 0; y < 5; ++y)
            for (unsigned x = 0; x < 7; ++x) {
                const auto i = (y * 7 + x) * 3;
                pixels[i] = -2.0f + x + 10 * y;
                pixels[i + 1] = (x + y) % 2 ? 2.0f : -1.0f;
                pixels[i + 2] = 0.125f;
            }
        RasterImage image({7, 5, 0, space}, pixels);
        auto source_node = std::make_shared<RasterSourceNode>(image);
        const Rect input_bounds{0, 0, 7, 5};
        EditSource source;
        source.id = uuid(90); source.kind = EditSourceKind::SceneLinearRasterF32;
        source.working_space = space; source.content_sha256 = fingerprint_raster_source(image);
        const auto domain = space == WorkingSpace::LinearProPhotoD50
            ? EditDomain::SceneLinearProPhotoD50 : EditDomain::SceneLinearRec2020D65;
        const std::vector<BoundEditSource> bindings{{source, source_node, input_bounds}};
        for (auto filter : {ResizeFilter::Nearest, ResizeFilter::Bilinear, ResizeFilter::Area})
            for (const auto dimensions : {std::pair{7u, 5u}, {9u, 8u}, {3u, 2u}, {1u, 1u}, {1u, 9u}, {9u, 1u}}) {
                const Rect extent{0, 0, dimensions.first, dimensions.second};
                auto resize = operation(91, "rawengine.resize", domain, domain, source.id,
                    {{"width", EditValue{static_cast<std::int64_t>(extent.width)}},
                     {"height", EditValue{static_cast<std::int64_t>(extent.height)}},
                     {"filter", EditValue{std::string(filter == ResizeFilter::Nearest ? "nearest" :
                        filter == ResizeFilter::Area ? "area" : "bilinear")}}});
                EditManifest manifest; manifest.working_space = space; manifest.sources = {source};
                manifest.operations = {resize}; manifest.output_id = resize.id;
                auto cache = std::make_shared<TileCache>(1024 * 1024);
                ExecutableEditGraph graph(parse_edit_manifest(serialize_edit_manifest(manifest)), bindings, nullptr, cache);
                require(same_rect(graph.output_bounds(), extent) && same_rect(graph.source_bounds(), input_bounds),
                        "resize confused source/output extent");
                const auto full = renderer.render_image(graph, extent, 16);
                // Independent oracle weights every source sample, without ROI/tile logic.
                for (unsigned y = 0; y < extent.height; ++y)
                    for (unsigned x = 0; x < extent.width; ++x) {
                        const double px = std::clamp((x + 0.5) * 7 / extent.width - 0.5, 0.0, 6.0);
                        const double py = std::clamp((y + 0.5) * 5 / extent.height - 0.5, 0.0, 4.0);
                        for (unsigned c = 0; c < 3; ++c) {
                            double expected = 0;
                            if (filter == ResizeFilter::Nearest) {
                                expected = pixels[(static_cast<unsigned>(std::floor(py + 0.5)) * 7 +
                                                   static_cast<unsigned>(std::floor(px + 0.5))) * 3 + c];
                            } else if (filter == ResizeFilter::Area) {
                                const double left = static_cast<double>(x) * 7 / extent.width;
                                const double right = static_cast<double>(x + 1) * 7 / extent.width;
                                const double top = static_cast<double>(y) * 5 / extent.height;
                                const double bottom = static_cast<double>(y + 1) * 5 / extent.height;
                                for (unsigned sy = 0; sy < 5; ++sy)
                                    for (unsigned sx = 0; sx < 7; ++sx)
                                        expected += std::max(0.0, std::min(right, sx + 1.0) - std::max(left, static_cast<double>(sx))) *
                                                    std::max(0.0, std::min(bottom, sy + 1.0) - std::max(top, static_cast<double>(sy))) *
                                                    pixels[(sy * 7 + sx) * 3 + c] / ((right - left) * (bottom - top));
                            } else for (unsigned sy = 0; sy < 5; ++sy)
                                for (unsigned sx = 0; sx < 7; ++sx)
                                    expected += std::max(0.0, 1 - std::abs(px - sx)) *
                                                std::max(0.0, 1 - std::abs(py - sy)) * pixels[(sy * 7 + sx) * 3 + c];
                            require(std::abs(full.rgb[(y * extent.width + x) * 3 + c] - static_cast<float>(expected)) < 1e-5f,
                                    "resized pixels differ from independent kernel oracle");
                        }
                    }
                if (extent.width == 7 && extent.height == 5) require(full.rgb == pixels, "identity resize changed samples");
                for (unsigned tile_size : {1u, 2u, 3u}) {
                    const auto tiled = renderer.render_image(graph, extent, tile_size);
                    require(tiled.rgb == full.rgb, "resize has tile seams");
                }
                const Rect roi{extent.width - 1, extent.height - 1, 1, 1};
                const auto tile = renderer.render_image(graph, roi, 1);
                for (unsigned c = 0; c < 3; ++c) require(tile.rgb[c] == full.rgb[full.rgb.size() - 3 + c], "resize ROI differs");
                const auto empty = renderer.render_image(graph, {extent.width, extent.height, 0, 0});
                require(empty.rgb.empty(), "empty resize emitted pixels");
                require(graph.required_source_region({extent.width, extent.height, 0, 0}).width == 0,
                        "empty resize planned source samples");
                const auto before = cache->stats(); renderer.render_image(graph, extent, 16);
                require(cache->stats().hits == before.hits + 1 && cache->stats().misses == before.misses, "warm resize cache failed");
                TileScheduler scheduler(1, 2);
                same_tile(scheduler.submit(graph.output_handle(), graph.output_bounds(), extent).get(), full, "scheduled resize differs");
                for (unsigned mip : {1u, 2u}) {
                    const auto scale = 1u << mip;
                    const RenderLevel level{mip, RenderQuality::Preview};
                    const Rect reduced_bounds{0, 0, (extent.width + scale - 1) / scale, (extent.height + scale - 1) / scale};
                    const auto first_level_stats = cache->stats();
                    const auto reduced = renderer.render_image(graph, RenderRequest{reduced_bounds, 16, level});
                    require(cache->stats().misses > first_level_stats.misses, "resize cache aliased native or another mip");
                    for (unsigned y = 0; y < reduced_bounds.height; ++y)
                        for (unsigned x = 0; x < reduced_bounds.width; ++x)
                            for (unsigned c = 0; c < 3; ++c) {
                                double sum = 0; unsigned count = 0;
                                for (unsigned iy = y * scale; iy < std::min(extent.height, (y + 1) * scale); ++iy)
                                    for (unsigned ix = x * scale; ix < std::min(extent.width, (x + 1) * scale); ++ix) {
                                        sum += full.rgb[(iy * extent.width + ix) * 3 + c]; ++count;
                                    }
                                require(reduced.rgb[(y * reduced_bounds.width + x) * 3 + c] == static_cast<float>(sum / count),
                                        "resize preview differs from independently averaged native linear output");
                            }
                    for (unsigned tile_size : {1u, 2u, 3u})
                        require(renderer.render_image(graph, RenderRequest{reduced_bounds, tile_size, level}).rgb == reduced.rgb,
                                "resize preview has tile seams");
                    const Rect reduced_roi{reduced_bounds.width - 1, reduced_bounds.height - 1, 1, 1};
                    const auto roi_result = renderer.render_image(graph, RenderRequest{reduced_roi, 1, level});
                    for (unsigned c = 0; c < 3; ++c)
                        require(roi_result.rgb[c] == reduced.rgb[reduced.rgb.size() - 3 + c], "reduced resize ROI differs");
                    const auto reduced_before = cache->stats();
                    renderer.render_image(graph, RenderRequest{reduced_bounds, 16, level});
                    require(cache->stats().hits == reduced_before.hits + 1 && cache->stats().misses == reduced_before.misses,
                            "warm reduced resize cache failed");
                    same_tile(scheduler.submit(graph.output_handle(), graph.output_bounds(), RenderRequest{reduced_bounds, 2, level}).get(),
                              reduced, "scheduled reduced resize differs");
                    require(graph.required_source_region({reduced_bounds.width, reduced_bounds.height, 0, 0}, level).width == 0,
                            "empty reduced resize planned pixels");
                }
                rejects([&] { graph.required_source_regions({0, 0, 1, 1}, {1, RenderQuality::Final}); }, "invalid reduced resize quality planned");
                rejects([&] { renderer.render_image(graph, RenderRequest{{0, 0, 1, 1}, 1, {3, RenderQuality::Preview}}); }, "unsupported resize mip rendered");
                auto bypass = manifest; bypass.operations[0].enabled = false;
                ExecutableEditGraph disabled(bypass, bindings);
                require(same_rect(disabled.output_bounds(), input_bounds), "disabled resize changed extent");
                require(renderer.render_image(disabled, input_bounds).rgb == pixels, "disabled resize changed pixels");
                if (extent.width == 3 && extent.height == 2) {
                    const auto region = graph.required_source_region({1, 0, 1, 1});
                    require(same_rect(region, filter == ResizeFilter::Nearest ? Rect{3, 1, 1, 1} :
                        filter == ResizeFilter::Area ? Rect{2, 0, 3, 3} : Rect{3, 0, 2, 2}),
                            "resize footprint missed interpolation samples");
                    auto revised = manifest;
                    revised.operations[0].parameters["filter"] = EditValue{std::string(filter == ResizeFilter::Nearest ? "bilinear" : "nearest")};
                    const auto changed = renderer.render_image(ExecutableEditGraph(revised, bindings, nullptr, cache), extent, 16);
                    require(changed.rgb != full.rgb, "filter change reused stale resize");
                    same_tile(changed, renderer.render_image(ExecutableEditGraph(revised, bindings), extent, 16), "cached filter revision differs");
                    revised.operations[0].parameters["width"] = EditValue{std::int64_t{4}};
                    same_tile(renderer.render_image(ExecutableEditGraph(revised, bindings, nullptr, cache), {0, 0, 4, 2}, 2),
                              renderer.render_image(ExecutableEditGraph(revised, bindings), {0, 0, 4, 2}, 2), "cached size revision differs");
                }
            }
        auto crop = operation(92, "rawengine.crop", domain, domain, source.id,
            {{"x", EditValue{std::int64_t{1}}}, {"y", EditValue{std::int64_t{1}}},
             {"width", EditValue{std::int64_t{5}}}, {"height", EditValue{std::int64_t{3}}}});
        auto resize = operation(93, "rawengine.resize", domain, domain, crop.id,
            {{"width", EditValue{std::int64_t{3}}}, {"height", EditValue{std::int64_t{2}}}, {"filter", EditValue{std::string("bilinear")}}});
        auto blur = operation(94, "rawengine.box_blur", domain, domain, resize.id, {{"radius", EditValue{std::int64_t{1}}}});
        auto other_crop = crop; other_crop.id = uuid(95);
        other_crop.parameters["x"] = EditValue{std::int64_t{0}}; other_crop.parameters["y"] = EditValue{std::int64_t{0}};
        other_crop.parameters["width"] = EditValue{std::int64_t{3}}; other_crop.parameters["height"] = EditValue{std::int64_t{5}};
        auto other_resize = resize; other_resize.id = uuid(96); other_resize.inputs["image"] = other_crop.id;
        auto mix = operation(97, "rawengine.linear_mix", domain, domain, source.id, {{"amount", EditValue{0.5}}});
        mix.inputs = {{"base", blur.id}, {"layer", other_resize.id}};
        EditManifest composed; composed.working_space = space; composed.sources = {source};
        composed.operations = {mix, other_resize, other_crop, blur, resize, crop}; composed.output_id = mix.id;
        ExecutableEditGraph graph(composed, bindings);
        require(same_rect(graph.required_source_region({1, 0, 1, 1}), {1, 0, 5, 4}),
                "crop/resize/blur/mix source planning is wrong");
        same_tile(renderer.render_image(graph, {0, 0, 3, 2}, 1), renderer.render_image(graph, {0, 0, 3, 2}, 8),
                  "composed geometry has seams");
        for (unsigned mip : {1u, 2u}) {
            const RenderLevel level{mip, RenderQuality::Preview};
            const auto scale = 1u << mip;
            const Rect reduced_bounds{0, 0, (3 + scale - 1) / scale, (2 + scale - 1) / scale};
            require(same_rect(graph.required_source_region({0, 0, 1, 1}, level), {0, 0, 6, 5}),
                    "resize anchor failed to plan crop/blur/mix preview sources");
            same_tile(renderer.render_image(graph, RenderRequest{reduced_bounds, 1, level}),
                      renderer.render_image(graph, RenderRequest{reduced_bounds, 8, level}), "composed reduced geometry has seams");
        }
        auto two_sources = composed;
        auto second_source = source; second_source.id = uuid(98);
        two_sources.sources.push_back(second_source);
        two_sources.operations[2].inputs["image"] = second_source.id;
        two_sources.operations[1].parameters["filter"] = EditValue{std::string("area")};
        two_sources.operations[4].parameters["filter"] = EditValue{std::string("area")};
        auto two_bindings = bindings; two_bindings.push_back({second_source, source_node, input_bounds});
        ExecutableEditGraph multi_area(two_sources, two_bindings);
        const auto branch_regions = multi_area.required_source_regions({0, 0, 1, 1}, {1, RenderQuality::Preview});
        require(branch_regions.size() == 2 && same_rect(branch_regions.at(source.id), {1, 1, 5, 3}) &&
                same_rect(branch_regions.at(second_source.id), {0, 0, 2, 5}), "area preview confused separate source footprints");
        same_tile(renderer.render_image(multi_area, RenderRequest{{0, 0, 2, 1}, 1, {1, RenderQuality::Preview}}),
                  renderer.render_image(multi_area, RenderRequest{{0, 0, 2, 1}, 8, {1, RenderQuality::Preview}}), "two-source area preview seams");
        auto invalid = composed; invalid.operations[4].parameters["filter"] = EditValue{std::string("magic")};
        rejects([&] { ExecutableEditGraph bad(invalid, bindings); }, "unknown resize filter accepted");
        invalid = composed; invalid.operations[4].parameters["height"] = EditValue{std::int64_t{0}};
        rejects([&] { ExecutableEditGraph bad(invalid, bindings); }, "zero resize extent accepted");
        invalid = composed; invalid.operations[4].parameters["width"] = EditValue{3.0};
        rejects([&] { ExecutableEditGraph bad(invalid, bindings); }, "floating resize dimension accepted");
        rejects([&] { ResizeNode bad(nullptr, input_bounds, 3, 2); }, "null resize input accepted");
        rejects([&] { ResizeNode bad(source_node, input_bounds, 3, 2, static_cast<ResizeFilter>(42)); }, "invalid resize enum accepted");
        auto encoded = std::make_shared<SrgbEncodeNode>(std::make_shared<ToneCurveNode>(std::make_shared<WorkingToSrgbNode>(source_node), 1.0f, 1.0f));
        rejects([&] { ResizeNode bad(encoded, input_bounds, 3, 2); }, "encoded resize input accepted");
        ResizeNode rebased(source_node, {2, 1, 3, 3}, 3, 3);
        auto reference = source_node->render({2, 1, 3, 3}); reference.bounds = {0, 0, 3, 3};
        same_tile(rebased.render({0, 0, 3, 3}), reference, "nonzero input origin resize mapping failed");
    }
}

void test_area_quality() {
    for (auto space : {WorkingSpace::LinearProPhotoD50, WorkingSpace::LinearRec2020D65}) {
        std::vector<float> pixels(9 * 9 * 3);
        for (unsigned y = 0; y < 9; ++y)
            for (unsigned x = 0; x < 9; ++x) {
                const auto i = (y * 9 + x) * 3;
                pixels[i] = static_cast<float>((x + y) % 2);
                pixels[i + 1] = x == 4 && y == 4 ? 1.0f : 0.0f;
                pixels[i + 2] = -0.5f + static_cast<float>(x) / 2;
            }
        auto source = std::make_shared<RasterSourceNode>(RasterImage({9, 9, 0, space}, pixels));
        ResizeNode area(source, {0, 0, 9, 9}, 3, 3, ResizeFilter::Area);
        ResizeNode bilinear(source, {0, 0, 9, 9}, 3, 3, ResizeFilter::Bilinear);
        const auto integrated = area.render({0, 0, 3, 3}), aliased = bilinear.render({0, 0, 3, 3});
        auto preblur = std::make_shared<BoxBlurNode>(source, Rect{0, 0, 9, 9}, 1);
        ResizeNode spatial(preblur, {0, 0, 9, 9}, 7, 5, ResizeFilter::Area);
        const auto spatial_native = spatial.render({0, 0, 7, 5});
        const auto spatial_preview = spatial.render_level({0, 0, 4, 3}, {1, RenderQuality::Preview});
        for (unsigned y = 0; y < 3; ++y)
            for (unsigned x = 0; x < 4; ++x)
                for (unsigned c = 0; c < 3; ++c) {
                    double sum = 0; unsigned count = 0;
                    for (unsigned iy = y * 2; iy < std::min(5u, (y + 1) * 2); ++iy)
                        for (unsigned ix = x * 2; ix < std::min(7u, (x + 1) * 2); ++ix) {
                            sum += spatial_native.rgb[(iy * 7 + ix) * 3 + c]; ++count;
                        }
                    require(spatial_preview.rgb[(y * 4 + x) * 3 + c] == static_cast<float>(sum / count),
                            "resize preview ran pre-resize blur at the reduced level");
                }
        double checker_sum = 0, impulse_sum = 0, ramp_sum = 0;
        float area_low = 1, area_high = 0, bilinear_low = 1, bilinear_high = 0;
        for (unsigned i = 0; i < 9; ++i) {
            const auto checker = integrated.rgb[i * 3];
            require(std::abs(checker - (i % 2 ? 5.0f / 9 : 4.0f / 9)) < 1e-6f, "area checker overlaps are wrong");
            area_low = std::min(area_low, checker); area_high = std::max(area_high, checker);
            bilinear_low = std::min(bilinear_low, aliased.rgb[i * 3]); bilinear_high = std::max(bilinear_high, aliased.rgb[i * 3]);
            require(std::abs(integrated.rgb[i * 3 + 1] - (i == 4 ? 1.0f / 9 : 0.0f)) < 1e-6f,
                    "area impulse energy or footprint is wrong");
            checker_sum += checker; impulse_sum += integrated.rgb[i * 3 + 1]; ramp_sum += integrated.rgb[i * 3 + 2];
        }
        require(area_high - area_low < 0.112f && bilinear_high - bilinear_low == 1.0f,
                "area failed to suppress the defined checker alias case");
        require(std::abs(checker_sum / 9 - 40.0 / 81) < 1e-6 && std::abs(impulse_sum * 9 - 1) < 1e-6 &&
                std::abs(ramp_sum / 9 - 1.5) < 1e-6, "area failed mean/energy preservation");
        auto constants = std::make_shared<RasterSourceNode>(RasterImage({7, 5, 0, space}, std::vector<float>(7 * 5 * 3, -0.5f)));
        for (auto size : {std::pair{3u, 2u}, {11u, 9u}, {1u, 1u}}) {
            const auto constant = ResizeNode(constants, {0, 0, 7, 5}, size.first, size.second, ResizeFilter::Area)
                                      .render({0, 0, size.first, size.second});
            require(std::all_of(constant.rgb.begin(), constant.rgb.end(), [](float value) { return value == -0.5f; }),
                    "area failed signed constant preservation");
        }
        auto rect_equal = [](Rect a, Rect b) { return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height; };
        ResizeNode boundary(source, {0, 0, 12, 12}, 3, 3, ResizeFilter::Area);
        require(rect_equal(boundary.input_region({1, 1, 1, 1}, {0, 0, 12, 12}), {4, 4, 4, 4}),
                "area exact boundary includes zero-overlap neighbor");
        const auto maximum = std::numeric_limits<std::uint32_t>::max();
        // Planning only: no giant source allocation or render.
        ResizeNode extreme(source, {0, 0, maximum, maximum}, maximum, maximum, ResizeFilter::Area);
        require(rect_equal(extreme.input_region({maximum - 1, maximum - 1, 1, 1}, {0, 0, maximum, maximum}),
                           {maximum - 1, maximum - 1, 1, 1}), "area rational boundaries overflowed");
        const auto last = maximum / 4;
        require(rect_equal(extreme.input_region_level({last, last, 1, 1}, {0, 0, maximum, maximum}, {2, RenderQuality::Preview}),
                           {maximum - 3, maximum - 3, 3, 3}), "area reduced odd-edge plan overflowed");
        auto raw = std::make_shared<RawUnpackNode>(RawImage(2, 2, std::vector<std::uint16_t>(4, 32768)));
        CameraColorTransform transform; transform.target = space;
        transform.camera_to_xyz_d50 = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        auto camera = std::make_shared<CameraToWorkingNode>(raw, transform);
        ResizeNode raw_resize(camera, {0, 0, 2, 2}, 3, 3, ResizeFilter::Area);
        require(!raw_resize.supports_level({1, RenderQuality::Preview}), "resize enabled an unbounded camera preview");
        rejects([&] { raw_resize.render_level({0, 0, 1, 1}, {1, RenderQuality::Preview}); }, "unbounded camera resized preview rendered");
    }
}

void test_multi_input() {
    Renderer renderer;
    auto rect_equal = [](Rect a, Rect b) {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    };
    for (auto space : {WorkingSpace::LinearProPhotoD50, WorkingSpace::LinearRec2020D65}) {
        const Rect bounds{0, 0, 9, 7};
        std::vector<float> a(9 * 7 * 3), b(a.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            a[i] = static_cast<float>(static_cast<int>(i % 29) - 12) / 8;
            b[i] = static_cast<float>(static_cast<int>(i % 17) - 4) / 4;
        }
        RasterImage ia({9, 7, 0, space}, a), ib({9, 7, 0, space}, b);
        auto na = std::make_shared<RasterSourceNode>(ia), nb = std::make_shared<RasterSourceNode>(ib);
        EditSource sa, sb;
        sa.id = uuid(80); sb.id = uuid(81);
        sa.kind = sb.kind = EditSourceKind::SceneLinearRasterF32;
        sa.working_space = sb.working_space = space;
        sa.content_sha256 = fingerprint_raster_source(ia); sb.content_sha256 = fingerprint_raster_source(ib);
        const auto domain = space == WorkingSpace::LinearProPhotoD50
            ? EditDomain::SceneLinearProPhotoD50 : EditDomain::SceneLinearRec2020D65;
        EditManifest manifest;
        manifest.working_space = space; manifest.sources = {sa, sb};
        auto exp = operation(82, "rawengine.exposure", domain, domain, sa.id, {{"stops", EditValue{1.0}}});
        auto blur = operation(83, "rawengine.box_blur", domain, domain, sb.id,
                              {{"radius", EditValue{std::int64_t{1}}}});
        auto mix = operation(84, "rawengine.linear_mix", domain, domain, sa.id, {{"amount", EditValue{0.25}}});
        mix.inputs = {{"base", exp.id}, {"layer", blur.id}};
        // Deliberately non-topological document order.
        manifest.operations = {mix, blur, exp}; manifest.output_id = mix.id;
        const std::vector<BoundEditSource> bindings{{sa, na, bounds}, {sb, nb, bounds}};
        auto cache = std::make_shared<TileCache>(4 * 1024 * 1024);
        ExecutableEditGraph graph(parse_edit_manifest(serialize_edit_manifest(manifest)), bindings, nullptr, cache);
        rejects([&] { graph.source_bounds(); }, "multi-source graph returned ambiguous source bounds");
        rejects([&] { graph.required_source_region({1, 1, 2, 2}); }, "multi-source graph returned singular ROI");
        for (std::uint32_t mip : {0u, 1u, 2u}) {
            const RenderLevel level{mip, mip ? RenderQuality::Preview : RenderQuality::Final};
            const auto scale = 1u << mip;
            const Rect extent{0, 0, (9 + scale - 1) / scale, (7 + scale - 1) / scale};
            // Independent clipped source reduction, then exposure and clipped box average.
            auto reduced_sample = [&](const std::vector<float>& pixels, unsigned x, unsigned y, unsigned c) {
                double sum = 0; unsigned count = 0;
                for (unsigned sy = y * scale; sy < std::min(7u, (y + 1) * scale); ++sy)
                    for (unsigned sx = x * scale; sx < std::min(9u, (x + 1) * scale); ++sx) {
                        sum += pixels[(sy * 9 + sx) * 3 + c]; ++count;
                    }
                return static_cast<float>(sum / count);
            };
            const auto full = renderer.render_image(graph, RenderRequest{extent, 16, level});
            for (unsigned y = 0; y < extent.height; ++y)
                for (unsigned x = 0; x < extent.width; ++x)
                    for (unsigned c = 0; c < 3; ++c) {
                        double sum = 0; unsigned count = 0;
                        for (unsigned by = y ? y - 1 : 0; by < std::min(extent.height, y + 2); ++by)
                            for (unsigned bx = x ? x - 1 : 0; bx < std::min(extent.width, x + 2); ++bx) {
                                sum += reduced_sample(b, bx, by, c); ++count;
                            }
                        const auto expected = 0.75f * 2 * reduced_sample(a, x, y, c) + 0.25f * static_cast<float>(sum / count);
                        require(std::abs(full.rgb[(y * extent.width + x) * 3 + c] - expected) < 1e-6f,
                                "linear mix differs from independent signed/overrange reference");
                    }
            for (auto tile_size : {1u, 2u, 3u}) {
                const auto tiled = renderer.render_image(graph, RenderRequest{extent, tile_size, level});
                require(tiled.rgb == full.rgb, "two-input output has tile seams");
            }
            const Rect roi{1, 1, 1, 1};
            const auto regions = graph.required_source_regions(roi, level);
            require(regions.size() == 2 && rect_equal(regions.at(sa.id), {scale, scale, scale, std::min(scale, 7 - scale)}) &&
                    rect_equal(regions.at(sb.id), {0, 0, std::min(9u, 3 * scale), std::min(7u, 3 * scale)}),
                    "per-source halo or reduced footprint is wrong");
            const auto cropped = renderer.render_image(graph, RenderRequest{roi, 1, level});
            for (unsigned c = 0; c < 3; ++c)
                require(cropped.rgb[c] == full.rgb[(extent.width + 1) * 3 + c], "multi-input ROI differs");
            TileScheduler scheduler(1, 2);
            same_tile(scheduler.submit(graph.output_handle(), graph.output_bounds(), RenderRequest{extent, 2, level}).get(),
                      full, "scheduled multi-input output differs");
        }
        cache->clear();
        const auto cold = renderer.render_image(graph, bounds, 16);
        const auto cold_stats = cache->stats();
        renderer.render_image(graph, bounds, 16);
        require(cache->stats().hits == cold_stats.hits + 1 && cache->stats().misses == cold_stats.misses,
                "warm mix did not reuse output");
        auto revised = manifest; revised.operations[2].parameters["stops"] = EditValue{2.0};
        ExecutableEditGraph revision(revised, bindings, nullptr, cache);
        const auto before = cache->stats();
        const auto changed = renderer.render_image(revision, bounds, 16);
        require(changed.rgb != cold.rgb && cache->stats().hits >= before.hits + 2 &&
                cache->stats().misses == before.misses + 2, "one-branch revision invalidated unchanged branch or reused stale mix");
        same_tile(changed, renderer.render_image(ExecutableEditGraph(revised, bindings), bounds, 16), "cached revision differs");
        auto layer_revision = manifest; layer_revision.operations[1].parameters["radius"] = EditValue{std::int64_t{2}};
        const auto layer_changed = renderer.render_image(ExecutableEditGraph(layer_revision, bindings, nullptr, cache), bounds, 16);
        require(layer_changed.rgb != cold.rgb, "layer signature failed to invalidate mix");
        same_tile(layer_changed, renderer.render_image(ExecutableEditGraph(layer_revision, bindings), bounds, 16), "layer cache identity differs");
        auto changed_pixels = b; changed_pixels[0] += 2;
        RasterImage changed_image({9, 7, 0, space}, changed_pixels);
        auto changed_source = sb; changed_source.content_sha256 = fingerprint_raster_source(changed_image);
        auto changed_manifest = manifest; changed_manifest.sources[1] = changed_source;
        auto changed_bindings = bindings;
        changed_bindings[1] = {changed_source, std::make_shared<RasterSourceNode>(changed_image), bounds};
        const auto source_changed = renderer.render_image(
            ExecutableEditGraph(changed_manifest, changed_bindings, nullptr, cache), bounds, 16);
        require(source_changed.rgb != cold.rgb, "second-source content change reused stale mix");
        same_tile(source_changed, renderer.render_image(ExecutableEditGraph(changed_manifest, changed_bindings), bounds, 16),
                  "second-source content identity differs with cache");
        for (double amount : {0.0, 1.0}) {
            auto endpoint = manifest; endpoint.operations[0].parameters["amount"] = EditValue{amount};
            ExecutableEditGraph endpoint_graph(endpoint, bindings);
            auto upstream = manifest; upstream.output_id = amount == 0 ? exp.id : blur.id;
            require(renderer.render_image(endpoint_graph, bounds).rgb ==
                    renderer.render_image(ExecutableEditGraph(upstream, bindings), bounds).rgb, "mix endpoint changed samples");
        }
        auto disabled = manifest; disabled.operations[0].enabled = false;
        ExecutableEditGraph bypass(disabled, bindings);
        require(bypass.required_source_regions({1, 1, 1, 1}).size() == 1 && rect_equal(bypass.source_bounds(), bounds),
                "disabled mix did not bypass to base");
        // Two differently anchored crops feed matching output extents.
        auto crops = manifest;
        crops.operations[2] = operation(82, "rawengine.crop", domain, domain, sa.id,
            {{"x", EditValue{std::int64_t{2}}}, {"y", EditValue{std::int64_t{1}}},
             {"width", EditValue{std::int64_t{5}}}, {"height", EditValue{std::int64_t{3}}}});
        crops.operations[1] = crops.operations[2]; crops.operations[1].id = blur.id;
        crops.operations[1].inputs["image"] = sb.id;
        crops.operations[1].parameters["x"] = EditValue{std::int64_t{1}};
        crops.operations[1].parameters["y"] = EditValue{std::int64_t{2}};
        ExecutableEditGraph crop_graph(crops, bindings);
        const auto regions = crop_graph.required_source_regions({0, 0, 1, 1}, {1, RenderQuality::Preview});
        require(rect_equal(regions.at(sa.id), {2, 1, 2, 2}) && rect_equal(regions.at(sb.id), {1, 2, 2, 2}),
                "multi-input crop anchors confused source coordinates");
        same_tile(renderer.render_image(crop_graph, RenderRequest{{0, 0, 3, 2}, 1, {1, RenderQuality::Preview}}),
                  renderer.render_image(crop_graph, RenderRequest{{0, 0, 3, 2}, 8, {1, RenderQuality::Preview}}), "crop mix seams");
        // Shared source ID must union both branch footprints.
        crops.operations[1].inputs["image"] = sa.id;
        ExecutableEditGraph shared(crops, bindings);
        require(rect_equal(shared.required_source_region({0, 0, 1, 1}, {1, RenderQuality::Preview}), {1, 1, 3, 3}),
                "shared-source branches failed to union footprints");
        auto after_mix = manifest;
        after_mix.operations.push_back(operation(85, "rawengine.box_blur", domain, domain, mix.id,
            {{"radius", EditValue{std::int64_t{1}}}}));
        after_mix.output_id = uuid(85);
        ExecutableEditGraph downstream(after_mix, bindings);
        const auto after_regions = downstream.required_source_regions({2, 2, 1, 1});
        require(rect_equal(after_regions.at(sa.id), {1, 1, 3, 3}) &&
                rect_equal(after_regions.at(sb.id), {0, 0, 5, 5}), "downstream halo failed to reach both branches");
        same_tile(renderer.render_image(downstream, bounds, 1), renderer.render_image(downstream, bounds, 16),
                  "downstream blur of mix has seams");
        auto invalid = manifest; invalid.operations[0].parameters["amount"] = EditValue{1.1};
        rejects([&] { ExecutableEditGraph bad(invalid, bindings); }, "out-of-range mix accepted");
        invalid = manifest; invalid.operations[0].inputs.erase("layer");
        rejects([&] { ExecutableEditGraph bad(invalid, bindings); }, "missing mix port accepted");
        invalid = manifest; invalid.operations[0].inputs["image"] = sa.id;
        rejects([&] { ExecutableEditGraph bad(invalid, bindings); }, "extra mix port accepted");
        invalid = crops; invalid.operations[1].parameters["width"] = EditValue{std::int64_t{4}};
        rejects([&] { ExecutableEditGraph bad(invalid, bindings); }, "mismatched mix extents accepted");
        rejects([&] { graph.required_source_regions({0, 0, 1, 1}, {1, RenderQuality::Final}); }, "unsupported mix quality accepted");
        auto other = std::make_shared<RasterSourceNode>(RasterImage({9, 7, 0,
            space == WorkingSpace::LinearProPhotoD50 ? WorkingSpace::LinearRec2020D65 : WorkingSpace::LinearProPhotoD50}, b));
        rejects([&] { LinearMixNode bad(na, other, 0.5f); }, "mixed working spaces accepted");
        rejects([&] { LinearMixNode bad(na, nb, std::numeric_limits<float>::quiet_NaN()); }, "nonfinite mix accepted");
        rejects([&] { LinearMixNode bad(nullptr, nb, 0.5f); }, "null mix input accepted");
        auto aliased_manifest = manifest;
        aliased_manifest.sources[1].content_sha256 = sa.content_sha256;
        auto aliased_bindings = bindings;
        aliased_bindings[1] = {aliased_manifest.sources[1], na, bounds};
        aliased_manifest.output_id = sa.id;
        ExecutableEditGraph aliased(aliased_manifest, aliased_bindings);
        const auto aliased_regions = aliased.required_source_regions({1, 1, 1, 1});
        require(aliased_regions.size() == 1 && aliased_regions.contains(sa.id),
                "shared runtime node confused distinct source IDs");
    }
}

void test_crop_coordinates() {
    constexpr std::uint32_t width = 9, height = 7;
    const Rect original_bounds{0, 0, width, height}, crop_bounds{0, 0, 5, 5};
    const Rect crop_rect{2, 1, 5, 5};
    Renderer renderer;
    auto same_rect = [](Rect a, Rect b) {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    };
    for (auto space : {WorkingSpace::LinearProPhotoD50, WorkingSpace::LinearRec2020D65}) {
        std::vector<float> pixels(width * height * 3);
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x) {
                pixels[(y * width + x) * 3] = static_cast<float>(10 * y + x);
                pixels[(y * width + x) * 3 + 1] = -0.25f + 0.1f * x;
                pixels[(y * width + x) * 3 + 2] = 1.25f + 0.2f * y;
            }
        RasterImage image({width, height, 0, space}, pixels);
        auto source_node = std::make_shared<RasterSourceNode>(image);
        EditSource source;
        source.id = uuid(60);
        source.kind = EditSourceKind::SceneLinearRasterF32;
        source.working_space = space;
        source.content_sha256 = fingerprint_raster_source(image);
        const auto domain = space == WorkingSpace::LinearProPhotoD50
            ? EditDomain::SceneLinearProPhotoD50 : EditDomain::SceneLinearRec2020D65;
        auto crop_op = [&](unsigned id, std::string input, Rect crop) {
            return operation(id, "rawengine.crop", domain, domain, std::move(input),
                {{"x", EditValue{static_cast<std::int64_t>(crop.x)}},
                 {"y", EditValue{static_cast<std::int64_t>(crop.y)}},
                 {"width", EditValue{static_cast<std::int64_t>(crop.width)}},
                 {"height", EditValue{static_cast<std::int64_t>(crop.height)}}});
        };
        EditManifest manifest;
        manifest.working_space = space;
        manifest.sources.push_back(source);
        manifest.operations.push_back(crop_op(61, source.id, crop_rect));
        manifest.output_id = uuid(61);
        auto cache = std::make_shared<TileCache>(256 * 1024);
        const BoundEditSource binding{source, source_node, original_bounds};
        ExecutableEditGraph graph(parse_edit_manifest(serialize_edit_manifest(manifest)), {binding}, nullptr, cache);
        require(same_rect(graph.source_bounds(), original_bounds) && same_rect(graph.output_bounds(), crop_bounds),
                "crop confused original source and rebased output extents");
        auto expected = source_node->render(crop_rect);
        expected.bounds = crop_bounds;
        for (std::uint32_t tile_size : {1u, 2u, 3u, 8u}) {
            const auto actual = renderer.render_image(graph, crop_bounds, tile_size);
            same_tile(actual, expected, "crop changed source samples or color descriptor");
            require(actual.rgb == expected.rgb, "native crop has a tile seam");
        }
        const auto before_warm = cache->stats();
        same_tile(renderer.render_image(graph, crop_bounds, 1), expected, "warm crop changed output");
        require(cache->stats().hits > before_warm.hits && cache->stats().misses == before_warm.misses,
                "warm crop failed to reuse its cached tiles");
        const Rect roi{1, 1, 2, 2};
        require(same_rect(graph.required_source_region(roi), {3, 2, 2, 2}),
                "crop ROI did not translate into original source pixels");
        auto roi_expected = source_node->render({3, 2, 2, 2});
        roi_expected.bounds = roi;
        same_tile(renderer.render_image(graph, roi, 1), roi_expected, "crop ROI differs from original source");
        TileScheduler scheduler(1, 2);
        same_tile(scheduler.submit(graph.output_handle(), graph.output_bounds(), crop_bounds).get(),
                  expected, "scheduled crop did not use its output extent");
        auto shifted = manifest;
        shifted.operations[0].parameters["x"] = EditValue{std::int64_t{3}};
        ExecutableEditGraph revised(shifted, {binding}, nullptr, cache);
        auto revised_expected = source_node->render({3, 1, 5, 5});
        revised_expected.bounds = crop_bounds;
        const auto before_shift = cache->stats();
        same_tile(renderer.render_image(revised, crop_bounds, 1), revised_expected,
                  "changed crop origin reused stale output");
        require(cache->stats().hits > before_shift.hits && revised_expected.rgb != expected.rgb,
                "shifted crop failed to reuse overlapping original source tiles");

        // Blur after crop clips at crop edges; blur before crop can sample outside.
        auto after_manifest = manifest;
        after_manifest.operations.push_back(operation(62, "rawengine.box_blur", domain, domain,
            uuid(61), {{"radius", EditValue{std::int64_t{1}}}}));
        after_manifest.output_id = uuid(62);
        ExecutableEditGraph after(after_manifest, {binding}, nullptr, cache);
        auto direct_crop = std::make_shared<CropNode>(source_node, original_bounds, crop_rect);
        auto direct_after = std::make_shared<BoxBlurNode>(direct_crop, crop_bounds, 1);
        const auto after_full = direct_after->render(crop_bounds);
        require(std::abs(after_full.rgb[0] - 17.5f) < 1e-6f &&
                same_rect(after.required_source_region({0, 0, 1, 1}), {2, 1, 2, 2}),
                "post-crop blur used original source edges instead of crop edges");
        auto before_manifest = manifest;
        before_manifest.operations.insert(before_manifest.operations.begin(),
            operation(62, "rawengine.box_blur", domain, domain, source.id,
                {{"radius", EditValue{std::int64_t{1}}}}));
        before_manifest.operations[1].inputs["image"] = uuid(62);
        ExecutableEditGraph before(before_manifest, {binding}, nullptr, cache);
        auto direct_before = std::make_shared<BoxBlurNode>(source_node, original_bounds, 1);
        CropNode before_crop(direct_before, original_bounds, crop_rect);
        const auto before_full = before_crop.render(crop_bounds);
        require(std::abs(before_full.rgb[0] - 12.0f) < 1e-6f &&
                same_rect(before.required_source_region({0, 0, 1, 1}), {1, 0, 3, 3}),
                "pre-crop blur lost its original-coordinate halo");
        for (std::uint32_t tile_size : {1u, 2u, 3u, 8u}) {
            same_tile(renderer.render_image(after, crop_bounds, tile_size), after_full,
                      "post-crop blur seam or incorrect output bounds");
            same_tile(renderer.render_image(before, crop_bounds, tile_size), before_full,
                      "pre-crop blur seam or incorrect output bounds");
        }
        auto nested_manifest = after_manifest;
        nested_manifest.operations.push_back(crop_op(63, uuid(62), {1, 1, 3, 3}));
        nested_manifest.operations.push_back(operation(64, "rawengine.exposure", domain, domain,
            uuid(63), {{"stops", EditValue{0.5}}}));
        nested_manifest.output_id = uuid(64);
        ExecutableEditGraph nested(nested_manifest, {binding}, nullptr, cache);
        auto nested_crop = std::make_shared<CropNode>(direct_after, crop_bounds, Rect{1, 1, 3, 3});
        ExposureNode direct_nested(nested_crop, 0.5f);
        require(same_rect(nested.output_bounds(), {0, 0, 3, 3}) &&
                same_rect(nested.required_source_region({0, 0, 1, 1}), {2, 1, 3, 3}),
                "nested crop/blur/exposure failed to compose coordinate spaces");
        same_tile(renderer.render_image(nested, nested.output_bounds(), 1),
                  direct_nested.render(nested.output_bounds()), "nested crop has incorrect pixels");
        auto disabled_manifest = after_manifest;
        disabled_manifest.operations[0].enabled = false;
        ExecutableEditGraph disabled(disabled_manifest, {binding}, nullptr, cache);
        require(same_rect(disabled.output_bounds(), original_bounds) &&
                same_rect(disabled.required_source_region({0, 0, 1, 1}), {0, 0, 2, 2}),
                "disabled crop retained its changed bounds or coordinate mapping");
        same_tile(renderer.render_image(disabled, original_bounds, 2),
                  direct_before->render(original_bounds), "disabled crop failed to bypass pixels");
        for (const auto& invalid_value : {EditValue{std::int64_t{-1}}, EditValue{1.5}}) {
            auto invalid = manifest;
            invalid.operations[0].parameters["x"] = invalid_value;
            rejects([&] { validate_edit_manifest(invalid); }, "invalid crop origin accepted");
        }
        auto invalid = manifest;
        invalid.operations[0].parameters["width"] = EditValue{std::int64_t{0}};
        rejects([&] { validate_edit_manifest(invalid); }, "empty crop extent accepted");
        invalid = manifest;
        invalid.operations[0].parameters["x"] = EditValue{std::int64_t{8}};
        rejects([&] { ExecutableEditGraph bad(invalid, {binding}); }, "out-of-bounds crop executed");
        for (std::uint32_t mip : {1u, 2u}) {
            const auto scale = 1u << mip;
            const Rect reduced_bounds{0, 0, (5 + scale - 1) / scale, (5 + scale - 1) / scale};
            const RenderLevel level{mip, RenderQuality::Preview};
            const RenderRequest request{reduced_bounds, 1, level};
            RasterImage reference_image({5, 5, 0, space}, expected.rgb);
            auto reference_source = std::make_shared<RasterSourceNode>(reference_image);
            const auto reduced_expected = renderer.render_image(*reference_source, crop_bounds, request);
            const auto before_level = cache->stats();
            const auto reduced_crop = renderer.render_image(graph, request);
            same_tile(reduced_crop, reduced_expected, "reduced crop differs from independent cropped source");
            require(std::abs(reduced_crop.rgb[0] - (mip == 1 ? 17.5f : 28.5f)) < 1e-6f &&
                    reduced_crop.rgb[reduced_crop.rgb.size() - 3] == 56.0f &&
                    cache->stats().misses > before_level.misses,
                    "crop preview lost its offset anchor, odd-edge weighting or level identity");
            const auto before_repeat = cache->stats();
            same_tile(renderer.render_image(graph, request), reduced_expected, "warm reduced crop changed samples");
            require(cache->stats().hits > before_repeat.hits && cache->stats().misses == before_repeat.misses,
                    "warm reduced crop failed to reuse its cache");
            for (std::uint32_t tile_size : {2u, 3u, 8u}) {
                const auto tiled = renderer.render_image(graph, RenderRequest{reduced_bounds, tile_size, level});
                require(tiled.rgb == reduced_crop.rgb, "reduced crop has a tiled seam");
            }
            const Rect edge_roi{reduced_bounds.width - 1, reduced_bounds.height - 1, 1, 1};
            require(same_rect(graph.required_source_region(edge_roi, level), {6, 5, 1, 1}),
                    "reduced crop source ROI did not use clipped crop-origin footprints");
            same_tile(renderer.render_image(graph, RenderRequest{edge_roi, 1, level}),
                      renderer.render_image(*reference_source, crop_bounds, RenderRequest{edge_roi, 1, level}),
                      "reduced crop ROI differs from full preview");
            same_tile(scheduler.submit(graph.output_handle(), graph.output_bounds(), request).get(),
                      reduced_expected, "scheduled reduced crop differs");
            BoxBlurNode reference_after(reference_source, crop_bounds, 1);
            same_tile(renderer.render_image(after, request),
                      renderer.render_image(reference_after, crop_bounds, request),
                      "blur after crop did not run in reduced crop pixels");
            const auto footprint = std::min(5u, 2 * scale);
            require(same_rect(after.required_source_region({0, 0, 1, 1}, level), {2, 1, footprint, footprint}),
                    "reduced post-crop blur halo mapped through the wrong level");
            RasterSourceNode before_reference(RasterImage({5, 5, 0, space}, before_full.rgb));
            same_tile(renderer.render_image(before, request),
                      renderer.render_image(before_reference, crop_bounds, request),
                      "blur before crop failed to use native inputs at reduction anchor");
            const auto nested_size = (3 + scale - 1) / scale;
            const RenderRequest nested_request{{0, 0, nested_size, nested_size}, 1, level};
            auto nested_native = nested_crop->render({0, 0, 3, 3});
            auto nested_source = std::make_shared<RasterSourceNode>(RasterImage({3, 3, 0, space}, nested_native.rgb));
            ExposureNode reference_nested(nested_source, 0.5f);
            same_tile(renderer.render_image(nested, nested_request),
                      renderer.render_image(reference_nested, {0, 0, 3, 3}, nested_request),
                      "nested crop did not use its final crop as reduction anchor");
            const auto nested_footprint = mip == 1 ? 4u : 5u;
            require(same_rect(nested.required_source_region({0, 0, 1, 1}, level),
                              {2, 1, nested_footprint, nested_footprint}),
                    "nested reduced crop/native blur ROI levels failed to compose");
            auto invalid_request = request;
            invalid_request.level.quality = RenderQuality::Final;
            rejects([&] { renderer.render_image(graph, invalid_request); }, "reduced crop final quality accepted");
        }
        rejects([&] { renderer.render_image(graph,
            RenderRequest{{0, 0, 1, 1}, 1, {3, RenderQuality::Preview}}); }, "unsupported crop mip accepted");
        const auto empty = renderer.render_image(graph, {5, 0, 0, 1}, 1);
        require(empty.rgb.empty(), "empty crop viewport produced samples");
        CropNode extreme(source_node, {std::numeric_limits<std::uint32_t>::max(), 0, 1, 1},
                         {std::numeric_limits<std::uint32_t>::max(), 0, 1, 1});
        try {
            extreme.input_region({1, 0, 0, 1}, {std::numeric_limits<std::uint32_t>::max(), 0, 1, 1});
            throw std::runtime_error("crop coordinate translation overflowed silently");
        } catch (const std::out_of_range&) {}
    }
    auto raw_source = std::make_shared<RawUnpackNode>(RawImage(2, 2, std::vector<std::uint16_t>(4, 32768)));
    CameraColorTransform calibration;
    calibration.camera_to_xyz_d50 = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    auto camera = std::make_shared<CameraToWorkingNode>(raw_source, calibration);
    CropNode raw_crop(camera, {0, 0, 2, 2}, {0, 0, 2, 2});
    require(!raw_crop.supports_level({1, RenderQuality::Preview}), "crop enabled an unbounded camera preview");
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

    auto external_state = std::make_shared<SchedulerProbeState>();
    auto external_entered = external_state->entered.get_future();
    auto external_node = std::make_shared<SchedulerProbeNode>(external_state);
    TileScheduler externally_cancelled(1, 1);
    auto external_blocker = externally_cancelled.submit(external_node, source_bounds, {0, 0, 1, 1});
    external_entered.wait();
    auto old_token = std::make_shared<CancellationToken>();
    auto new_token = std::make_shared<CancellationToken>();
    auto external_old = externally_cancelled.submit_latest("viewport", external_node,
        source_bounds, RenderRequest{{1, 0, 1, 1}}, RenderPriority::Interactive, old_token);
    auto external_new = externally_cancelled.submit_latest("viewport", external_node,
        source_bounds, {2, 0, 1, 1}, RenderPriority::Interactive, 256, new_token);
    new_token->cancel();
    external_state->release.set_value();
    external_blocker.get();
    for (auto* future : {&external_old, &external_new}) {
        try {
            future->get();
            throw std::runtime_error("externally cancelled latest request rendered");
        } catch (const RenderCancelled&) {}
    }
    require(old_token->is_cancelled() && new_token->is_cancelled() &&
            external_state->order == std::vector<std::uint32_t>({0}),
            "latest supersession lost its caller cancellation token");

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
        test_srgb_mip_preview();
        test_crop_coordinates();
        test_multi_input();
        test_resize_geometry();
        test_orientation_geometry();
        test_area_quality();
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

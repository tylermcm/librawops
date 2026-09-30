#include "EditGraph.hpp"
#include "TileScheduler.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace rawengine;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool equal(Rect a, Rect b) { return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height; }
void same(const Tile& a, const Tile& b, bool exact = false) {
    require(equal(a.bounds, b.bounds) && a.descriptor == b.descriptor && a.rgb.size() == b.rgb.size(), "RAW preview tile metadata differs");
    for (std::size_t i = 0; i < a.rgb.size(); ++i)
        require(exact ? a.rgb[i] == b.rgb[i] : std::abs(a.rgb[i] - b.rgb[i]) < 1e-6f, "RAW preview pixels differ");
}
template<class F> void rejects(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("unsupported RAW preview accepted");
}
std::string id(unsigned value) {
    auto result = std::string("00000000-0000-0000-0000-000000000000");
    constexpr char digits[] = "0123456789abcdef";
    result[34] = digits[value / 16]; result[35] = digits[value % 16]; return result;
}
EditOperation op(unsigned index, std::string type, EditDomain in, EditDomain out,
                 unsigned upstream, EditValue::Object parameters = {}) {
    EditOperation result;
    result.id = id(index); result.type_id = std::move(type);
    result.processing_version = kCurrentEditProcessingVersion;
    result.input_domain = in; result.output_domain = out;
    result.inputs.emplace("image", id(upstream)); result.parameters = std::move(parameters);
    return result;
}
EditDomain domain(WorkingSpace space) {
    return space == WorkingSpace::LinearProPhotoD50 ? EditDomain::SceneLinearProPhotoD50 : EditDomain::SceneLinearRec2020D65;
}
CameraColorTransform calibration(WorkingSpace space) {
    return {{0.55, 0.22, 0.13, 0.17, 0.68, 0.09, 0.03, 0.11, 0.73}, space};
}
RawImage fixture(BayerPattern pattern, unsigned px, unsigned py, Rect area) {
    RawMetadata metadata;
    metadata.width = 11; metadata.height = 10; metadata.row_stride_samples = 14;
    metadata.pattern = pattern; metadata.cfa_phase_x = static_cast<std::uint8_t>(px);
    metadata.cfa_phase_y = static_cast<std::uint8_t>(py); metadata.active_area = area;
    metadata.black_levels = {4000, 7000, 3000, 8000};
    metadata.white_levels = {19000, 23000, 16000, 27000};
    std::vector<std::uint16_t> samples(140, 65535);
    for (unsigned y = 0; y < 10; ++y)
        for (unsigned x = 0; x < 11; ++x)
            samples[y * 14 + x] = static_cast<std::uint16_t>((x * 7919 + y * 5987 + x * y * 677) % 41000);
    return RawImage(metadata, std::move(samples));
}
std::shared_ptr<const Node> camera_node(const RawImage& raw, WorkingSpace space) {
    auto source = std::make_shared<RawUnpackNode>(raw);
    auto wb = std::make_shared<WhiteBalanceNode>(source, 1.25f, 0.8f, 1.6f);
    auto exposure = std::make_shared<ExposureNode>(wb, 0.5f);
    return std::make_shared<CameraToWorkingNode>(exposure, calibration(space), raw.metadata().active_area);
}
EditManifest manifest(const RawImage& raw, WorkingSpace space) {
    EditManifest result; result.working_space = space;
    EditSource source; source.id = id(1); source.kind = EditSourceKind::DecodedBayerU16;
    source.content_sha256 = raw.fingerprint(); result.sources.push_back(source);
    result.operations.push_back(op(2, "rawengine.white_balance", EditDomain::CameraLinear, EditDomain::CameraLinear, 1,
        {{"red_gain", EditValue{1.25}}, {"green_gain", EditValue{0.8}}, {"blue_gain", EditValue{1.6}}}));
    result.operations.push_back(op(3, "rawengine.exposure", EditDomain::CameraLinear, EditDomain::CameraLinear, 2,
        {{"stops", EditValue{0.5}}}));
    EditValue::Array matrix; for (double value : calibration(space).camera_to_xyz_d50) matrix.emplace_back(value);
    result.operations.push_back(op(4, "rawengine.camera_to_working", EditDomain::CameraLinear, domain(space), 3,
        {{"matrix", EditValue{std::move(matrix)}}}));
    result.output_id = id(4); return result;
}
Tile average(const Tile& native, unsigned mip) {
    const auto scale = 1u << mip;
    Tile result{{0, 0, (native.bounds.width + scale - 1) / scale, (native.bounds.height + scale - 1) / scale}, {}, native.descriptor};
    for (unsigned y = 0; y < result.bounds.height; ++y)
        for (unsigned x = 0; x < result.bounds.width; ++x) {
            double sum[3]{}; unsigned count = 0;
            for (unsigned sy = y * scale; sy < std::min((y + 1) * scale, native.bounds.height); ++sy)
                for (unsigned sx = x * scale; sx < std::min((x + 1) * scale, native.bounds.width); ++sx) {
                    for (unsigned c = 0; c < 3; ++c) sum[c] += native.rgb[(sy * native.bounds.width + sx) * 3 + c];
                    ++count;
                }
            for (double value : sum) result.rgb.push_back(static_cast<float>(value / count));
        }
    return result;
}
Rect sensor_footprint(Rect roi, Rect area, unsigned mip) {
    const unsigned scale = 1u << mip;
    const auto left = area.x + std::min(roi.x * scale, area.width);
    const auto top = area.y + std::min(roi.y * scale, area.height);
    const auto right = area.x + std::min((roi.x + roi.width) * scale, area.width);
    const auto bottom = area.y + std::min((roi.y + roi.height) * scale, area.height);
    const auto x = left > area.x ? left - 1 : left, y = top > area.y ? top - 1 : top;
    return {x, y, std::min(right + 1, area.x + area.width) - x,
                  std::min(bottom + 1, area.y + area.height) - y};
}
std::shared_ptr<const Node> downstream(std::shared_ptr<const Node> input, WorkingSpace space) {
    input = std::make_shared<ExposureNode>(input, -0.25f);
    const auto target = space == WorkingSpace::LinearProPhotoD50 ? WorkingSpace::LinearRec2020D65 : WorkingSpace::LinearProPhotoD50;
    input = std::make_shared<WorkingSpaceConvertNode>(input, target);
    input = std::make_shared<WorkingToSrgbNode>(input);
    input = std::make_shared<ToneCurveNode>(input, 0.4f, 1.15f);
    return std::make_shared<SrgbEncodeNode>(input);
}
void append_output(EditManifest& m, WorkingSpace space) {
    const auto target = space == WorkingSpace::LinearProPhotoD50 ? WorkingSpace::LinearRec2020D65 : WorkingSpace::LinearProPhotoD50;
    m.operations.push_back(op(5, "rawengine.exposure", domain(space), domain(space), 4, {{"stops", EditValue{-0.25}}}));
    m.operations.push_back(op(6, "rawengine.working_space_convert", domain(space), domain(target), 5));
    m.operations.push_back(op(7, "rawengine.working_to_srgb", domain(target), EditDomain::SceneLinearSrgb, 6));
    m.operations.push_back(op(8, "rawengine.tone_curve", EditDomain::SceneLinearSrgb, EditDomain::DisplayLinearSrgb, 7,
        {{"shoulder", EditValue{0.4}}, {"gamma", EditValue{1.15}}}));
    m.operations.push_back(op(9, "rawengine.srgb_encode", EditDomain::DisplayLinearSrgb, EditDomain::DisplayEncodedSrgb, 8));
    m.output_id = id(9);
}
void test_patterns_and_levels() {
    Renderer renderer; TileScheduler scheduler(2, 8);
    for (const auto space : {WorkingSpace::LinearProPhotoD50, WorkingSpace::LinearRec2020D65})
        for (unsigned pattern = 0; pattern < 4; ++pattern)
            for (unsigned phase = 0; phase < 4; ++phase)
                for (Rect area : {Rect{0, 0, 7, 5}, Rect{1, 3, 7, 5}, Rect{2, 2, 1, 5}, Rect{3, 1, 5, 1}, Rect{4, 4, 1, 1}}) {
                    const auto raw = fixture(static_cast<BayerPattern>(pattern), phase % 2, phase / 2, area);
                    const auto camera = camera_node(raw, space);
                    const auto native = renderer.render_image(*camera, area, area, 50);
                    same(renderer.render_image(*camera, area, area, 1), native, true);
                    auto m = manifest(raw, space);
                    auto cache = std::make_shared<TileCache>(1024 * 1024);
                    ExecutableEditGraph graph(parse_edit_manifest(serialize_edit_manifest(m)),
                        {{m.sources[0], std::make_shared<RawUnpackNode>(raw), area}}, nullptr, cache);
                    same(renderer.render_image(graph, area, 2), native, true);
                    require(equal(graph.output_bounds(), area), "native RAW origin changed");
                    for (unsigned mip : {1u, 2u}) {
                        const RenderLevel level{mip, RenderQuality::Preview};
                        const auto expected = average(native, mip);
                        const RenderRequest request{expected.bounds, 50, level};
                        same(renderer.render_image(*camera, area, request), expected, true);
                        same(renderer.render_image(graph, request), expected, true);
                        same(renderer.render_image(graph, RenderRequest{expected.bounds, 1, level}), expected, true);
                        same(scheduler.submit(graph.output_handle(), graph.output_bounds(), request).get(), expected, true);
                        require(equal(graph.required_source_region(expected.bounds, level), area), "full RAW footprint differs");
                        const Rect roi{expected.bounds.width - 1, expected.bounds.height - 1, 1, 1};
                        const auto tile = renderer.render_image(graph, RenderRequest{roi, 1, level});
                        require(std::equal(tile.rgb.begin(), tile.rgb.end(), expected.rgb.end() - 3), "edge RAW ROI differs");
                        require(equal(graph.required_source_region(roi, level), sensor_footprint(roi, area, mip)), "RAW reduced sensor halo differs");
                        const auto before = cache->stats();
                        same(renderer.render_image(graph, request), expected, true);
                        const auto after = cache->stats();
                        require(after.hits > before.hits && after.misses == before.misses, "RAW warm cache missed");
                        auto output_manifest = m; append_output(output_manifest, space);
                        ExecutableEditGraph output_graph(output_manifest,
                            {{m.sources[0], std::make_shared<RawUnpackNode>(raw), area}}, nullptr, cache);
                        auto reduced_source = std::make_shared<RasterSourceNode>(RasterImage(
                            {expected.bounds.width, expected.bounds.height, 0, space}, expected.rgb));
                        const auto output_reference = renderer.render_image(*downstream(reduced_source, space), expected.bounds, expected.bounds, 50);
                        same(renderer.render_image(output_graph, request), output_reference);
                        same(renderer.render_image(output_graph, RenderRequest{expected.bounds, 1, level}), output_reference);
                        require(equal(output_graph.required_source_region(roi, level), sensor_footprint(roi, area, mip)), "output RAW source mapping differs");
                        rejects([&] { renderer.render_image(graph, RenderRequest{expected.bounds, 1, {mip, RenderQuality::Final}}); });
                    }
                    rejects([&] { graph.required_source_region({0, 0, 1, 1}, {0, RenderQuality::Preview}); });
                    rejects([&] { scheduler.submit(graph.output_handle(), area, RenderRequest{{0, 0, 1, 1}, 1, {3, RenderQuality::Preview}}); });
                }
}
void test_composition_and_gates() {
    const Rect area{1, 3, 7, 5}; const auto space = WorkingSpace::LinearProPhotoD50;
    const auto raw = fixture(BayerPattern::GBRG, 1, 1, area); auto camera = camera_node(raw, space);
    Renderer renderer;
    const auto native = renderer.render_image(*camera, area, area);
    require(*std::min_element(native.rgb.begin(), native.rgb.end()) < 0 && *std::max_element(native.rgb.begin(), native.rgb.end()) > 1,
            "RAW preview fixture lost signed/headroom coverage");
    for (unsigned mip : {1u, 2u}) {
        const RenderLevel level{mip, RenderQuality::Preview};
        const auto reduced = average(native, mip);
        auto raster = std::make_shared<RasterSourceNode>(RasterImage({reduced.bounds.width, reduced.bounds.height, 0, space}, reduced.rgb));
        BoxBlurNode preview_blur(camera, area, 1), reference_blur(raster, reduced.bounds, 1);
        same(renderer.render_image(preview_blur, area, RenderRequest{reduced.bounds, 1, level}),
             renderer.render_image(reference_blur, reduced.bounds, reduced.bounds, 1));
        auto m = manifest(raw, space);
        m.operations.push_back(op(5, "rawengine.box_blur", domain(space), domain(space), 4, {{"radius", EditValue{std::int64_t{1}}}}));
        m.output_id = id(5);
        ExecutableEditGraph graph(m, {{m.sources[0], std::make_shared<RawUnpackNode>(raw), area}});
        const Rect roi{0, 0, 1, 1};
        const Rect halo{0, 0, std::min(2u, reduced.bounds.width), std::min(2u, reduced.bounds.height)};
        require(equal(graph.required_source_region(roi, level), sensor_footprint(halo, area, mip)), "RAW blur/demosaic halos did not compose");
        same(renderer.render_image(graph, RenderRequest{reduced.bounds, 1, level}),
             renderer.render_image(reference_blur, reduced.bounds, reduced.bounds));
        same(renderer.render_image(graph, RenderRequest{{0, 0, 0, reduced.bounds.height}, 1, level}),
             Tile{{0, 0, 0, reduced.bounds.height}, {}, camera->output_descriptor()}, true);
        // The last geometry stage replaces the camera reduction anchor.
        auto crop = std::make_shared<CropNode>(camera, area, Rect{2, 4, 5, 3});
        auto rotate = std::make_shared<OrientationNode>(crop, crop->output_bounds(), 1, true, false);
        ResizeNode resize(rotate, rotate->output_bounds(), 7, 5, ResizeFilter::Area);
        const auto resized_native = renderer.render_image(resize, resize.output_bounds(), resize.output_bounds());
        const auto resized_expected = average(resized_native, mip);
        same(renderer.render_image(resize, resize.output_bounds(), RenderRequest{resized_expected.bounds, 1, level}), resized_expected, true);
        auto geometry = manifest(raw, space);
        geometry.operations.push_back(op(5, "rawengine.crop", domain(space), domain(space), 4,
            {{"x", EditValue{std::int64_t{2}}}, {"y", EditValue{std::int64_t{4}}},
             {"width", EditValue{std::int64_t{5}}}, {"height", EditValue{std::int64_t{3}}}}));
        geometry.operations.push_back(op(6, "rawengine.orientation", domain(space), domain(space), 5,
            {{"quarter_turns", EditValue{std::int64_t{1}}}, {"flip_horizontal", EditValue{true}}, {"flip_vertical", EditValue{false}}}));
        geometry.operations.push_back(op(7, "rawengine.resize", domain(space), domain(space), 6,
            {{"width", EditValue{std::int64_t{7}}}, {"height", EditValue{std::int64_t{5}}}, {"filter", EditValue{std::string("area")}}}));
        geometry.output_id = id(7);
        ExecutableEditGraph geometry_graph(parse_edit_manifest(serialize_edit_manifest(geometry)),
            {{geometry.sources[0], std::make_shared<RawUnpackNode>(raw), area}});
        same(renderer.render_image(geometry_graph, RenderRequest{resized_expected.bounds, 1, level}), resized_expected, true);
        const Rect reduced_roi{0, 0, 1, 1};
        const auto resized_input = resize.input_region_level(reduced_roi, rotate->output_bounds(), level);
        const auto cropped_input = rotate->input_region(resized_input, crop->output_bounds());
        const auto calibrated_input = crop->input_region(cropped_input, area);
        RawUnpackNode unpack(raw);
        const auto footprint = unpack.input_region(calibrated_input, area);
        require(equal(geometry_graph.required_source_region(reduced_roi, level), footprint), "RAW geometry anchor source mapping differs");
        require(equal(geometry_graph.required_source_region(resized_expected.bounds, level), {1, 3, 7, 5}), "RAW geometry lost the sensor halo");
    }
    GraphRecipe recipe; recipe.camera_color = calibration(space); recipe.output_mode = OutputMode::SrgbPreview;
    recipe.red_gain = 1.25f; recipe.green_gain = 0.8f; recipe.blue_gain = 1.6f; recipe.exposure_stops = 0.5f;
    ImageGraph fixed(raw, recipe);
    for (unsigned mip : {1u, 2u}) {
        const auto reduced = average(native, mip);
        auto raster_recipe = recipe; raster_recipe.camera_color.reset(); raster_recipe.red_gain = raster_recipe.green_gain = raster_recipe.blue_gain = 1;
        raster_recipe.exposure_stops = 0;
        ImageGraph reference(RasterImage({reduced.bounds.width, reduced.bounds.height, 0, space}, reduced.rgb), raster_recipe);
        same(renderer.render_image(fixed, RenderRequest{reduced.bounds, 1, {mip, RenderQuality::Preview}}),
             renderer.render_image(reference, reduced.bounds, 1));
    }
    const auto source = std::make_shared<RawUnpackNode>(raw);
    require(!source->supports_level({1, RenderQuality::Preview}), "uncalibrated Bayer preview enabled");
    CameraToWorkingNode unbounded(source, calibration(space));
    require(!unbounded.supports_level({1, RenderQuality::Preview}), "unbounded camera preview enabled");
    auto legacy_recipe = recipe; legacy_recipe.output_mode = OutputMode::LegacyBounded;
    ImageGraph legacy(raw, legacy_recipe);
    rejects([&] { renderer.render_image(legacy, RenderRequest{{0, 0, 4, 3}, 1, {1, RenderQuality::Preview}}); });
    rejects([&] { CameraToWorkingNode invalid(source, calibration(space), {1, 0, 0, 3}); });
}
void test_cache_revisions_and_branches() {
    Renderer renderer;
    const Rect area{1, 3, 7, 5}; const auto space = WorkingSpace::LinearProPhotoD50;
    const auto raw = fixture(BayerPattern::RGGB, 1, 0, area);
    auto m = manifest(raw, space); append_output(m, space);
    auto cache = std::make_shared<TileCache>(1024 * 1024);
    const auto binding = BoundEditSource{m.sources[0], std::make_shared<RawUnpackNode>(raw), area};
    ExecutableEditGraph graph(m, {binding}, nullptr, cache);
    const RenderRequest request{{0, 0, 4, 3}, 50, {1, RenderQuality::Preview}};
    const auto original = renderer.render_image(graph, request);
    const auto before = cache->stats();
    auto changed = m; changed.operations[6].parameters["shoulder"] = EditValue{0.8};
    ExecutableEditGraph revised(changed, {binding}, nullptr, cache);
    const auto revised_tile = renderer.render_image(revised, request);
    const auto after = cache->stats();
    require(after.hits == before.hits + 1 && after.misses == before.misses + 2, "RAW late tone edit did not reuse calibrated preview");
    require(original.rgb != revised_tile.rgb, "RAW tone revision did not alter output");
    auto native_reference = average(renderer.render_image(*camera_node(raw, space), area, area), 1);
    auto raster = std::make_shared<RasterSourceNode>(RasterImage({4, 3, 0, space}, native_reference.rgb));
    auto exposure = std::make_shared<ExposureNode>(raster, -0.25f);
    auto convert = std::make_shared<WorkingSpaceConvertNode>(exposure, WorkingSpace::LinearRec2020D65);
    auto srgb = std::make_shared<WorkingToSrgbNode>(convert);
    auto tone = std::make_shared<ToneCurveNode>(srgb, 0.8f, 1.15f);
    SrgbEncodeNode encode(tone);
    same(revised_tile, renderer.render_image(encode, native_reference.bounds, native_reference.bounds));
    for (unsigned mip : {1u, 2u}) {
        auto camera_manifest = manifest(raw, space);
        ExecutableEditGraph calibrated(camera_manifest, {binding}, nullptr, cache);
        const RenderRequest one{{0, 0, 1, 1}, 1, {mip, RenderQuality::Preview}};
        const auto cold_stats = cache->stats();
        const auto cold = renderer.render_image(calibrated, one);
        require(cache->stats().misses > cold_stats.misses, "RAW mip levels shared cache entries");
        const auto warm_stats = cache->stats();
        same(renderer.render_image(calibrated, one), cold, true);
        require(cache->stats().misses == warm_stats.misses, "RAW reduced warm request missed");
        const Rect native_roi{area.x, area.y, 1, 1};
        const auto native = renderer.render_image(calibrated, native_roi);
        same(native, renderer.render_image(*camera_node(raw, space), area, native_roi), true);
        auto white_balance_edit = camera_manifest;
        white_balance_edit.operations[0].parameters["red_gain"] = EditValue{2.0};
        ExecutableEditGraph wb_revision(white_balance_edit, {binding}, nullptr, cache);
        require(renderer.render_image(wb_revision, one).rgb != cold.rgb, "RAW WB revision reused obsolete calibrated preview");
        auto calibration_edit = camera_manifest;
        auto& matrix = std::get<EditValue::Array>(calibration_edit.operations[2].parameters["matrix"].data);
        matrix[0] = EditValue{0.7};
        ExecutableEditGraph calibration_revision(calibration_edit, {binding}, nullptr, cache);
        require(renderer.render_image(calibration_revision, one).rgb != cold.rgb, "RAW calibration revision reused obsolete preview");
    }
    const auto layer_raw = fixture(BayerPattern::BGGR, 0, 1, area);
    auto mixed = manifest(raw, space);
    EditSource layer; layer.id = id(17); layer.kind = EditSourceKind::DecodedBayerU16; layer.content_sha256 = layer_raw.fingerprint();
    mixed.sources.push_back(layer);
    EditValue::Array matrix; for (double v : calibration(space).camera_to_xyz_d50) matrix.emplace_back(v);
    mixed.operations.push_back(op(20, "rawengine.camera_to_working", EditDomain::CameraLinear, domain(space), 17,
        {{"matrix", EditValue{std::move(matrix)}}}));
    auto mix = op(21, "rawengine.linear_mix", domain(space), domain(space), 4, {{"amount", EditValue{0.3}}});
    mix.inputs = {{"base", id(4)}, {"layer", id(20)}}; mixed.operations.push_back(mix); mixed.output_id = mix.id;
    ExecutableEditGraph mixed_graph(mixed, {binding, {layer, std::make_shared<RawUnpackNode>(layer_raw), area}}, nullptr, cache);
    CameraToWorkingNode layer_camera(std::make_shared<RawUnpackNode>(layer_raw), calibration(space), area);
    for (unsigned mip : {1u, 2u}) {
        const RenderLevel level{mip, RenderQuality::Preview};
        const auto base = average(renderer.render_image(*camera_node(raw, space), area, area), mip);
        const auto overlay = average(renderer.render_image(layer_camera, area, area), mip);
        auto base_source = std::make_shared<RasterSourceNode>(RasterImage({base.bounds.width, base.bounds.height, 0, space}, base.rgb));
        auto layer_source = std::make_shared<RasterSourceNode>(RasterImage({base.bounds.width, base.bounds.height, 0, space}, overlay.rgb));
        LinearMixNode reference(base_source, layer_source, 0.3f);
        same(renderer.render_image(mixed_graph, RenderRequest{base.bounds, 1, level}),
             renderer.render_image(reference, base.bounds, base.bounds));
        const auto footprints = mixed_graph.required_source_regions({0, 0, 1, 1}, level);
        require(footprints.size() == 2 && equal(footprints.at(id(1)), sensor_footprint({0, 0, 1, 1}, area, mip)) &&
                equal(footprints.at(layer.id), footprints.at(id(1))), "RAW branch source footprints differ");
    }
}
} // namespace
int main() {
    try { test_patterns_and_levels(); test_composition_and_gates(); test_cache_revisions_and_branches(); std::cout << "RAW preview reference tests passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

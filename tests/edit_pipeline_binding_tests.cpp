#include "EditGraph.hpp"
#include "SpatialOps.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace rawengine;
namespace {
constexpr Rect bounds{0, 0, 9, 7};
using Handle = std::shared_ptr<const Node>;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid pipeline binding accepted");
}
bool equal(Rect a, Rect b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
void same(const Tile& a, const Tile& b) {
    require(equal(a.bounds, b.bounds) && a.descriptor == b.descriptor &&
            a.rgb.size() == b.rgb.size(), "pipeline tile metadata differs");
    require(!std::memcmp(a.rgb.data(), b.rgb.data(), a.rgb.size() * sizeof(float)),
            "pipeline direct/saved pixels differ");
}
std::string id(unsigned value) {
    std::string result = "97000000-0000-0000-0000-000000000000";
    constexpr char hex[] = "0123456789abcdef";
    result[34] = hex[value / 16]; result[35] = hex[value % 16];
    return result;
}
EditValue integer(unsigned value) { return EditValue{std::int64_t(value)}; }
template<class C> EditValue list(const C& values) {
    EditValue::Array result;
    for (auto value : values) result.emplace_back(double(value));
    return EditValue{std::move(result)};
}
EditDomain domain(WorkingSpace space) {
    return space == WorkingSpace::LinearProPhotoD50 ? EditDomain::SceneLinearProPhotoD50
                                                  : EditDomain::SceneLinearRec2020D65;
}
EditOperation operation(unsigned index, const char* name, EditDomain in, EditDomain out,
                        const std::string& upstream, EditValue::Object parameters = {}) {
    EditOperation result;
    result.id = id(index); result.type_id = std::string("rawengine.") + name;
    result.processing_version = 2; result.input_domain = in; result.output_domain = out;
    result.inputs = {{"image", upstream}}; result.parameters = std::move(parameters);
    return result;
}
// A deterministic backend fixture tests profile binding and cache policy only.
// Real ICC color conversion is independently exercised by RawEngineLittleCmsTests.
class TestDisplayTransform final : public IccDisplayTransform {
public:
    IccProfileIdentity identity;
    TestDisplayTransform() { identity.profile_sha256.fill(0x47); identity.intent = "relative_colorimetric"; }
    std::array<std::uint8_t, 32> profile_sha256() const noexcept override { return identity.profile_sha256; }
    std::optional<IccProfileIdentity> output_icc_identity() const override { return identity; }
    void apply(float* rgb, std::size_t pixels) const override {
        for (std::size_t i = 0; i < pixels * 3; ++i)
            rgb[i] = std::clamp(rgb[i] * .75f + .125f, 0.f, 1.f);
    }
};
struct Case {
    EditManifest manifest;
    std::vector<BoundEditSource> bindings;
    Handle direct;
    Rect canvas = bounds;
    bool reduced = true;
    std::shared_ptr<const IccDisplayTransform> transform;
};
struct Counts {
    unsigned cases = 0, renders = 0, footprints = 0, cache = 0, history = 0, rejections = 0;
    std::set<std::string> types;
};

// Compose public direct-node ROI contracts independently of the saved factory.
// Geometry is the outer node in these cases; point stages preserve its input extent.
Rect direct_source_region(const Case& test, Rect roi, RenderLevel level) {
    const Node* node = test.direct.get();
    while (node->input_node()) {
        const auto input_level = node->input_level(level);
        const auto scale = 1u << input_level.mip;
        Rect input_bounds{0, 0, (bounds.width + scale - 1) / scale,
                                (bounds.height + scale - 1) / scale};
        roi = node->input_region_level(roi, input_bounds, level);
        level = input_level; node = node->input_node();
    }
    if (test.bindings.front().identity.kind == EditSourceKind::DecodedBayerU16)
        return node->input_region_level(roi, bounds, level);
    const auto scale = 1u << level.mip;
    roi.x *= scale; roi.y *= scale;
    roi.width = std::min(roi.width * scale, bounds.width - roi.x);
    roi.height = std::min(roi.height * scale, bounds.height - roi.y);
    return roi;
}
void check(const Case& test, Counts& counts) {
    const auto& target = test.manifest.operations.back();
    counts.types.insert(target.type_id); ++counts.cases;
    const auto json = serialize_edit_manifest(test.manifest);
    const auto parsed = parse_edit_manifest(json);
    require(serialize_edit_manifest(parsed) == json, "pipeline canonical replay differs");
    auto cache = std::make_shared<TileCache>(1 << 20);
    ExecutableEditGraph graph(parsed, test.bindings, test.transform, cache);
    require(equal(graph.output_bounds(), test.canvas), "pipeline output extent differs");
    const std::array<RenderLevel, 5> levels{{{}, {1, RenderQuality::Preview},
        {2, RenderQuality::Preview}, {1, RenderQuality::Final}, {3, RenderQuality::Preview}}};
    for (const auto level : levels) {
        const bool supported = level.mip == 0 || (test.reduced && level.mip <= 2 && level.quality == RenderQuality::Preview);
        require(test.direct->supports_level(level) == supported && graph.output().supports_level(level) == supported,
                "pipeline level admission differs from declared case");
        const auto scale = 1u << level.mip;
        Rect roi{0, 0, (test.canvas.width + scale - 1) / scale, (test.canvas.height + scale - 1) / scale};
        if (!supported) {
            rejects([&] { graph.output().render_level(roi, level); });
            rejects([&] { graph.required_source_regions(roi, level); });
            counts.rejections += 2; continue;
        }
        // Full, interior and true-border tiles cover rebasing and clipped halos.
        for (auto rect : {roi, Rect{roi.width - 1, roi.height - 1, 1, 1},
                          Rect{std::min(1u, roi.width - 1), 0, 1, 1}}) {
            const auto direct = test.direct->render_level(rect, level);
            const auto saved = graph.output().render_level(rect, level);
            same(direct, saved); ++counts.renders;
            const auto regions = graph.required_source_regions(rect, level);
            require(regions.size() == test.bindings.size(), "pipeline source-ID footprint count differs");
            // Mix fixtures have two point-only scene-linear sources. Both must be requested,
            // independently of their content, with the same native sample footprint.
            const auto expected = test.bindings.size() == 2
                ? Rect{rect.x * scale, rect.y * scale,
                       std::min(rect.width * scale, bounds.width - rect.x * scale),
                       std::min(rect.height * scale, bounds.height - rect.y * scale)}
                : direct_source_region(test, rect, level);
            for (const auto& binding : test.bindings) {
                require(equal(regions.at(binding.identity.id), expected), "pipeline native source support differs");
                ++counts.footprints;
            }
            const auto misses = cache->stats().misses;
            same(saved, graph.output().render_level(rect, level));
            require(cache->stats().misses == misses, "pipeline warm cache missed"); ++counts.cache;
        }
    }
    EditHistory history(parsed, test.bindings, {4, 1 << 20}, test.transform, cache);
    auto revision = parsed; revision.extra_fields.emplace("binding_audit", EditValue{true});
    const auto first = history.current()->id;
    const auto second = history.commit(revision);
    require(history.undo() == first && history.redo() == second, "pipeline history navigation differs");
    auto restored = EditHistory::restore(history.serialize(), test.bindings, test.transform, cache);
    for (const auto revision_id : {first, second}) {
        auto snapshot = restored->revision(revision_id);
        same(test.direct->render(test.canvas), snapshot->graph->output().render(test.canvas));
        ++counts.history;
    }
    for (bool enabled : {false, true}) {
        auto invalid = parsed; invalid.operations.back().enabled = enabled;
        invalid.operations.back().parameters.emplace("extra", integer(1));
        rejects([&] { ExecutableEditGraph g(invalid, test.bindings, test.transform); });
        invalid = parsed; invalid.operations.back().enabled = enabled;
        invalid.operations.back().schema_version = 2;
        rejects([&] { ExecutableEditGraph g(invalid, test.bindings, test.transform); });
        invalid = parsed; invalid.operations.back().enabled = enabled;
        invalid.operations.back().processing_version = 99;
        rejects([&] { ExecutableEditGraph g(invalid, test.bindings, test.transform); });
        counts.rejections += 3;
    }
    auto invalid = parsed;
    invalid.operations.back().output_domain = target.output_domain == EditDomain::CameraLinear
        ? EditDomain::SceneLinearSrgb : EditDomain::CameraLinear;
    rejects([&] { ExecutableEditGraph g(invalid, test.bindings, test.transform); });
    auto wrong_bindings = test.bindings; wrong_bindings.front().identity.content_sha256[0] ^= 1;
    rejects([&] { ExecutableEditGraph g(parsed, wrong_bindings, test.transform); });
    counts.rejections += 2;
    if (test.transform) {
        rejects([&] { ExecutableEditGraph g(parsed, test.bindings); });
        invalid = parsed; invalid.output_profile->profile_sha256[0] ^= 1;
        rejects([&] { ExecutableEditGraph g(invalid, test.bindings, test.transform); });
        counts.rejections += 2;
    }
    if (parsed.working_space == WorkingSpace::LinearRec2020D65 && parsed.processing_version == 2) {
        auto older = test; older.manifest.processing_version = 1;
        for (auto& op : older.manifest.operations) op.processing_version = 1;
        if (target.type_id == "rawengine.convolution") {
            for (bool enabled : {false, true}) {
                older.manifest.operations.back().enabled = enabled;
                rejects([&] { ExecutableEditGraph g(older.manifest, older.bindings, older.transform); });
                ++counts.rejections;
            }
        } else {
            // The original pipeline types admit process-1 Rec.2020 replay.
            // Run the same pixel/footprint/cache/history gates on that version.
            check(older, counts);
        }
    }
}
void verify(WorkingSpace space, Counts& counts) {
    std::vector<float> pixels(9 * 7 * 3);
    for (unsigned i = 0; i < pixels.size(); ++i) pixels[i] = float(int(i * 19 % 37) - 7) / 16;
    auto raster = std::make_shared<RasterSourceNode>(RasterImage({9, 7, 0, space}, pixels));
    EditSource source; source.id = id(1); source.working_space = space; source.content_sha256 = *raster->source_fingerprint();
    const auto working = domain(space);
    auto run = [&](const char* name, EditValue::Object parameters, Handle direct,
                   EditDomain out, Rect canvas = bounds, bool reduced = true) {
        Case test; test.manifest.working_space = space; test.manifest.sources = {source};
        test.manifest.operations = {operation(90, name, working, out, source.id, std::move(parameters))};
        test.manifest.output_id = id(90); test.bindings = {{source, raster, bounds}};
        test.direct = std::move(direct); test.canvas = canvas; test.reduced = reduced; check(test, counts);
    };
    const double value = .123456789;
    run("exposure", {{"stops", EditValue{value}}}, std::make_shared<ExposureNode>(raster, float(value)), working);
    run("box_blur", {{"radius", integer(2)}}, std::make_shared<BoxBlurNode>(raster, bounds, 2), working);
    ConvolutionKernel kernel{3, 1, {.125, .75, .125}};
    run("convolution", {{"width", integer(3)}, {"height", integer(1)}, {"coefficients", list(kernel.coefficients)},
        {"border", EditValue{std::string("replicate")}}}, std::make_shared<ConvolutionNode>(raster, bounds, kernel), working);
    run("crop", {{"x", integer(1)}, {"y", integer(1)}, {"width", integer(7)}, {"height", integer(5)}},
        std::make_shared<CropNode>(raster, bounds, Rect{1, 1, 7, 5}), working, {0, 0, 7, 5});
    run("orientation", {{"quarter_turns", integer(1)}, {"flip_horizontal", EditValue{true}}, {"flip_vertical", EditValue{false}}},
        std::make_shared<OrientationNode>(raster, bounds, 1, true, false), working, {0, 0, 7, 9});
    for (const auto filter : {ResizeFilter::Nearest, ResizeFilter::Bilinear, ResizeFilter::Area}) {
        const char* name = filter == ResizeFilter::Nearest ? "nearest" : filter == ResizeFilter::Area ? "area" : "bilinear";
        run("resize", {{"width", integer(7)}, {"height", integer(5)}, {"filter", EditValue{std::string(name)}}},
            std::make_shared<ResizeNode>(raster, bounds, 7, 5, filter), working, {0, 0, 7, 5});
    }
    const auto target = space == WorkingSpace::LinearProPhotoD50 ? WorkingSpace::LinearRec2020D65 : WorkingSpace::LinearProPhotoD50;
    run("working_space_convert", {}, std::make_shared<WorkingSpaceConvertNode>(raster, target), domain(target));
    run("working_to_srgb", {}, std::make_shared<WorkingToSrgbNode>(raster), EditDomain::SceneLinearSrgb);
    auto transform = std::make_shared<TestDisplayTransform>();
    Case output; output.manifest.working_space = space; output.manifest.sources = {source};
    output.bindings = {{source, raster, bounds}};
    output.manifest.operations = {operation(80, "working_to_srgb", working, EditDomain::SceneLinearSrgb, source.id)};
    auto linear = std::make_shared<WorkingToSrgbNode>(raster);
    auto tone = std::make_shared<ToneCurveNode>(linear, float(value), float(1.23456789));
    output.manifest.operations.push_back(operation(90, "tone_curve", EditDomain::SceneLinearSrgb,
        EditDomain::DisplayLinearSrgb, id(80), {{"shoulder", EditValue{value}}, {"gamma", EditValue{1.23456789}}}));
    output.manifest.output_id = id(90); output.direct = tone; check(output, counts);
    output.manifest.operations.push_back(operation(91, "srgb_encode", EditDomain::DisplayLinearSrgb, EditDomain::DisplayEncodedSrgb, id(90)));
    output.manifest.output_id = id(91); output.direct = std::make_shared<SrgbEncodeNode>(tone); check(output, counts);
    output.manifest.operations.back() = operation(91, "icc_display", EditDomain::DisplayLinearSrgb, EditDomain::DisplayEncodedIcc, id(90));
    output.manifest.output_profile = transform->identity; output.transform = transform;
    output.direct = std::make_shared<IccDisplayNode>(tone, transform); output.reduced = false; check(output, counts);
    auto unmanaged = std::make_shared<ToneCurveNode>(raster, float(value), float(1.23456789));
    run("tone_curve", {{"shoulder", EditValue{value}}, {"gamma", EditValue{1.23456789}}}, unmanaged,
        EditDomain::ToneMappedUnmanaged, bounds, false);
    Case clip; clip.manifest.working_space = space; clip.manifest.sources = {source}; clip.bindings = {{source, raster, bounds}};
    clip.manifest.operations = {operation(80, "tone_curve", working, EditDomain::ToneMappedUnmanaged, source.id,
        {{"shoulder", EditValue{value}}, {"gamma", EditValue{1.23456789}}}),
        operation(90, "output_clip", EditDomain::ToneMappedUnmanaged, EditDomain::UnmanagedBounded, id(80))};
    clip.manifest.output_id = id(90); clip.direct = std::make_shared<OutputClipNode>(unmanaged); clip.reduced = false; check(clip, counts);
    auto layer_pixels = pixels; for (auto& pixel : layer_pixels) pixel = pixel * .5f + .25f;
    auto layer = std::make_shared<RasterSourceNode>(RasterImage({9, 7, 0, space}, layer_pixels));
    auto layer_source = source; layer_source.id = id(2); layer_source.content_sha256 = *layer->source_fingerprint();
    Case mix; mix.manifest.working_space = space; mix.manifest.sources = {source, layer_source};
    mix.bindings = {{source, raster, bounds}, {layer_source, layer, bounds}};
    auto mix_op = operation(90, "linear_mix", working, working, source.id, {{"amount", EditValue{value}}});
    mix_op.inputs = {{"base", source.id}, {"layer", layer_source.id}}; mix.manifest.operations = {mix_op};
    mix.manifest.output_id = id(90); mix.direct = std::make_shared<LinearMixNode>(raster, layer, float(value)); check(mix, counts);
    RawMetadata metadata; metadata.width = 9; metadata.height = 7; metadata.active_area = bounds;
    metadata.black_levels = {64, 96, 32, 80}; metadata.white_levels = {4095, 4095, 4095, 4095};
    std::vector<std::uint16_t> samples(9 * 7);
    for (unsigned i = 0; i < samples.size(); ++i) samples[i] = std::uint16_t(i * 197 % 4096);
    auto raw = std::make_shared<RawUnpackNode>(RawImage(metadata, samples));
    EditSource raw_source; raw_source.id = id(1); raw_source.kind = EditSourceKind::DecodedBayerU16;
    raw_source.content_sha256 = *raw->source_fingerprint();
    Case camera; camera.manifest.working_space = space; camera.manifest.sources = {raw_source}; camera.bindings = {{raw_source, raw, bounds}};
    auto white = std::make_shared<WhiteBalanceNode>(raw, float(1.23456789), float(.987654321), float(1.345678912));
    camera.manifest.operations = {operation(80, "white_balance", EditDomain::CameraLinear, EditDomain::CameraLinear, raw_source.id,
        {{"red_gain", EditValue{1.23456789}}, {"green_gain", EditValue{.987654321}}, {"blue_gain", EditValue{1.345678912}}})};
    camera.manifest.output_id = id(80); camera.direct = white; camera.reduced = false; check(camera, counts);
    auto exposure = std::make_shared<ExposureNode>(white, float(value));
    camera.manifest.operations.push_back(operation(81, "exposure", EditDomain::CameraLinear, EditDomain::CameraLinear, id(80), {{"stops", EditValue{value}}}));
    camera.manifest.output_id = id(81); camera.direct = exposure; check(camera, counts);
    CameraColorTransform calibration{{.55, .22, .13, .17, .68, .09, .03, .11, .73}, space};
    camera.manifest.operations.push_back(operation(90, "camera_to_working", EditDomain::CameraLinear, working, id(81), {{"matrix", list(calibration.camera_to_xyz_d50)}}));
    camera.manifest.output_id = id(90); camera.direct = std::make_shared<CameraToWorkingNode>(exposure, calibration, bounds);
    camera.reduced = true; check(camera, counts);
    GraphRecipe recipe; recipe.exposure_stops = float(value); recipe.tone_shoulder = float(.456789123); recipe.tone_gamma = float(1.23456789);
    Case legacy; legacy.bindings = {{source, raster, bounds}}; legacy.reduced = false;
    legacy.manifest = snapshot_legacy_recipe(source, recipe, space == WorkingSpace::LinearProPhotoD50
        ? LegacyRecipeEra::ImplicitProPhoto : LegacyRecipeEra::ImplicitRec2020, id(90));
    legacy.direct = std::make_shared<OutputClipNode>(std::make_shared<ToneCurveNode>(
        std::make_shared<ExposureNode>(raster, recipe.exposure_stops), recipe.tone_shoulder, recipe.tone_gamma));
    check(legacy, counts);
}
} // namespace
void verify_pipeline_bindings() {
    Counts counts; verify(WorkingSpace::LinearProPhotoD50, counts); verify(WorkingSpace::LinearRec2020D65, counts);
    const std::set<std::string> expected{"rawengine.box_blur", "rawengine.camera_to_working",
        "rawengine.convolution", "rawengine.crop", "rawengine.exposure", "rawengine.icc_display",
        "rawengine.legacy.fixed_chain", "rawengine.linear_mix", "rawengine.orientation",
        "rawengine.output_clip", "rawengine.resize", "rawengine.srgb_encode", "rawengine.tone_curve",
        "rawengine.white_balance", "rawengine.working_space_convert", "rawengine.working_to_srgb"};
    require(counts.types == expected, "pipeline type coverage differs");
    std::cout << "Verified " << counts.types.size() << " pipeline types; " << counts.cases << " cases, "
              << counts.renders << " exact renders, " << counts.footprints << " source footprints, "
              << counts.cache << " warm-cache checks, " << counts.history << " history replays, "
              << counts.rejections << " rejected bindings/levels\n";
}

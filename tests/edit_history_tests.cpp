#include "EditGraph.hpp"
#include "TileScheduler.hpp"
#include <algorithm>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace rawengine;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F function, const char* message) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

struct Fixture {
    EditManifest base;
    std::vector<BoundEditSource> bindings;
    std::vector<float> pixels;
    Fixture() {
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 7; ++x) {
                pixels.push_back(x / 4.0f - 0.5f);
                pixels.push_back(-y / 4.0f);
                pixels.push_back((x + y) / 8.0f);
            }
        RasterImage image({7, 5, 0, WorkingSpace::LinearProPhotoD50}, pixels);
        EditSource source;
        source.id = "40000000-0000-0000-0000-000000000001";
        source.working_space = WorkingSpace::LinearProPhotoD50;
        source.content_sha256 = fingerprint_raster_source(image);
        base.sources.push_back(source);
        base.output_id = source.id;
        bindings.push_back({source, std::make_shared<RasterSourceNode>(std::move(image)), {0, 0, 7, 5}});
    }
    EditManifest exposure(double stops) const {
        auto manifest = base;
        EditOperation operation;
        operation.id = "40000000-0000-0000-0000-000000000002";
        operation.type_id = "rawengine.exposure";
        operation.processing_version = 2;
        operation.inputs.emplace("image", base.output_id);
        operation.parameters.emplace("stops", EditValue{stops});
        manifest.operations.push_back(operation);
        manifest.output_id = operation.id;
        return manifest;
    }
};

Tile render(EditHistory::Snapshot revision, std::uint32_t mip = 0) {
    const auto bounds = revision->graph->output_bounds();
    const auto scale = 1u << mip;
    RenderRequest request;
    request.viewport = {0, 0, bounds.width / scale + (bounds.width % scale != 0),
                             bounds.height / scale + (bounds.height % scale != 0)};
    request.tile_size = 2;
    request.level = {mip, mip ? RenderQuality::Preview : RenderQuality::Final};
    return Renderer{}.render_image(*revision->graph, request);
}

void navigation_and_geometry() {
    Fixture fixture;
    auto cache = std::make_shared<TileCache>(1024 * 1024);
    EditHistory history(fixture.base, fixture.bindings, {3, 1024 * 1024}, nullptr, cache);
    auto original = history.current();
    require(original->id == 1 && render(original).rgb == fixture.pixels, "initial snapshot differs from owned pixels");
    require(history.commit(fixture.exposure(1)) == 2, "second revision ID differs");
    auto doubled = fixture.pixels;
    for (auto& value : doubled) value *= 2;
    require(render(history.current()).rgb == doubled, "exposure revision failed numeric reference");
    auto cropped = fixture.exposure(1);
    EditOperation crop;
    crop.id = "40000000-0000-0000-0000-000000000003";
    crop.type_id = "rawengine.crop";
    crop.processing_version = 2;
    crop.inputs.emplace("image", cropped.output_id);
    crop.parameters = {{"x", EditValue{std::int64_t{1}}}, {"y", EditValue{std::int64_t{1}}},
                       {"width", EditValue{std::int64_t{5}}}, {"height", EditValue{std::int64_t{3}}}};
    cropped.output_id = crop.id;
    cropped.operations.push_back(crop);
    require(history.commit(cropped) == 3, "crop commit ID differs");
    require(render(history.current()).bounds.width == 5 && render(history.current(), 1).bounds.height == 2,
            "geometry revision has incorrect native/reduced bounds");
    auto pair = history.comparison(1, 3);
    require(pair.first->graph->output_bounds().width == 7 && pair.second->graph->output_bounds().width == 5,
            "comparison collapsed independent geometry");
    require(history.undo() == 2 && history.redo() == 3 && history.undo() == 2, "undo/redo selected wrong revision");
    const auto saved = history.serialize();
    auto restored = EditHistory::restore(saved, fixture.bindings, nullptr, cache);
    require(restored->serialize() == saved && restored->current()->id == 2 && restored->redo() == 3,
            "history save/replay lost cursor, states or canonical bytes");
    require(history.commit(fixture.exposure(-1)) == 4, "branch commit reused a discarded ID");
    rejects([&] { history.revision(3); }, "discarded redo revision remains addressable");
    rejects([&] { history.redo(); }, "redo survived new branch");
    require(render(pair.second).bounds.width == 5, "retained snapshot changed after redo truncation");
    history.commit(fixture.exposure(0.5));
    require(history.stats().revision_ids == std::vector<std::uint64_t>({2, 4, 5}), "count eviction retained wrong IDs");
    rejects([&] { history.revision(1); }, "evicted revision is addressable");
    require(render(original).rgb == fixture.pixels, "eviction invalidated external snapshot");
    auto replay = EditHistory::restore(history.serialize(), fixture.bindings);
    require(replay->commit(fixture.base) == 6, "restore reused evicted/discarded IDs");
}

void failures_and_bytes() {
    Fixture fixture;
    EditHistory history(fixture.base, fixture.bindings, {8, 1024 * 1024});
    history.commit(fixture.exposure(1));
    history.undo();
    const auto saved = history.serialize();
    auto bad = fixture.exposure(1);
    bad.operations.front().type_id = "example.unknown";
    rejects([&] { history.commit(bad); }, "unknown operation committed");
    bad = fixture.base;
    bad.sources.front().content_sha256[0] ^= 1;
    rejects([&] { history.commit(bad); }, "history changed source identity");
    rejects([&] { EditHistory::restore(saved, {{bad.sources.front(), fixture.bindings.front().node, {0, 0, 7, 5}}}); },
            "restored history rebound changed source");
    require(history.serialize() == saved && history.redo() == 2, "failed commit changed cursor/redo/IDs");
    const auto bytes = serialize_edit_manifest(fixture.base).size();
    EditHistory bounded(fixture.base, fixture.bindings, {100, bytes * 2});
    auto retained = bounded.current();
    bounded.commit(fixture.base); bounded.commit(fixture.base);
    require(bounded.stats().manifest_bytes == bytes * 2 && bounded.stats().revision_ids == std::vector<std::uint64_t>({2, 3}),
            "manifest byte budget failed to evict oldest state");
    require(render(retained).rgb == fixture.pixels, "byte eviction invalidated snapshot");
    EditHistory tight(fixture.base, fixture.bindings, {8, bytes});
    rejects([&] { tight.commit(fixture.exposure(1)); }, "oversized revision committed");
    require(tight.commit(fixture.base) == 2, "failed budget commit consumed an ID");
    rejects([&] { EditHistory invalid(fixture.base, fixture.bindings, {0, bytes}); }, "zero revision budget accepted");
    rejects([&] { EditHistory invalid(fixture.base, fixture.bindings, {2, bytes - 1}); }, "undersized initial budget accepted");
    auto invalid = saved;
    auto offset = invalid.find("\"history_format_version\":1");
    invalid.replace(offset, std::string("\"history_format_version\":1").size(), "\"history_format_version\":2");
    rejects([&] { EditHistory::restore(invalid, fixture.bindings); }, "future history format accepted");
}

void concurrency_and_lifetimes() {
    Fixture fixture;
    auto history = std::make_unique<EditHistory>(fixture.base, fixture.bindings, EditHistory::Limits{64, 1024 * 1024});
    auto original = history->current();
    std::vector<std::future<std::uint64_t>> commits;
    for (int i = 0; i < 12; ++i)
        commits.push_back(std::async(std::launch::async, [&, i] { return history->commit(fixture.exposure(i / 4.0)); }));
    std::vector<std::uint64_t> ids;
    for (auto& future : commits) ids.push_back(future.get());
    std::sort(ids.begin(), ids.end());
    for (std::size_t i = 0; i < ids.size(); ++i) require(ids[i] == i + 2, "concurrent commits reused/lost IDs");
    require(history->stats().revision_ids.size() == 13, "concurrent history lost states");
    auto comparison = history->comparison(1, 13);
    const auto expected = render(comparison.second);
    TileScheduler scheduler(1, 4);
    auto job = scheduler.submit(comparison.second->graph->output_handle(), {0, 0, 7, 5}, {0, 0, 7, 5}, RenderPriority::Normal, 2);
    history.reset();
    fixture.bindings.clear();
    require(job.get().rgb == expected.rgb && render(original).rgb == fixture.pixels,
            "history/source destruction invalidated scheduled/external snapshots");
}
} // namespace

int main() {
    try {
        navigation_and_geometry(); failures_and_bytes(); concurrency_and_lifetimes();
        std::cout << "Edit history tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

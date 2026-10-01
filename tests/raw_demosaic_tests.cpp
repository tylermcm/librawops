#include "EditGraph.hpp"
#include <iostream>
#include <stdexcept>
using namespace rawengine;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("unsupported demosaic policy accepted");
}
// Correct pixels, fingerprint, bounds and color alone cannot certify reconstruction policy.
class UnidentifiedRaw final : public Node {
    RawUnpackNode source_;
public:
    explicit UnidentifiedRaw(RawImage image) : source_(std::move(image)) {}
    Tile render(Rect r) const override { return source_.render(r); }
    ImageDescriptor output_descriptor() const noexcept override { return source_.output_descriptor(); }
    std::optional<Rect> source_bounds() const override { return source_.source_bounds(); }
    std::optional<std::array<std::uint8_t, 32>> source_fingerprint() const override { return source_.source_fingerprint(); }
};
void policy_replay() {
    RawMetadata m; m.width = 7; m.height = 5; m.black_levels.fill(4000); m.white_levels.fill(19000);
    std::vector<std::uint16_t> samples(35);
    for (unsigned i = 0; i < 35; ++i) samples[i] = static_cast<std::uint16_t>(2000 + i * 777);
    RawImage raw(m, samples);
    auto node = std::make_shared<RawUnpackNode>(raw);
    EditSource source; source.id = "00000000-0000-0000-0000-000000000001";
    source.kind = EditSourceKind::DecodedBayerU16; source.content_sha256 = raw.fingerprint();
    EditManifest legacy; legacy.sources.push_back(source); legacy.output_id = source.id;
    auto explicit_manifest = legacy; explicit_manifest.format_version = 3;
    explicit_manifest.sources[0].demosaic = RawDemosaicIdentity{};
    auto explicit_source = explicit_manifest.sources[0];
    auto cache = std::make_shared<TileCache>(1024 * 1024);
    const Rect bounds{0, 0, 7, 5};
    ExecutableEditGraph old(legacy, {{explicit_source, node, bounds}}, nullptr, cache);
    auto expected = Renderer{}.render_image(old, bounds, 2);
    auto stats = cache->stats();
    ExecutableEditGraph current(explicit_manifest, {{source, node, bounds}}, nullptr, cache);
    require(Renderer{}.render_image(current, bounds, 2).rgb == expected.rgb, "explicit policy changed pixels");
    require(cache->stats().hits > stats.hits && cache->stats().misses == stats.misses,
            "implicit/explicit policy did not share canonical cache");
    require(parse_edit_manifest(serialize_edit_manifest(explicit_manifest)) == explicit_manifest,
            "format-3 policy did not roundtrip");
    auto v1 = serialize_edit_manifest(legacy);
    v1.replace(v1.find("\"format_version\":2"), 18, "\"format_version\":1");
    ExecutableEditGraph oldest(parse_edit_manifest(v1), {{explicit_source, node, bounds}});
    require(Renderer{}.render_image(oldest, bounds, 7).rgb == expected.rgb, "format-1 RAW replay changed");
    EditHistory history(legacy, {{explicit_source, node, bounds}}, {8, 1024 * 1024}, nullptr, cache);
    const auto revision = history.commit(explicit_manifest);
    require(history.undo() && history.redo(), "mixed-format history navigation failed");
    auto restored = EditHistory::restore(history.serialize(), {{source, node, bounds}}, nullptr, cache);
    require(restored->current()->id == revision &&
            Renderer{}.render_image(*restored->current()->graph, bounds, 7).rgb == expected.rgb,
            "mixed-format history restore changed pixels");
    for (auto policy : {RawDemosaicIdentity{"rawengine.future", 1}, RawDemosaicIdentity{"rawengine.bilinear", 2},
                        RawDemosaicIdentity{"rawengine.bilinear", 0}}) {
        rejects([&] { RawUnpackNode bad(raw, policy); });
        auto bad = explicit_manifest; bad.sources[0].demosaic = policy;
        rejects([&] { validate_edit_manifest(bad); });
        rejects([&] { history.commit(bad); });
    }
    auto bad = explicit_manifest; bad.sources[0].demosaic.reset();
    rejects([&] { validate_edit_manifest(bad); });
    bad = explicit_manifest; bad.format_version = 2;
    rejects([&] { validate_edit_manifest(bad); });
    bad = explicit_manifest; bad.sources[0].kind = EditSourceKind::SceneLinearRasterF32;
    bad.sources[0].working_space = WorkingSpace::LinearProPhotoD50;
    rejects([&] { validate_edit_manifest(bad); });
    rejects([&] { ExecutableEditGraph bad_graph(legacy, {{source, std::make_shared<UnidentifiedRaw>(raw), bounds}}); });
    require(history.current()->id == revision, "invalid policy changed history");
}
}
int main() {
    try { policy_replay(); std::cout << "RAW demosaic identity/replay passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

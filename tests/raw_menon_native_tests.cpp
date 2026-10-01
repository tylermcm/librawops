#include "EditGraph.hpp"
#include <algorithm>
#include <bit>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace rawengine;
namespace {
const RawDemosaicIdentity policy{"rawengine.menon_base", 1};
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::uint32_t word(std::istream& input, unsigned bytes = 4) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) {
        const auto c = input.get();
        require(c != std::char_traits<char>::eof(), "truncated Menon reference");
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << (i * 8);
    }
    return value;
}
void same(const Tile& actual, const Tile& expected) {
    require(actual.descriptor == ImageDescriptor::camera_linear(), "Menon output domain changed");
    require(actual.bounds.x == expected.bounds.x && actual.bounds.y == expected.bounds.y &&
            actual.bounds.width == expected.bounds.width && actual.bounds.height == expected.bounds.height &&
            actual.rgb.size() == expected.rgb.size(), "Menon output shape changed");
    for (std::size_t i = 0; i < actual.rgb.size(); ++i)
        if (std::bit_cast<std::uint32_t>(actual.rgb[i]) != std::bit_cast<std::uint32_t>(expected.rgb[i]))
            throw std::runtime_error("Menon upstream parity differs at channel " + std::to_string(i));
}
Tile crop(const Tile& full, Rect r) {
    Tile out{r, {}};
    for (unsigned y = r.y; y < r.y + r.height; ++y)
        for (unsigned x = r.x; x < r.x + r.width; ++x) {
            const auto i = (static_cast<std::size_t>(y - full.bounds.y) * full.bounds.width + x - full.bounds.x) * 3;
            out.rgb.insert(out.rgb.end(), full.rgb.begin() + i, full.rgb.begin() + i + 3);
        }
    return out;
}
void references() {
    std::ifstream input(RAWENGINE_MENON_REFERENCE, std::ios::binary);
    require(bool(input), "cannot read frozen Menon reference");
    char magic[8]; input.read(magic, 8);
    require(std::string(magic, 8) == "LRMENON1", "wrong Menon reference format");
    const auto count = word(input);
    require(count == 93, "wrong Menon fixture count");
    std::size_t channels = 0;
    for (unsigned fixture = 0; fixture < count; ++fixture) {
        RawMetadata m;
        m.width = word(input); m.height = word(input); m.row_stride_samples = word(input);
        m.pattern = static_cast<BayerPattern>(word(input));
        m.cfa_phase_x = static_cast<std::uint8_t>(word(input)); m.cfa_phase_y = static_cast<std::uint8_t>(word(input));
        m.active_area = {word(input), word(input), word(input), word(input)};
        for (auto& v : m.black_levels) v = static_cast<std::uint16_t>(word(input, 2));
        for (auto& v : m.white_levels) v = static_cast<std::uint16_t>(word(input, 2));
        const auto size = word(input);
        require(size == std::uint64_t(m.row_stride_samples) * m.height && size < 100000, "invalid reference size");
        std::vector<std::uint16_t> samples(size);
        for (auto& v : samples) v = static_cast<std::uint16_t>(word(input, 2));
        Tile expected{m.active_area, std::vector<float>(static_cast<std::size_t>(m.active_area.width) * m.active_area.height * 3)};
        for (auto& v : expected.rgb) v = std::bit_cast<float>(word(input));
        const RawImage image(m, samples);
        const auto area = image.metadata().active_area;
        RawUnpackNode node(image, policy);
        try {
            same(node.render(area), expected); // Includes internal 256-pixel boundaries.
            require(node.render({area.x, area.y, 0, 0}).rgb.empty(), "empty Menon request allocated pixels");
            same(Renderer{}.render_image(node, area, area, fixture == 92 ? 37 : 3), expected);
            const Rect roi{area.x + area.width / 3, area.y + area.height / 3,
                           std::max(1u, area.width / 3), std::max(1u, area.height / 3)};
            same(node.render(roi), crop(expected, roi));
            same(Renderer{}.render_image(node, area, roi, 2), crop(expected, roi));
            // Padding and inactive photosites cannot affect reconstruction.
            for (unsigned y = 0; y < m.height; ++y)
                for (unsigned x = 0; x < m.row_stride_samples; ++x)
                    if (x < area.x || x >= area.x + area.width || y < area.y || y >= area.y + area.height)
                        samples[static_cast<std::size_t>(y) * m.row_stride_samples + x] = 0;
            same(RawUnpackNode(RawImage(m, samples), policy).render(area), expected);
            if (fixture == 92) {
                const auto footprint = node.input_region({area.x + 20, area.y + 30, 3, 4}, area);
                require(footprint.x == area.x + 14 && footprint.y == area.y + 24 && footprint.width == 15 && footprint.height == 16,
                        "Menon source halo differs");
                const auto edge = node.input_region({area.x, area.y, 2, 2}, area);
                require(edge.x == area.x && edge.y == area.y && edge.width == 8 && edge.height == 8, "true-border halo differs");
                auto job = std::async(std::launch::async, [&] { return node.render(roi); });
                same(node.render(roi), job.get());
            }
            if (area.width == 1 || area.height == 1)
                same(node.render(area), RawUnpackNode(image).render(area));
        } catch (const std::exception& e) {
            throw std::runtime_error("fixture " + std::to_string(fixture) + ": " + e.what());
        }
        channels += expected.rgb.size();
    }
    require(input.peek() == std::char_traits<char>::eof(), "extra Menon reference bytes");
    std::cout << count << " upstream/fallback fixtures, " << channels << " channels, full/tile/ROI/padding parity passed\n";
}
void identity() {
    RawMetadata m; m.width = 13; m.height = 11; m.black_levels.fill(4000); m.white_levels.fill(19000);
    std::vector<std::uint16_t> samples(143);
    for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<std::uint16_t>((i * 7919) % 41000);
    const RawImage image(m, samples);
    const Rect area{0, 0, 13, 11};
    auto menon = std::make_shared<RawUnpackNode>(image, policy);
    auto bilinear = std::make_shared<RawUnpackNode>(image);
    EditSource source; source.id = "00000000-0000-0000-0000-000000000001";
    source.kind = EditSourceKind::DecodedBayerU16; source.content_sha256 = image.fingerprint(); source.demosaic = policy;
    EditManifest manifest; manifest.format_version = 3; manifest.sources.push_back(source); manifest.output_id = source.id;
    auto cache = std::make_shared<TileCache>(1024 * 1024);
    ExecutableEditGraph graph(manifest, {{source, menon, area}}, nullptr, cache);
    auto expected = Renderer{}.render_image(graph, area, 3);
    same(expected, menon->render(area));
    require(parse_edit_manifest(serialize_edit_manifest(manifest)) == manifest, "Menon manifest did not roundtrip");
    EditHistory history(manifest, {{source, menon, area}}, {8, 1024 * 1024}, nullptr, cache);
    auto restored = EditHistory::restore(history.serialize(), {{source, menon, area}}, nullptr, cache);
    same(Renderer{}.render_image(*restored->current()->graph, area, 3), expected);
    const auto misses = cache->stats().misses;
    auto old = manifest; old.sources[0].demosaic = RawDemosaicIdentity{};
    ExecutableEditGraph other(old, {{old.sources[0], bilinear, area}}, nullptr, cache);
    require(Renderer{}.render_image(other, area, 3).rgb != expected.rgb && cache->stats().misses > misses,
            "different algorithms shared cached reconstruction");
    try { ExecutableEditGraph bad(manifest, {{source, bilinear, area}}); throw std::runtime_error("policy mismatch accepted"); }
    catch (const std::invalid_argument&) {}
    for (const auto& invalid : {RawDemosaicIdentity{"rawengine.menon_base", 0}, RawDemosaicIdentity{"rawengine.menon_base", 2},
                                RawDemosaicIdentity{"rawengine.menon_refined", 1}}) {
        try { RawUnpackNode bad(image, invalid); throw std::runtime_error("unknown Menon policy accepted"); }
        catch (const std::invalid_argument&) {}
    }
    std::cout << "Menon identity/cache/replay/history isolation passed\n";
}
}
int main() {
    try { references(); identity(); return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

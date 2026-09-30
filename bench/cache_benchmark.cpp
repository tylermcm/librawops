#include "EditGraph.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace rawengine;

namespace {
using Clock = std::chrono::steady_clock;

double milliseconds(Clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}

EditOperation operation(const char* id, const char* type, const char* input,
                        EditDomain input_domain, EditDomain output_domain,
                        EditValue::Object parameters = {}) {
    EditOperation result;
    result.id = id; result.type_id = type;
    result.processing_version = kCurrentEditProcessingVersion;
    result.input_domain = input_domain; result.output_domain = output_domain;
    result.inputs.emplace("image", input);
    result.parameters = std::move(parameters);
    return result;
}

double stream(const ExecutableEditGraph& graph) {
    double checksum = 0;
    Renderer().render_tiles(graph, {0, 0, 1024, 768},
        [&](const Tile& tile) {
            for (std::size_t i = 0; i < tile.rgb.size(); i += 3)
                checksum += tile.rgb[i];
        }, 256);
    return checksum;
}
} // namespace

int main() {
    try {
        constexpr std::uint32_t width = 2048, height = 1536;
        std::vector<float> pixels(static_cast<std::size_t>(width) * height * 3);
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto i = (static_cast<std::size_t>(y) * width + x) * 3;
                pixels[i] = static_cast<float>(x % 1021) / 1021.0f;
                pixels[i + 1] = static_cast<float>(y % 761) / 761.0f;
                pixels[i + 2] = static_cast<float>((x + y) % 997) / 997.0f;
            }
        RasterImage image({width, height, 0, WorkingSpace::LinearProPhotoD50},
                          std::move(pixels));
        const auto fingerprint_start = Clock::now();
        EditSource source;
        source.id = "00000000-0000-0000-0000-000000000001";
        source.kind = EditSourceKind::SceneLinearRasterF32;
        source.working_space = WorkingSpace::LinearProPhotoD50;
        source.content_sha256 = fingerprint_raster_source(image);
        const auto fingerprint_ms = milliseconds(fingerprint_start);
        EditManifest manifest;
        manifest.sources.push_back(source);
        manifest.operations.push_back(operation("00000000-0000-0000-0000-000000000002",
            "rawengine.exposure", source.id.c_str(),
            EditDomain::SceneLinearProPhotoD50, EditDomain::SceneLinearProPhotoD50,
            {{"stops", EditValue{0.5}}}));
        manifest.operations.push_back(operation("00000000-0000-0000-0000-000000000003",
            "rawengine.tone_curve", manifest.operations[0].id.c_str(),
            EditDomain::SceneLinearProPhotoD50, EditDomain::ToneMappedUnmanaged,
            {{"shoulder", EditValue{0.25}}, {"gamma", EditValue{1.0}}}));
        manifest.operations.push_back(operation("00000000-0000-0000-0000-000000000004",
            "rawengine.output_clip", manifest.operations[1].id.c_str(),
            EditDomain::ToneMappedUnmanaged, EditDomain::UnmanagedBounded));
        manifest.output_id = manifest.operations[2].id;
        auto node = std::make_shared<RasterSourceNode>(image);
        const BoundEditSource binding{source, node, {0, 0, width, height}};
        auto cache = std::make_shared<TileCache>(48 * 1024 * 1024);
        ExecutableEditGraph graph(manifest, {binding}, nullptr, cache);
        const auto cold_start = Clock::now();
        const double cold_checksum = stream(graph);
        const double cold_ms = milliseconds(cold_start);
        std::vector<double> warm_times;
        for (int i = 0; i < 5; ++i) {
            const auto start = Clock::now();
            if (stream(graph) != cold_checksum)
                throw std::runtime_error("warm cache changed output");
            warm_times.push_back(milliseconds(start));
        }
        std::sort(warm_times.begin(), warm_times.end());
        const auto warm_stats = cache->stats();
        manifest.operations[1].parameters["shoulder"] = EditValue{0.45};
        const auto rebuild_start = Clock::now();
        ExecutableEditGraph revised(manifest, {binding}, nullptr, cache);
        const double rebuild_ms = milliseconds(rebuild_start);
        const auto late_start = Clock::now();
        const double late_checksum = stream(revised);
        const double late_ms = milliseconds(late_start);
        const auto late_stats = cache->stats();
        if (late_checksum == cold_checksum || late_stats.hits <= warm_stats.hits)
            throw std::runtime_error("late edit did not change output and reuse upstream tiles");
        std::cout << std::fixed << std::setprecision(3)
                  << "synthetic scene-linear raster " << width << 'x' << height
                  << ", ROI 1024x768, tile 256, cache 48 MiB\n"
                  << "initial source fingerprint: " << fingerprint_ms << " ms\n"
                  << "cold render: " << cold_ms << " ms\n"
                  << "warm render median of 5: " << warm_times[2] << " ms\n"
                  << "late edit graph construction: " << rebuild_ms << " ms\n"
                  << "late edit first render: " << late_ms << " ms\n"
                  << "cache: " << late_stats.entries << " entries, "
                  << late_stats.used_bytes << " charged bytes, "
                  << late_stats.hits << " hits, " << late_stats.misses << " misses\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

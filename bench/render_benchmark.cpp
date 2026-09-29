#include "RawEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace rawengine;

namespace {

std::uint32_t number(const char* text) {
    const std::string value(text);
    std::size_t end = 0;
    const auto parsed = std::stoull(value, &end);
    if (end != value.size() || parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("expected a positive uint32 argument");
    return static_cast<std::uint32_t>(parsed);
}

template <class F>
double median_ms(F&& operation, std::uint32_t repetitions) {
    std::vector<double> samples;
    samples.reserve(repetitions);
    for (std::uint32_t run = 0; run < repetitions; ++run) {
        const auto start = std::chrono::steady_clock::now();
        operation();
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 6) throw std::invalid_argument(
            "usage: render_benchmark [width height repetitions tile_size mode]");
        const auto width = argc > 1 ? number(argv[1]) : 7500;
        const auto height = argc > 2 ? number(argv[2]) : 6000;
        const auto repetitions = argc > 3 ? number(argv[3]) : 3;
        const auto tile_size = argc > 4 ? number(argv[4]) : 256;
        const std::string_view mode = argc > 5 ? argv[5] : "legacy";
        if (mode != "legacy" && mode != "srgb-preview")
            throw std::invalid_argument("mode must be legacy or srgb-preview");
        const auto pixel_count = static_cast<std::uint64_t>(width) * height;
        if (pixel_count > std::vector<std::uint16_t>().max_size())
            throw std::length_error("image is too large");
        std::vector<std::uint16_t> samples(static_cast<std::size_t>(pixel_count));
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x)
                samples[static_cast<std::size_t>(y) * width + x] =
                    static_cast<std::uint16_t>(500 + ((x * 17u + y * 13u) % 50000u));

        GraphRecipe recipe;
        if (mode == "srgb-preview") {
            const double d50_x = 0.3457 / 0.3585;
            const double d50_z = (1.0 - 0.3457 - 0.3585) / 0.3585;
            recipe.camera_color = CameraColorTransform{{d50_x, 0, 0,
                                                         0, 1, 0,
                                                         0, 0, d50_z},
                                                        WorkingSpace::LinearRec2020D65};
            recipe.output_mode = OutputMode::SrgbPreview;
        }
        ImageGraph graph(RawImage(width, height, std::move(samples)), recipe);
        Renderer renderer;
        const Rect roi{0, 0, std::min(width, 1024u), std::min(height, 768u)};
        double checksum = 0.0;
        const auto roi_ms = median_ms([&] {
            auto output = renderer.render_roi(graph, roi, tile_size);
            checksum += output[output.size() / 2];
        }, repetitions);
        const auto stream_ms = median_ms([&] {
            renderer.render_tiles(graph, {0, 0, width, height}, [&](const Tile& tile) {
                checksum += tile.rgb[tile.rgb.size() / 2];
            }, tile_size);
        }, repetitions);

        std::cout << std::fixed << std::setprecision(3)
                  << "{\"width\":" << width << ",\"height\":" << height
                  << ",\"mode\":\"" << mode << "\""
                  << ",\"repetitions\":" << repetitions << ",\"tile_size\":" << tile_size
                  << ",\"roi_width\":" << roi.width << ",\"roi_height\":" << roi.height
                  << ",\"roi_median_ms\":" << roi_ms
                  << ",\"full_stream_median_ms\":" << stream_ms
                  << ",\"checksum\":" << checksum << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

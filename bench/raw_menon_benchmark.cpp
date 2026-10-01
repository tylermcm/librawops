#include "RawEngine.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        auto number = [&](int index, unsigned fallback) {
            if (argc <= index) return fallback;
            const std::string text(argv[index]);
            if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
                throw std::invalid_argument("positive integer arguments required");
            const auto value = std::stoull(text);
            if (!value || value > 100000) throw std::invalid_argument("benchmark argument outside bounds");
            return static_cast<unsigned>(value);
        };
        const auto width = number(1, 1000), height = number(2, 1000), repeats = number(3, 3), tile_size = number(4, 256);
        if (std::uint64_t(width) * height > 60000000 || repeats > 10)
            throw std::invalid_argument("benchmark budget exceeded");
        std::vector<std::uint16_t> samples(static_cast<std::size_t>(width) * height);
        for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<std::uint16_t>((i * 7919 + i / width * 677) % 41000);
        rawengine::RawUnpackNode source(rawengine::RawImage(width, height, std::move(samples), rawengine::BayerPattern::RGGB, 4000, 19000),
                                        {"rawengine.menon_base", 1});
        const rawengine::Rect area{0, 0, width, height};
        std::vector<double> times;
        double checksum = 0;
        for (unsigned run = 0; run < repeats; ++run) {
            const auto started = std::chrono::steady_clock::now();
            rawengine::Renderer{}.render_tiles(source, area, area, [&](const rawengine::Tile& tile) {
                for (std::size_t i = 0; i < tile.rgb.size(); i += 51) checksum += tile.rgb[i];
            }, tile_size);
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        }
        auto sorted = times; std::sort(sorted.begin(), sorted.end());
        const auto middle = sorted.size() / 2;
        const auto median = sorted.size() % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) * 0.5;
        std::cout << "{\"algorithm\":\"rawengine.menon_base\",\"processing_version\":1,\"width\":" << width
                  << ",\"height\":" << height << ",\"tile_size\":" << tile_size << ",\"milliseconds\":[";
        for (std::size_t i = 0; i < times.size(); ++i) std::cout << (i ? "," : "") << times[i];
        std::cout << "],\"median_ms\":" << median << ",\"checksum\":" << checksum
                  << ",\"scope\":\"synthetic uint16 source-only streaming; no decode, calibration, denoise, cache or materialized full-frame RGB\"}\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

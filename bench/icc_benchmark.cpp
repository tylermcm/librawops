#include "LittleCmsBackend.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

using namespace rawengine;

namespace {

double peak_working_set_mib() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<double>(counters.PeakWorkingSetSize) / 1048576.0;
#endif
    return -1.0;
}

template <typename F>
double median_ms(int passes, F&& run) {
    std::vector<double> measurements;
    for (int pass = 0; pass < passes; ++pass) {
        const auto start = std::chrono::steady_clock::now();
        run();
        const auto end = std::chrono::steady_clock::now();
        measurements.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::sort(measurements.begin(), measurements.end());
    return measurements[measurements.size() / 2];
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::uint32_t width = argc > 1 ? static_cast<std::uint32_t>(std::stoul(argv[1])) : 7500;
        const std::uint32_t height = argc > 2 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 6000;
        const std::uint32_t tile_size = argc > 3 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 256;
        const int passes = argc > 4 ? std::stoi(argv[4]) : 3;
        if (!width || !height || !tile_size || passes < 1 || passes > 50 ||
            static_cast<std::uint64_t>(width) * height >
                std::vector<std::uint16_t>().max_size() / 3)
            throw std::invalid_argument("invalid dimensions, tile size or pass count");
        std::ifstream file(RAWENGINE_TEST_PROFILE_PATH, std::ios::binary);
        if (!file) throw std::runtime_error("ICC fixture is missing");
        const std::vector<std::uint8_t> profile{
            std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        const auto t0 = std::chrono::steady_clock::now();
        auto display = make_lcms_display_transform(profile);
        const auto t1 = std::chrono::steady_clock::now();
        const double create_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const auto tile_pixels = static_cast<std::size_t>(tile_size) * tile_size;
        std::vector<float> display_tile(tile_pixels * 3, 0.18f);
        volatile float checksum = 0.0f;
        const double output_ms = median_ms(passes, [&] {
            for (std::uint32_t y = 0; y < height; y += tile_size)
                for (std::uint32_t x = 0; x < width; x += tile_size) {
                    const auto count = static_cast<std::size_t>(std::min(tile_size, width - x)) *
                                       std::min(tile_size, height - y);
                    std::fill_n(display_tile.data(), count * 3, 0.18f);
                    display->apply(display_tile.data(), count);
                    checksum = checksum + display_tile[0];
                }
        });
        const double output_peak = peak_working_set_mib();

        std::vector<std::uint16_t> pixels(static_cast<std::size_t>(width) * height * 3,
                                           32768);
        const auto t2 = std::chrono::steady_clock::now();
        auto source = make_lcms_raster_source({width, height, 0}, std::move(pixels), profile);
        const auto t3 = std::chrono::steady_clock::now();
        const double import_create_ms =
            std::chrono::duration<double, std::milli>(t3 - t2).count();
        const double import_ms = median_ms(passes, [&] {
            for (std::uint32_t y = 0; y < height; y += tile_size)
                for (std::uint32_t x = 0; x < width; x += tile_size) {
                    const Tile tile = source->render({x, y,
                        std::min(tile_size, width - x), std::min(tile_size, height - y)});
                    checksum = checksum + tile.rgb[0];
                }
        });
        const double import_peak = peak_working_set_mib();
        std::cout << std::fixed << std::setprecision(3)
                  << "pixels=" << static_cast<std::uint64_t>(width) * height
                  << " tile=" << tile_size << " passes=" << passes << '\n'
                  << "output_create_ms=" << create_ms
                  << " output_median_ms=" << output_ms
                  << " output_tile_buffers_mib="
                  << static_cast<double>(tile_pixels * 3 * sizeof(float) * 2) / 1048576.0
                  << " output_peak_working_set_mib=" << output_peak << '\n'
                  << "import_create_ms=" << import_create_ms
                  << " import_median_ms=" << import_ms
                  << " import_source_mib="
                  << static_cast<double>(static_cast<std::uint64_t>(width) * height * 3 *
                                         sizeof(std::uint16_t)) / 1048576.0
                  << " import_tile_buffers_mib="
                  << static_cast<double>(tile_pixels * 3 *
                                         (sizeof(float) + sizeof(std::uint16_t))) / 1048576.0
                  << " import_peak_working_set_mib=" << import_peak << '\n'
                  << "checksum=" << checksum << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

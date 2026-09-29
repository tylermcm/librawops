#include "RawEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace rawengine;

namespace {

class SyntheticLinearSource final : public Node {
public:
    explicit SyntheticLinearSource(WorkingSpace space)
        : descriptor_(ImageDescriptor::scene_linear(space)) {}
    ImageDescriptor output_descriptor() const noexcept override { return descriptor_; }
    Tile render(Rect bounds) const override {
        Tile tile{bounds, std::vector<float>(static_cast<std::size_t>(bounds.width) *
                                             bounds.height * 3), descriptor_};
        for (std::uint32_t y = 0; y < bounds.height; ++y)
            for (std::uint32_t x = 0; x < bounds.width; ++x) {
                const auto i = (static_cast<std::size_t>(y) * bounds.width + x) * 3;
                const float ramp = static_cast<float>((bounds.x + x + 3u * (bounds.y + y)) % 1024u) / 512.0f;
                tile.rgb[i] = ramp - 0.25f;
                tile.rgb[i + 1] = 0.5f * ramp;
                tile.rgb[i + 2] = 1.5f - ramp;
            }
        return tile;
    }
private:
    ImageDescriptor descriptor_;
};

std::uint32_t number(const char* text) {
    const std::string value(text);
    std::size_t end = 0;
    const auto parsed = std::stoull(value, &end);
    if (end != value.size() || !parsed || parsed > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("expected a positive uint32 argument");
    return static_cast<std::uint32_t>(parsed);
}

double stream(const Node& node, std::uint32_t width, std::uint32_t height,
              std::uint32_t tile_size) {
    double checksum = 0.0;
    for (std::uint64_t y = 0; y < height; y += tile_size)
        for (std::uint64_t x = 0; x < width; x += tile_size) {
            const Tile tile = node.render({static_cast<std::uint32_t>(x),
                                           static_cast<std::uint32_t>(y),
                                           static_cast<std::uint32_t>(std::min<std::uint64_t>(
                                               tile_size, width - x)),
                                           static_cast<std::uint32_t>(std::min<std::uint64_t>(
                                               tile_size, height - y))});
            checksum += tile.rgb[tile.rgb.size() / 2];
        }
    return checksum;
}

double median_ms(const Node& node, std::uint32_t width, std::uint32_t height,
                 std::uint32_t tile_size, std::uint32_t repetitions, double& checksum) {
    std::vector<double> timings;
    timings.reserve(repetitions);
    for (std::uint32_t run = 0; run < repetitions; ++run) {
        const auto start = std::chrono::steady_clock::now();
        checksum += stream(node, width, height, tile_size);
        const auto end = std::chrono::steady_clock::now();
        timings.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::sort(timings.begin(), timings.end());
    return timings[timings.size() / 2];
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 5)
            throw std::invalid_argument("usage: color_space_benchmark [width height repetitions tile_size]");
        const auto width = argc > 1 ? number(argv[1]) : 7500;
        const auto height = argc > 2 ? number(argv[2]) : 6000;
        const auto repetitions = argc > 3 ? number(argv[3]) : 3;
        const auto tile_size = argc > 4 ? number(argv[4]) : 256;
        if (static_cast<std::uint64_t>(tile_size) * tile_size >
            std::vector<float>().max_size() / 3)
            throw std::length_error("tile is too large");

        auto pro = std::make_shared<SyntheticLinearSource>(WorkingSpace::LinearProPhotoD50);
        auto rec = std::make_shared<SyntheticLinearSource>(WorkingSpace::LinearRec2020D65);
        auto pro_to_rec = std::make_shared<WorkingSpaceConvertNode>(
            pro, WorkingSpace::LinearRec2020D65);
        auto rec_to_pro = std::make_shared<WorkingSpaceConvertNode>(
            rec, WorkingSpace::LinearProPhotoD50);
        WorkingSpaceConvertNode roundtrip(pro_to_rec, WorkingSpace::LinearProPhotoD50);
        double checksum = 0.0;
        // Untimed warmups keep the first allocation and DLL startup out of medians.
        checksum += stream(*pro, 256, 256, 256);
        checksum += stream(*pro_to_rec, 256, 256, 256);
        const auto pro_source_ms = median_ms(*pro, width, height, tile_size, repetitions, checksum);
        const auto pro_to_rec_ms = median_ms(*pro_to_rec, width, height, tile_size, repetitions, checksum);
        const auto rec_source_ms = median_ms(*rec, width, height, tile_size, repetitions, checksum);
        const auto rec_to_pro_ms = median_ms(*rec_to_pro, width, height, tile_size, repetitions, checksum);
        const auto roundtrip_ms = median_ms(roundtrip, width, height, tile_size, repetitions, checksum);
        std::cout << std::fixed << std::setprecision(3)
                  << "{\"width\":" << width << ",\"height\":" << height
                  << ",\"repetitions\":" << repetitions << ",\"tile_size\":" << tile_size
                  << ",\"pro_source_ms\":" << pro_source_ms
                  << ",\"pro_to_rec_ms\":" << pro_to_rec_ms
                  << ",\"rec_source_ms\":" << rec_source_ms
                  << ",\"rec_to_pro_ms\":" << rec_to_pro_ms
                  << ",\"roundtrip_ms\":" << roundtrip_ms
                  << ",\"checksum\":" << checksum << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

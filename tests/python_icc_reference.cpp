#include "LittleCmsBackend.hpp"
#include "EditGraph.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace rawengine;
namespace {
std::string bytes_hex(const void* data, std::size_t bytes) {
    constexpr char digits[] = "0123456789abcdef";
    const auto* p = static_cast<const unsigned char*>(data);
    std::string result; result.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) { result.push_back(digits[p[i] >> 4]); result.push_back(digits[p[i] & 15]); }
    return result;
}
void result(const char* name, const Tile& tile, bool comma = true) {
    std::cout << '"' << name << "\":\"" << bytes_hex(tile.rgb.data(), tile.rgb.size() * sizeof(float)) << '"';
    if (comma) std::cout << ',';
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("expected ICC profile path");
        std::ifstream file(argv[1], std::ios::binary);
        if (!file) throw std::runtime_error("missing reference profile");
        std::vector<std::uint8_t> profile{std::istreambuf_iterator<char>(file), {}};
        constexpr Rect bounds{0, 0, 9, 7};
        std::vector<float> linear(9 * 7 * 3);
        std::vector<std::uint16_t> encoded(11 * 7 * 3, 65535), bayer(9 * 7);
        for (unsigned i = 0; i < linear.size(); ++i) linear[i] = float(int(i * 19 % 37) - 7) / 16;
        for (unsigned y = 0; y < 7; ++y) for (unsigned x = 0; x < 9; ++x) for (unsigned c = 0; c < 3; ++c)
            encoded[(y * 11 + x) * 3 + c] = std::uint16_t(((y * 9 + x) * 3 + c) * 7919 % 65536);
        for (unsigned i = 0; i < bayer.size(); ++i) bayer[i] = std::uint16_t(i * 197 % 4096);
        std::cout << '['; bool first = true;
        for (auto space : {WorkingSpace::LinearProPhotoD50, WorkingSpace::LinearRec2020D65})
            for (auto intent : {IccRenderingIntent::Perceptual, IccRenderingIntent::RelativeColorimetric,
                                IccRenderingIntent::Saturation, IccRenderingIntent::AbsoluteColorimetric})
                for (bool bpc : {false, true}) {
                    const IccDisplayOptions policy{intent, bpc};
                    auto output = make_lcms_display_transform(profile, policy);
                    GraphRecipe recipe; recipe.output_mode = OutputMode::IccDisplay;
                    recipe.exposure_stops = .123456789f; recipe.tone_shoulder = .456789123f; recipe.tone_gamma = 1.23456789f;
                    auto source = make_lcms_raster_source({9, 7, 11, space}, encoded, profile, policy);
                    ImageGraph float_graph(RasterImage({9, 7, 0, space}, linear), recipe, output);
                    ImageGraph import_graph(source, bounds, recipe, output);
                    RawMetadata metadata; metadata.width = 9; metadata.height = 7; metadata.active_area = bounds;
                    metadata.black_levels = {64, 96, 32, 80}; metadata.white_levels.fill(4095);
                    auto raw_recipe = recipe; raw_recipe.red_gain = 1.25f; raw_recipe.green_gain = .8f; raw_recipe.blue_gain = 1.6f;
                    raw_recipe.camera_color = CameraColorTransform{{.55,.22,.13,.17,.68,.09,.03,.11,.73}, space};
                    ImageGraph raw_graph(RawImage(metadata, bayer), raw_recipe, output);
                    if (!first) std::cout << ','; first = false;
                    const auto identity = *output->output_icc_identity();
                    std::cout << "{\"space\":\"" << (space == WorkingSpace::LinearProPhotoD50 ? "prophoto-d50" : "rec2020-d65")
                        << "\",\"intent\":\"" << identity.intent << "\",\"bpc\":" << (bpc ? "true" : "false") << ',';
                    result("float", Renderer().render_image(float_graph, bounds, 3));
                    result("icc", Renderer().render_image(import_graph, bounds, 3));
                    result("raw", Renderer().render_image(raw_graph, bounds, 3));
                    result("linear_import", source->render(bounds));
                    std::cout << "\"source_sha256\":\"" << bytes_hex(source->source_fingerprint()->data(), 32) << "\"}";
                }
        std::cout << "]\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

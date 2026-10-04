#include "CoverageOps.hpp"
#include "reference/alpha_numeric_v1.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace rawengine;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Exception, class Callback> void rejects(Callback callback) {
    try { callback(); }
    catch (const Exception&) { return; }
    throw std::runtime_error("expected exception did not occur");
}

float value(std::uint32_t bits) { return std::bit_cast<float>(bits); }

PremultipliedRgbaPixel pixel(const std::array<std::uint32_t, 8>& bits, std::size_t offset = 0) {
    return {{value(bits[offset]), value(bits[offset + 1]), value(bits[offset + 2])}, value(bits[offset + 3])};
}

void exact(PremultipliedRgbaPixel actual, const std::array<std::uint32_t, 4>& expected) {
    for (std::size_t c = 0; c < 3; ++c)
        require(std::bit_cast<std::uint32_t>(actual.rgb[c]) == expected[c], "oracle RGB bit mismatch");
    require(std::bit_cast<std::uint32_t>(actual.alpha) == expected[3], "oracle alpha bit mismatch");
}

void oracle_cases() {
    std::size_t index = 0;
    for (const auto& fixture : alpha_reference::cases) {
        auto operation = [&] {
            const auto s = pixel(fixture.input);
            if (fixture.kind == 0) return premultiply_rgb(s.rgb, s.alpha);
            if (fixture.kind == 1) return apply_coverage(s, value(fixture.input[4]));
            if (fixture.kind == 2) return source_over(s, pixel(fixture.input, 4));
            return PremultipliedRgbaPixel{straight_rgb_float32(s), 0};
        };
        try {
            if (fixture.overflow) rejects<std::overflow_error>(operation);
            else exact(operation(), fixture.output);
        } catch (const std::exception& error) {
            throw std::runtime_error("fixture " + std::to_string(index) + ": " + error.what());
        }
        ++index;
    }
    for (const auto& fixture : alpha_reference::straight_cases) {
        const PremultipliedRgbaPixel s{{value(fixture.input[0]), value(fixture.input[1]),
                                        value(fixture.input[2])}, value(fixture.input[3])};
        const auto result = straight_rgb(s);
        for (std::size_t c = 0; c < 3; ++c)
            require(std::bit_cast<std::uint64_t>(result[c]) == fixture.output[c], "binary64 straight mismatch");
    }
}

void admission_and_endpoints() {
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto inf = std::numeric_limits<float>::infinity();
    for (float alpha : {-1.0f, 1.000001f, nan, inf, -inf}) {
        rejects<std::invalid_argument>([&] { premultiply_rgb(std::array<float,3>{1,2,3}, alpha); });
        rejects<std::invalid_argument>([&] { apply_coverage(PremultipliedRgbaPixel{{1,2,3},1}, alpha); });
    }
    for (float color : {nan, inf, -inf}) {
        rejects<std::invalid_argument>([&] { premultiply_rgb(std::array<float,3>{0,color,0}, 0); });
        rejects<std::invalid_argument>([&] { source_over(PremultipliedRgbaPixel{{color,0,0},0}, {}); });
        rejects<std::invalid_argument>([&] { source_over(PremultipliedRgbaPixel{{1,2,3},1}, {{color,0,0},1}); });
    }
    const PremultipliedRgbaPixel hidden{{1,0,0},0};
    rejects<std::invalid_argument>([&] { straight_rgb(hidden); });
    rejects<std::invalid_argument>([&] { apply_coverage(hidden, 0); });
    rejects<std::invalid_argument>([&] { source_over({}, hidden); });
    exact(premultiply_rgb(std::array<float,3>{-5,2,3}, -0.0f), {0,0,0,0});
    exact(apply_coverage(PremultipliedRgbaPixel{{-0.0f,0,-0.0f},-0.0f}, 1), {0,0,0,0});
    exact(source_over({}, PremultipliedRgbaPixel{{-0.0f,0,-0.0f},-0.0f}), {0,0,0,0});
    const PremultipliedRgbaPixel p{{-0.0f,-1,4},1};
    exact(premultiply_rgb(p.rgb, 1), {0x80000000,0xbf800000,0x40800000,0x3f800000});
    exact(apply_coverage(p, 1), {0x80000000,0xbf800000,0x40800000,0x3f800000});
    exact(source_over(p, {}), {0x80000000,0xbf800000,0x40800000,0x3f800000});
    // Tiny positive alpha is preserved; narrowing huge straight color is explicit.
    const PremultipliedRgbaPixel tiny{{std::numeric_limits<float>::max(),-1,0}, value(1)};
    const auto wide = straight_rgb(tiny);
    require(std::isfinite(wide[0]) && wide[0] > std::numeric_limits<float>::max(), "tiny alpha safe access");
    rejects<std::overflow_error>([&] { straight_rgb_float32(tiny); });
    exact(apply_coverage(tiny, 0.5f), {0,0,0,0});
    const auto s = premultiply_rgb(std::array<float,3>{2,-1,4}, 0.25f);
    const auto b = premultiply_rgb(std::array<float,3>{-0.5f,3,1}, 0.5f);
    const auto out = source_over(s,b);
    require(out.rgb == std::array<float,3>{0.3125f,0.875f,1.375f} && out.alpha == 0.625f,
            "independent signed/headroom example");
    require(source_over(b,s).rgb != out.rgb, "source order must affect composition");
}

void tile_cases() {
    for (auto space : {WorkingSpace::LinearProPhotoD50, WorkingSpace::LinearRec2020D65}) {
        const Rect bounds{11,17,13,7};
        Tile rgb{bounds, {}, ImageDescriptor::scene_linear(space)};
        CoverageTile coverage{bounds,{}};
        for (std::size_t i = 0; i < 91; ++i) {
            rgb.rgb.insert(rgb.rgb.end(), {static_cast<float>(i % 9) - 4,
                                           static_cast<float>(i % 7) * 0.25f, -0.5f});
            coverage.coverage.push_back(static_cast<float>(i % 5) * 0.25f);
        }
        const auto saved_rgb = rgb.rgb;
        const auto saved_coverage = coverage.coverage;
        const auto full = premultiply_rgb(rgb, coverage);
        validate_premultiplied_rgba_tile(full);
        const auto masked = apply_coverage(full, coverage);
        const auto composed = source_over(masked, full);
        for (std::uint32_t y = 0; y < 7; ++y) {
            for (std::uint32_t x = 0; x < 13; x += 3) {
                const auto width = std::min(3u, 13u-x);
                const auto start = static_cast<std::size_t>(y)*13+x;
                const Rect part{bounds.x+x,bounds.y+y,width,1};
                Tile input{part, std::vector<float>(rgb.rgb.begin()+start*3,
                           rgb.rgb.begin()+(start+width)*3), rgb.descriptor};
                CoverageTile alpha{part, std::vector<float>(coverage.coverage.begin()+start,
                                   coverage.coverage.begin()+start+width)};
                const auto p = premultiply_rgb(input,alpha);
                const auto a = apply_coverage(p,alpha);
                const auto o = source_over(a,p);
                for (std::size_t i = 0; i < width*4; ++i) {
                    require(std::bit_cast<std::uint32_t>(p.rgba[i]) ==
                            std::bit_cast<std::uint32_t>(full.rgba[start*4+i]), "premultiply partition parity");
                    require(std::bit_cast<std::uint32_t>(a.rgba[i]) ==
                            std::bit_cast<std::uint32_t>(masked.rgba[start*4+i]), "coverage partition parity");
                    require(std::bit_cast<std::uint32_t>(o.rgba[i]) ==
                            std::bit_cast<std::uint32_t>(composed.rgba[start*4+i]), "source-over partition parity");
                }
            }
        }
        require(rgb.rgb == saved_rgb && coverage.coverage == saved_coverage, "owned inputs changed");
        auto copy = full;
        copy.rgba[0] = 99;
        require(full.rgba[0] == 0 && rgb.rgb == saved_rgb, "output storage aliases input");
        auto short_rgba = full; short_rgba.rgba.pop_back();
        rejects<std::invalid_argument>([&] { source_over(full,short_rgba); });
        auto invalid = full; invalid.rgba.back() = 2;
        rejects<std::invalid_argument>([&] { apply_coverage(invalid,coverage); });
        invalid = full; invalid.working_space = static_cast<WorkingSpace>(99);
        rejects<std::invalid_argument>([&] { validate_premultiplied_rgba_tile(invalid); });
        invalid = full; invalid.working_space = space == WorkingSpace::LinearProPhotoD50 ?
            WorkingSpace::LinearRec2020D65 : WorkingSpace::LinearProPhotoD50;
        rejects<std::invalid_argument>([&] { source_over(full,invalid); });
        auto shifted = coverage; ++shifted.bounds.x;
        rejects<std::invalid_argument>([&] { premultiply_rgb(rgb,shifted); });
        auto bad_rgb = rgb; bad_rgb.descriptor = ImageDescriptor::srgb_output();
        rejects<std::invalid_argument>([&] { premultiply_rgb(bad_rgb,coverage); });
        bad_rgb = rgb; bad_rgb.descriptor.alpha = static_cast<AlphaMode>(1);
        rejects<std::invalid_argument>([&] { premultiply_rgb(bad_rgb,coverage); });
        bad_rgb = rgb; bad_rgb.rgb.pop_back();
        rejects<std::invalid_argument>([&] { premultiply_rgb(bad_rgb,coverage); });
        bad_rgb = rgb; bad_rgb.rgb.back() = std::numeric_limits<float>::quiet_NaN();
        auto zero_alpha = coverage; for (auto& a : zero_alpha.coverage) a = 0;
        rejects<std::invalid_argument>([&] { premultiply_rgb(bad_rgb,zero_alpha); });
    }
    rejects<std::invalid_argument>([] { validate_coverage_tile({{0,0,0,1},{}}); });
    rejects<std::invalid_argument>([] { validate_coverage_tile({{UINT32_MAX,0,1,1},{0}}); });
    rejects<std::invalid_argument>([] { validate_coverage_tile({{0,0,1,1},{0,1}}); });
    rejects<std::invalid_argument>([] { validate_coverage_tile({{0,0,1,1},{-1}}); });
    rejects<std::invalid_argument>([] { validate_premultiplied_rgba_tile({{0,0,UINT32_MAX,UINT32_MAX},{}}); });
}

} // namespace

int main() {
    try {
        oracle_cases();
        admission_and_endpoints();
        tile_cases();
        std::cout << "496 exact staged fixtures,120 wide straight fixtures,validation,endpoints,"
                     "two-space tile partitions and ownership passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

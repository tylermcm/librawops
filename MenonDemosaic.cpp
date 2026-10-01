// SPDX-License-Identifier: BSD-3-Clause
// Adapted from Colour Developers' Menon 2007 implementation, commit
// 7bff324983fb77b41444fda3bf922e354d386d1c; Copyright 2015 Colour Developers.
// Adaptation copyright 2026 LibRawOps contributors.
// See third_party/colour-demosaicing/LICENSE for the notice and disclaimer.
#include "MenonDemosaic.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rawengine::detail {
namespace {
using Coordinate = std::int64_t;
constexpr std::uint32_t block_size = 256;

Rect support(Rect r, Rect area, unsigned left, unsigned top, unsigned right_halo, unsigned bottom_halo) {
    const auto x = std::max<Coordinate>(area.x, Coordinate(r.x) - left);
    const auto y = std::max<Coordinate>(area.y, Coordinate(r.y) - top);
    const auto right = std::min<Coordinate>(Coordinate(area.x) + area.width, Coordinate(r.x) + r.width + right_halo);
    const auto bottom = std::min<Coordinate>(Coordinate(area.y) + area.height, Coordinate(r.y) + r.height + bottom_halo);
    return {static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
            static_cast<std::uint32_t>(right - x), static_cast<std::uint32_t>(bottom - y)};
}

Rect expand(Rect r, Rect area, std::uint32_t radius) { return support(r, area, radius, radius, radius, radius); }

template<class T> struct Plane {
    Rect bounds;
    std::vector<T> values;
    explicit Plane(Rect r) : bounds(r), values(static_cast<std::size_t>(r.width) * r.height) {}
    std::size_t index(Coordinate x, Coordinate y) const {
        if (x < bounds.x || y < bounds.y || x >= Coordinate(bounds.x) + bounds.width || y >= Coordinate(bounds.y) + bounds.height)
            throw std::logic_error("Menon stage exceeded its declared source support");
        return static_cast<std::size_t>(y - bounds.y) * bounds.width + static_cast<std::size_t>(x - bounds.x);
    }
    T& at(Coordinate x, Coordinate y) { return values[index(x, y)]; }
    const T& at(Coordinate x, Coordinate y) const { return values[index(x, y)]; }
};

template<class F> void pixels(Rect r, F action) {
    for (Coordinate y = r.y; y < Coordinate(r.y) + r.height; ++y)
        for (Coordinate x = r.x; x < Coordinate(r.x) + r.width; ++x) action(x, y);
}

struct Sensor {
    const RawImage& image;
    const RawMetadata& metadata;
    Rect area;
    explicit Sensor(const RawImage& raw) : image(raw), metadata(raw.metadata()), area(metadata.active_area) {}
    bool contains(Coordinate x, Coordinate y) const {
        return x >= area.x && y >= area.y && x < Coordinate(area.x) + area.width && y < Coordinate(area.y) + area.height;
    }
    Coordinate mirror(Coordinate value, Coordinate origin, Coordinate size) const {
        if (size == 1) return origin;
        const auto period = 2 * (size - 1);
        auto folded = (value - origin) % period;
        if (folded < 0) folded += period;
        return origin + (folded < size ? folded : period - folded);
    }
    template<class T> double get(const Plane<T>& p, Coordinate x, Coordinate y) const {
        if (!contains(x, y)) {
            x = mirror(x, area.x, area.width); y = mirror(y, area.y, area.height);
        }
        return p.at(x, y);
    }
    std::size_t site(Coordinate x, Coordinate y) const {
        return ((static_cast<unsigned>(y & 1) + metadata.cfa_phase_y) & 1) * 2
             + ((static_cast<unsigned>(x & 1) + metadata.cfa_phase_x) & 1);
    }
    int color(Coordinate x, Coordinate y) const {
        constexpr int patterns[4][4] = {{0, 1, 1, 2}, {2, 1, 1, 0}, {1, 0, 2, 1}, {1, 2, 0, 1}};
        return patterns[static_cast<int>(metadata.pattern)][site(x, y)];
    }
    float sample(Coordinate x, Coordinate y) const {
        const auto s = site(x, y);
        const auto code = image.samples()[static_cast<std::size_t>(y) * metadata.row_stride_samples + static_cast<std::size_t>(x)];
        return (static_cast<float>(code) - metadata.black_levels[s]) /
               (metadata.white_levels[s] - metadata.black_levels[s]);
    }
};

void block(const Sensor& sensor, Rect r, Tile& output) {
    // Final opposite colors read green-site colors at radius 1; those read
    // selected green at radius 2. Its classifier reads chroma at radius 4,
    // whose five-tap estimate reads normalized raw at radius 6. Clip only to
    // the true active area. Gradient buffers have asymmetric extents: the
    // classifier's horizontal offsets are [-2,0]x[-2,2], with vertical
    // transposition. This keeps forward differences inside chroma's radius 4.
    Plane<float> raw(expand(r, sensor.area, 6));
    const auto directional_bounds = expand(r, sensor.area, 4);
    Plane<double> gh(directional_bounds), gv(directional_bounds), ch(directional_bounds), cv(directional_bounds);
    const auto green_bounds = expand(r, sensor.area, 2);
    Plane<double> green(green_bounds);
    Plane<std::uint8_t> horizontal(green_bounds);
    Plane<double> dh(support(green_bounds, sensor.area, 2, 2, 0, 2));
    Plane<double> dv(support(green_bounds, sensor.area, 2, 2, 2, 0));
    const auto color_bounds = expand(r, sensor.area, 1);
    Plane<double> red(color_bounds), blue(color_bounds);
    pixels(raw.bounds, [&](Coordinate x, Coordinate y) { raw.at(x, y) = sensor.sample(x, y); });
    pixels(directional_bounds, [&](Coordinate x, Coordinate y) {
        const double own = raw.at(x, y);
        if (sensor.color(x, y) == 1) {
            gh.at(x, y) = gv.at(x, y) = own;
        } else {
            const double h = 0.5 * (sensor.get(raw, x - 1, y) + sensor.get(raw, x + 1, y))
                           + 0.5 * own - 0.25 * (sensor.get(raw, x - 2, y) + sensor.get(raw, x + 2, y));
            const double v = 0.5 * (sensor.get(raw, x, y - 1) + sensor.get(raw, x, y + 1))
                           + 0.5 * own - 0.25 * (sensor.get(raw, x, y - 2) + sensor.get(raw, x, y + 2));
            gh.at(x, y) = h; gv.at(x, y) = v;
            ch.at(x, y) = own - h; cv.at(x, y) = own - v;
        }
    });
    pixels(dh.bounds, [&](Coordinate x, Coordinate y) {
        dh.at(x, y) = std::abs(ch.at(x, y) - sensor.get(ch, x + 2, y));
    });
    pixels(dv.bounds, [&](Coordinate x, Coordinate y) {
        dv.at(x, y) = std::abs(cv.at(x, y) - sensor.get(cv, x, y + 2));
    });
    auto gradient = [&](const Plane<double>& p, Coordinate x, Coordinate y) {
        // Zero extension is applied after the mirrored forward difference.
        return sensor.contains(x, y) ? p.at(x, y) : 0.0;
    };
    struct Term { int x, y; double weight; };
    constexpr Term classifier[] = {{0, 2, 1}, {-2, 2, 1}, {-1, 1, 1}, {0, 0, 3},
                                   {-2, 0, 3}, {-1, -1, 1}, {0, -2, 1}, {-2, -2, 1}};
    pixels(green_bounds, [&](Coordinate x, Coordinate y) {
        if (sensor.color(x, y) == 1) {
            // Every classifier term at green is zero (its offsets preserve
            // green parity); upstream's horizontal tie is therefore fixed.
            horizontal.at(x, y) = 1; green.at(x, y) = raw.at(x, y);
            return;
        }
        double h = 0, v = 0;
        for (const auto& t : classifier) {
            h += t.weight * gradient(dh, x + t.x, y + t.y);
            v += t.weight * gradient(dv, x + t.y, y + t.x);
        }
        horizontal.at(x, y) = v >= h;
        green.at(x, y) = v >= h ? gh.at(x, y) : gv.at(x, y);
    });
    auto mean = [&](const auto& p, Coordinate x, Coordinate y, int dx, int dy) {
        return 0.5 * (sensor.get(p, x - dx, y - dy) + sensor.get(p, x + dx, y + dy));
    };
    pixels(color_bounds, [&](Coordinate x, Coordinate y) {
        const auto own = sensor.color(x, y);
        if (own == 0) red.at(x, y) = raw.at(x, y);
        if (own == 2) blue.at(x, y) = raw.at(x, y);
        if (own == 1) {
            const bool red_horizontal = sensor.color(x + 1, y) == 0;
            const int dx = red_horizontal ? 1 : 0, dy = red_horizontal ? 0 : 1;
            red.at(x, y) = green.at(x, y) + mean(raw, x, y, dx, dy) - mean(green, x, y, dx, dy);
            blue.at(x, y) = green.at(x, y) + mean(raw, x, y, dy, dx) - mean(green, x, y, dy, dx);
        }
    });
    pixels(r, [&](Coordinate x, Coordinate y) {
        const auto own = sensor.color(x, y);
        const int dx = horizontal.at(x, y) ? 1 : 0, dy = horizontal.at(x, y) ? 0 : 1;
        double rr = red.at(x, y), bb = blue.at(x, y);
        if (own == 2) rr = bb + mean(red, x, y, dx, dy) - mean(blue, x, y, dx, dy);
        if (own == 0) bb = rr + mean(blue, x, y, dx, dy) - mean(red, x, y, dx, dy);
        const auto i = (static_cast<std::size_t>(y - output.bounds.y) * output.bounds.width +
                        static_cast<std::size_t>(x - output.bounds.x)) * 3;
        output.rgb[i] = static_cast<float>(rr);
        output.rgb[i + 1] = static_cast<float>(green.at(x, y));
        output.rgb[i + 2] = static_cast<float>(bb);
    });
}
} // namespace

Tile render_menon_base(const RawImage& image, Rect request) {
    const auto max = std::vector<float>().max_size();
    if (request.width && request.height > max / request.width / 3)
        throw std::length_error("image dimensions exceed addressable storage");
    Tile output{request, std::vector<float>(static_cast<std::size_t>(request.width) * request.height * 3)};
    const Sensor sensor(image);
    for (std::uint64_t y = request.y; y < std::uint64_t(request.y) + request.height; y += block_size)
        for (std::uint64_t x = request.x; x < std::uint64_t(request.x) + request.width; x += block_size) {
            const Rect r{static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                         static_cast<std::uint32_t>(std::min<std::uint64_t>(block_size, std::uint64_t(request.x) + request.width - x)),
                         static_cast<std::uint32_t>(std::min<std::uint64_t>(block_size, std::uint64_t(request.y) + request.height - y))};
            block(sensor, r, output);
        }
    return output;
}
} // namespace rawengine::detail

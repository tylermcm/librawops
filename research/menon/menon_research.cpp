// SPDX-License-Identifier: BSD-3-Clause
// Portions adapted from Colour Developers' Menon 2007 implementation.
// Copyright 2015 Colour Developers. Adaptation copyright 2026 LibRawOps contributors.
// See LICENSE.colour-demosaicing. Pinned reference commit:
// 7bff324983fb77b41444fda3bf922e354d386d1c.
// Standalone research only; not an admitted product demosaicing backend.

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Plane = std::vector<double>;
constexpr std::array<std::array<int, 4>, 4> patterns{{
    {{0, 1, 1, 2}}, {{2, 1, 1, 0}}, {{1, 0, 2, 1}}, {{1, 2, 0, 1}}
}};

struct Image {
    int width, height, pattern;
    std::vector<float> raw;
    std::size_t index(int x, int y) const { return static_cast<std::size_t>(y) * width + x; }
    int color(int x, int y) const { return patterns[pattern][((y & 1) * 2) + (x & 1)]; }
    int mirror(int coordinate, int size) const {
        if (size == 1) return 0;
        const int period = 2 * (size - 1);
        int folded = coordinate % period;
        if (folded < 0) folded += period;
        return folded < size ? folded : period - folded;
    }
    double get(const Plane& p, int x, int y, bool zero_outside = false) const {
        if (x >= 0 && y >= 0 && x < width && y < height) return p[index(x, y)];
        if (zero_outside) return 0;
        return p[index(mirror(x, width), mirror(y, height))];
    }
};

std::vector<float> bilinear(const Image& im) {
    std::vector<float> out(im.raw.size() * 3);
    for (int y = 0; y < im.height; ++y) for (int x = 0; x < im.width; ++x) {
        for (int c = 0; c < 3; ++c) {
            float value = im.raw[im.index(x, y)];
            if (im.color(x, y) != c) {
                float sum = 0;
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                    int xx = x + dx, yy = y + dy;
                    if (xx >= 0 && yy >= 0 && xx < im.width && yy < im.height && im.color(xx, yy) == c) {
                        sum += im.raw[im.index(xx, yy)];
                        ++count;
                    }
                }
                value = count ? sum / count : 0;
            }
            out[im.index(x, y) * 3 + c] = value;
        }
    }
    return out;
}

std::vector<float> reconstruct(const Image& im, bool refine) {
    if (im.width == 1 || im.height == 1) return bilinear(im);
    const auto size = im.raw.size();
    Plane raw(im.raw.begin(), im.raw.end());
    Plane r(size, 0), g(size, 0), b(size, 0), gh(size), gv(size), ch(size, 0), cv(size, 0);
    for (int y = 0; y < im.height; ++y) for (int x = 0; x < im.width; ++x) {
        const auto i = im.index(x, y);
        const int c = im.color(x, y);
        if (c == 0) r[i] = raw[i];
        if (c == 1) g[i] = raw[i];
        if (c == 2) b[i] = raw[i];
        if (c == 1) {
            gh[i] = gv[i] = raw[i];
        } else {
            gh[i] = 0.5 * (im.get(raw, x - 1, y) + im.get(raw, x + 1, y))
                  + 0.5 * raw[i] - 0.25 * (im.get(raw, x - 2, y) + im.get(raw, x + 2, y));
            gv[i] = 0.5 * (im.get(raw, x, y - 1) + im.get(raw, x, y + 1))
                  + 0.5 * raw[i] - 0.25 * (im.get(raw, x, y - 2) + im.get(raw, x, y + 2));
            ch[i] = raw[i] - gh[i];
            cv[i] = raw[i] - gv[i];
        }
    }
    Plane dh(size), dv(size);
    for (int y = 0; y < im.height; ++y) for (int x = 0; x < im.width; ++x) {
        const auto i = im.index(x, y);
        dh[i] = std::abs(ch[i] - im.get(ch, x + 2, y));
        dv[i] = std::abs(cv[i] - im.get(cv, x, y + 2));
    }
    struct Term { int dx, dy; double weight; };
    // Reversed offsets of upstream scipy.ndimage.convolve's asymmetric kernel.
    constexpr std::array<Term, 8> classifier{{
        {0, 2, 1}, {-2, 2, 1}, {-1, 1, 1}, {0, 0, 3},
        {-2, 0, 3}, {-1, -1, 1}, {0, -2, 1}, {-2, -2, 1}
    }};
    std::vector<std::uint8_t> horizontal(size);
    for (int y = 0; y < im.height; ++y) for (int x = 0; x < im.width; ++x) {
        double h = 0, v = 0;
        for (const auto& t : classifier) {
            h += t.weight * im.get(dh, x + t.dx, y + t.dy, true);
            v += t.weight * im.get(dv, x + t.dy, y + t.dx, true);
        }
        const auto i = im.index(x, y);
        horizontal[i] = v >= h;
        g[i] = horizontal[i] ? gh[i] : gv[i];
    }
    auto mean = [&](const Plane& p, int x, int y, int dx, int dy) {
        return 0.5 * (im.get(p, x - dx, y - dy) + im.get(p, x + dx, y + dy));
    };
    // Completed green plane supplies both color reconstructions at green sites.
    for (int y = 0; y < im.height; ++y) for (int x = 0; x < im.width; ++x) {
        if (im.color(x, y) != 1) continue;
        const auto i = im.index(x, y);
        const bool red_horizontal = im.color(x + 1, y) == 0;
        const int rx = red_horizontal ? 1 : 0, ry = red_horizontal ? 0 : 1;
        r[i] = g[i] + mean(raw, x, y, rx, ry) - mean(g, x, y, rx, ry);
        b[i] = g[i] + mean(raw, x, y, ry, rx) - mean(g, x, y, ry, rx);
    }
    // Opposite colors read completed green-site colors; they never read each other.
    for (int y = 0; y < im.height; ++y) for (int x = 0; x < im.width; ++x) {
        const auto i = im.index(x, y);
        const int c = im.color(x, y);
        const int dx = horizontal[i] ? 1 : 0, dy = horizontal[i] ? 0 : 1;
        if (c == 2) r[i] = b[i] + mean(r, x, y, dx, dy) - mean(b, x, y, dx, dy);
        if (c == 0) b[i] = r[i] + mean(b, x, y, dx, dy) - mean(r, x, y, dx, dy);
    }
    if (refine) {
        Plane rg(size), bg(size);
        for (std::size_t i = 0; i < size; ++i) { rg[i] = r[i] - g[i]; bg[i] = b[i] - g[i]; }
        auto thirds = [&](const Plane& p, int x, int y, int dx, int dy) {
            constexpr double weight = 1.0 / 3.0;
            return im.get(p, x - dx, y - dy) * weight + p[im.index(x, y)] * weight
                 + im.get(p, x + dx, y + dy) * weight;
        };
        // Refinement 1: only missing green; deltas come from the completed base.
        for (int y = 0; y < im.height; ++y) for (int x = 0; x < im.width; ++x) {
            const auto i = im.index(x, y);
            const int dx = horizontal[i] ? 1 : 0, dy = horizontal[i] ? 0 : 1;
            if (im.color(x, y) == 0) g[i] = r[i] - thirds(rg, x, y, dx, dy);
            if (im.color(x, y) == 2) g[i] = b[i] - thirds(bg, x, y, dx, dy);
        }
        // Refinement 2: freeze differences after the green update.
        for (std::size_t i = 0; i < size; ++i) { rg[i] = r[i] - g[i]; bg[i] = b[i] - g[i]; }
        for (int y = 0; y < im.height; ++y) for (int x = 0; x < im.width; ++x) {
            if (im.color(x, y) != 1) continue;
            const auto i = im.index(x, y);
            const bool red_horizontal = im.color(x + 1, y) == 0;
            const int rx = red_horizontal ? 1 : 0, ry = red_horizontal ? 0 : 1;
            r[i] = g[i] + mean(rg, x, y, rx, ry);
            b[i] = g[i] + mean(bg, x, y, ry, rx);
        }
        // Refinement 3: freeze R-B before either opposite-color update.
        Plane rb(size);
        for (std::size_t i = 0; i < size; ++i) rb[i] = r[i] - b[i];
        for (int y = 0; y < im.height; ++y) for (int x = 0; x < im.width; ++x) {
            const auto i = im.index(x, y);
            const int dx = horizontal[i] ? 1 : 0, dy = horizontal[i] ? 0 : 1;
            if (im.color(x, y) == 2) r[i] = b[i] + thirds(rb, x, y, dx, dy);
            if (im.color(x, y) == 0) b[i] = r[i] - thirds(rb, x, y, dx, dy);
        }
    }
    std::vector<float> out(size * 3);
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 3] = static_cast<float>(r[i]);
        out[i * 3 + 1] = static_cast<float>(g[i]);
        out[i * 3 + 2] = static_cast<float>(b[i]);
    }
    return out;
}

int number(const char* argument, int minimum, int maximum) {
    const std::string text(argument);
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) throw std::invalid_argument("integer control required");
    const auto value = std::stoull(text);
    if (value < static_cast<unsigned>(minimum) || value > static_cast<unsigned>(maximum)) throw std::invalid_argument("integer control outside research bounds");
    return static_cast<int>(value);
}
} // namespace

int main(int argc, char** argv) {
    try {
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
        if constexpr (std::endian::native != std::endian::little) throw std::runtime_error("research file bridge requires a little-endian host");
        if (argc != 7 && argc != 11) throw std::invalid_argument("usage: input.f32 width height pattern base|refined output.f32 [x y width height]");
        const int width = number(argv[2], 1, 1'000'000), height = number(argv[3], 1, 1'000'000);
        const auto count = static_cast<std::uint64_t>(width) * height;
        if (count > 1'000'000) throw std::invalid_argument("research sample budget exceeded");
        const int pattern = number(argv[4], 0, 3);
        const std::string mode(argv[5]);
        if (mode != "base" && mode != "refined") throw std::invalid_argument("unknown reconstruction mode");
        int x = 0, y = 0, rw = width, rh = height;
        if (argc == 11) {
            x = number(argv[7], 0, width - 1); y = number(argv[8], 0, height - 1);
            rw = number(argv[9], 1, width); rh = number(argv[10], 1, height);
            if (x + rw > width || y + rh > height) throw std::invalid_argument("ROI outside active plane");
        }
        Image im{width, height, pattern, std::vector<float>(static_cast<std::size_t>(count))};
        std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
        if (!input || input.tellg() != static_cast<std::streamoff>(count * sizeof(float))) throw std::invalid_argument("input file size differs from Bayer dimensions");
        input.seekg(0);
        input.read(reinterpret_cast<char*>(im.raw.data()), static_cast<std::streamsize>(count * sizeof(float)));
        if (!input) throw std::runtime_error("cannot read Bayer plane");
        for (float value : im.raw) if (!std::isfinite(value)) throw std::invalid_argument("nonfinite Bayer sample");
        const auto started = std::chrono::steady_clock::now();
        auto rgb = reconstruct(im, mode == "refined");
        const auto finished = std::chrono::steady_clock::now();
        for (float value : rgb) if (!std::isfinite(value)) throw std::overflow_error("nonfinite float32 reconstruction");
        std::ofstream output(argv[6], std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot open research output");
        for (int row = y; row < y + rh; ++row) {
            const auto offset = (static_cast<std::size_t>(row) * width + x) * 3;
            output.write(reinterpret_cast<const char*>(rgb.data() + offset), static_cast<std::streamsize>(rw) * 3 * sizeof(float));
        }
        if (!output) throw std::runtime_error("cannot write research output");
        const auto ms = std::chrono::duration<double, std::milli>(finished - started).count();
        std::cout << "{\"width\":" << rw << ",\"height\":" << rh << ",\"mode\":\"" << mode
                  << "\",\"reconstruction_ms\":" << ms << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

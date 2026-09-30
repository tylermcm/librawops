#include "RawEngine.hpp"
#include "LittleCmsBackend.hpp"
#include "Sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace rawengine {
namespace {

void u8(Sha256& hash, std::uint8_t value) { hash.update(&value, 1); }

void u16(Sha256& hash, std::uint16_t value) {
    const std::uint8_t bytes[]{static_cast<std::uint8_t>(value),
                               static_cast<std::uint8_t>(value >> 8)};
    hash.update(bytes, sizeof(bytes));
}

void u32(Sha256& hash, std::uint32_t value) {
    const std::uint8_t bytes[]{static_cast<std::uint8_t>(value),
                               static_cast<std::uint8_t>(value >> 8),
                               static_cast<std::uint8_t>(value >> 16),
                               static_cast<std::uint8_t>(value >> 24)};
    hash.update(bytes, sizeof(bytes));
}

void tag(Sha256& hash, const char* name) {
    hash.update(name, std::strlen(name) + 1);
}

void u16_row(Sha256& hash, const std::uint16_t* samples, std::size_t count) {
    std::array<std::uint8_t, 8192> bytes{};
    while (count) {
        const auto n = std::min(count, bytes.size() / 2);
        for (std::size_t i = 0; i < n; ++i) {
            bytes[2 * i] = static_cast<std::uint8_t>(samples[i]);
            bytes[2 * i + 1] = static_cast<std::uint8_t>(samples[i] >> 8);
        }
        hash.update(bytes.data(), n * 2);
        samples += n; count -= n;
    }
}

void f32_row(Sha256& hash, const float* samples, std::size_t count) {
    std::array<std::uint8_t, 8192> bytes{};
    while (count) {
        const auto n = std::min(count, bytes.size() / 4);
        for (std::size_t i = 0; i < n; ++i) {
            const auto bits = std::bit_cast<std::uint32_t>(samples[i]);
            for (int b = 0; b < 4; ++b)
                bytes[4 * i + b] = static_cast<std::uint8_t>(bits >> (8 * b));
        }
        hash.update(bytes.data(), n * 4);
        samples += n; count -= n;
    }
}

} // namespace

std::array<std::uint8_t, 32> RawImage::fingerprint() const {
    std::call_once(*fingerprint_once_, [&] {
        const auto& m = metadata();
        Sha256 hash;
        tag(hash, "librawops.source.raw.u16.v1");
        u32(hash, m.width); u32(hash, m.height);
        u8(hash, static_cast<std::uint8_t>(m.pattern));
        u8(hash, m.cfa_phase_x); u8(hash, m.cfa_phase_y);
        u32(hash, m.active_area.x); u32(hash, m.active_area.y);
        u32(hash, m.active_area.width); u32(hash, m.active_area.height);
        for (auto value : m.black_levels) u16(hash, value);
        for (auto value : m.white_levels) u16(hash, value);
        for (std::uint32_t y = m.active_area.y;
             y < m.active_area.y + m.active_area.height; ++y)
            u16_row(hash, samples().data() +
                          static_cast<std::size_t>(y) * m.row_stride_samples + m.active_area.x,
                    m.active_area.width);
        *fingerprint_ = hash.finish();
    });
    return *fingerprint_;
}

std::array<std::uint8_t, 32> RasterImage::fingerprint() const {
    std::call_once(*fingerprint_once_, [&] {
        const auto& m = metadata();
        Sha256 hash;
        tag(hash, "librawops.source.raster.f32.v1");
        u32(hash, m.width); u32(hash, m.height);
        u8(hash, static_cast<std::uint8_t>(m.working_space));
        for (std::uint32_t y = 0; y < m.height; ++y)
            f32_row(hash, pixels().data() +
                          static_cast<std::size_t>(y) * m.row_stride_pixels * 3,
                    static_cast<std::size_t>(m.width) * 3);
        *fingerprint_ = hash.finish();
    });
    return *fingerprint_;
}

std::array<std::uint8_t, 32> fingerprint_raw_source(const RawImage& image) {
    return image.fingerprint();
}

std::array<std::uint8_t, 32> fingerprint_raster_source(const RasterImage& image) {
    return image.fingerprint();
}

std::array<std::uint8_t, 32> fingerprint_icc_raster_source(
    IccRasterMetadata m, const std::vector<std::uint16_t>& pixels) {
    if (!m.width || !m.height ||
        (m.working_space != WorkingSpace::LinearProPhotoD50 &&
         m.working_space != WorkingSpace::LinearRec2020D65))
        throw std::invalid_argument("invalid ICC raster fingerprint metadata");
    if (!m.row_stride_pixels) m.row_stride_pixels = m.width;
    if (m.row_stride_pixels < m.width ||
        static_cast<std::size_t>(m.row_stride_pixels) >
            pixels.max_size() / 3 / m.height ||
        pixels.size() != static_cast<std::size_t>(m.row_stride_pixels) * m.height * 3)
        throw std::invalid_argument("ICC raster fingerprint sample count differs from stride");
    Sha256 hash;
    tag(hash, "librawops.source.icc-raster.u16.v1");
    u32(hash, m.width); u32(hash, m.height);
    u8(hash, static_cast<std::uint8_t>(m.working_space));
    for (std::uint32_t y = 0; y < m.height; ++y)
        u16_row(hash, pixels.data() +
                      static_cast<std::size_t>(y) * m.row_stride_pixels * 3,
                static_cast<std::size_t>(m.width) * 3);
    return hash.finish();
}

} // namespace rawengine

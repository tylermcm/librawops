#include "Sha256.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace rawengine {
namespace {
constexpr std::uint32_t constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};
}

void Sha256::block(const std::uint8_t* bytes) {
    std::uint32_t w[64]{};
    for (int i = 0; i < 16; ++i) {
        const auto p = bytes + i * 4;
        w[i] = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
               (std::uint32_t(p[2]) << 8) | p[3];
    }
    for (int i = 16; i < 64; ++i) {
        const auto s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const auto s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = state_;
    for (int i = 0; i < 64; ++i) {
        const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto choice = (e & f) ^ (~e & g);
        const auto t1 = h + s1 + choice + constants[i] + w[i];
        const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto t2 = s0 + majority;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) {
    if (size > std::numeric_limits<std::uint64_t>::max() / 8 - bytes_)
        throw std::length_error("SHA-256 input is too large");
    bytes_ += size;
    auto* input = static_cast<const std::uint8_t*>(data);
    while (size) {
        const auto count = std::min(size, buffer_.size() - buffered_);
        std::memcpy(buffer_.data() + buffered_, input, count);
        buffered_ += count; input += count; size -= count;
        if (buffered_ == buffer_.size()) {
            block(buffer_.data());
            buffered_ = 0;
        }
    }
}

std::array<std::uint8_t, 32> Sha256::finish() {
    const auto bit_length = bytes_ * 8;
    const std::uint8_t one = 0x80, zero = 0;
    update(&one, 1);
    while (buffered_ != 56) update(&zero, 1);
    std::uint8_t length[8]{};
    for (int i = 0; i < 8; ++i)
        length[i] = static_cast<std::uint8_t>(bit_length >> ((7 - i) * 8));
    update(length, sizeof(length));
    std::array<std::uint8_t, 32> digest{};
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j)
            digest[i * 4 + j] = static_cast<std::uint8_t>(state_[i] >> ((3 - j) * 8));
    return digest;
}

} // namespace rawengine

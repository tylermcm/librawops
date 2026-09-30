#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rawengine {

// Internal streaming SHA-256. Call finish only once.
class Sha256 final {
public:
    void update(const void* bytes, std::size_t size);
    std::array<std::uint8_t, 32> finish();
private:
    void block(const std::uint8_t* bytes);
    std::array<std::uint32_t, 8> state_{
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t bytes_ = 0;
};

} // namespace rawengine

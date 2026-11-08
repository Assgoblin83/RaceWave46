#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// SHA-256 from FIPS 180-4. Used to authenticate the cartridge image, the
// generation inputs and the canonical form of the generated sources without
// depending on a crypto library.
namespace wr64::crypto {

class Sha256 {
public:
    Sha256();

    void update(const void* data, size_t size);
    void update(std::string_view text) { update(text.data(), text.size()); }
    // Finishes the digest. The object must not be updated afterwards.
    std::array<uint8_t, 32> finish();

    static std::array<uint8_t, 32> hash(std::span<const uint8_t> data);
    static std::array<uint8_t, 32> hash(std::string_view text);

private:
    void block(const uint8_t* chunk);

    uint32_t state_[8];
    uint64_t length_ = 0;
    uint8_t buffer_[64] = {};
    size_t buffered_ = 0;
};

std::string to_hex(std::span<const uint8_t> digest);

}  // namespace wr64::crypto

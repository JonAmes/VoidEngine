#pragma once
// Shared FNV-1a-64 (born in Phase 4 / Q5; extracted in 6.5 so stats state
// hashes and replay hashes ride the SAME primitive — "compatible with the
// replay-hash machinery" means bit-for-bit the same mixing).
//
// Raw-bit hashing is deliberate (4.7 note): -0.0 and +0.0 hash DIFFERENTLY.
// The hash is a bitwise state stamp, not a value comparison — that is what
// makes it stable across codepaths and divergent on 1 ULP.
//
// Byte order is fixed MSB-first everywhere, so hashes are independent of
// host endianness.
#include <bit>
#include <cstdint>
#include <span>

namespace ve::core {

class Fnv1a64 {
public:
    void mix_byte(std::uint8_t b) {
        m_h ^= b;
        m_h *= kFnvPrime;
    }

    void mix_u64(std::uint64_t v) {
        for (int shift = 56; shift >= 0; shift -= 8)
            mix_byte(static_cast<std::uint8_t>(v >> shift));
    }

    void mix_u32(std::uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8)
            mix_byte(static_cast<std::uint8_t>(v >> shift));
    }

    void mix_bytes(std::span<const std::byte> bs) {
        for (const auto& b : bs)
            mix_byte(std::to_integer<std::uint8_t>(b));
    }

    void mix_f32(float f) { mix_u32(std::bit_cast<std::uint32_t>(f)); }
    void mix_f64(double d) { mix_u64(std::bit_cast<std::uint64_t>(d)); }

    [[nodiscard]] std::uint64_t value() const { return m_h; }

private:
    // Primes per the published FNV-1a 64-bit spec.
    static constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
    static constexpr std::uint64_t kFnvPrime = 1099511628211ull;

    std::uint64_t m_h = kFnvOffset;
};

}  // namespace ve::core

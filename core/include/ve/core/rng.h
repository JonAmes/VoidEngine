#pragma once
// Phase 9 task 9.10 — the shared integer avalanche for seeded sim streams
// (#18d/#20d pattern: deterministic randomness keyed on world seed + entity
// + counter, never wall-clock). splitmix64 (Steele et al.) was already the
// choice in terrain.cpp/terrain_stream.cpp — same constants, promoted here
// so every sim stream uses ONE mix (the terrain copies stay local; no
// drive-by refactor). Bit-deterministic everywhere: handful of integer ops.
#include <cstdint>

namespace ve::core {

constexpr std::uint64_t splitmix64(std::uint64_t z) {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Uniform [0,1): top 53 bits / 2^53. h>>11 < 2^53 so the int->double
// conversion is exact (same recipe as the terrain sampler's kInv_2p53).
constexpr double unit01(std::uint64_t h) {
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

}  // namespace ve::core

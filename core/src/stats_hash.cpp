// Phase 6 task 6.5 — see stats_hash.h for the canonical form.
#include "ve/core/sim/stats_hash.h"

#include <bit>
#include <cstddef>

#include "ve/core/fnv.h"

namespace ve::core {

namespace {
void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::byte>(v >> shift));  // MSB-first
}

void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::byte>(v >> shift));
}
}  // namespace

void write_stats_canonical(const StatsSet& s, std::vector<std::byte>& out) {
    put_u64(out, static_cast<std::uint64_t>(s.entries().size()));
    for (const auto& e : s.entries()) {  // insertion order = state (rule 5)
        put_u64(out, static_cast<std::uint64_t>(e.key.size()));
        for (const char c : e.key)
            out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        put_u32(out, std::bit_cast<std::uint32_t>(e.value.base));
        put_u32(out, std::bit_cast<std::uint32_t>(e.value.derived));
    }
}

std::uint64_t stats_state_hash(const StatsSet& s) {
    std::vector<std::byte> bytes;
    write_stats_canonical(s, bytes);
    Fnv1a64 h;
    h.mix_bytes(bytes);
    return h.value();
}

}  // namespace ve::core

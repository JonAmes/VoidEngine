#pragma once
// Phase 6 task 6.5 — canonical byte form of a StatsSet's state + the
// state hash folded from it (same FNV-1a-64 as the replay machinery,
// shared via ve/core/fnv.h).
//
// Canonical form (fixed, order-sensitive, raw bits):
//   entry_count            u64, MSB-first
//   per entry, in INSERTION ORDER:
//     key_len              u64, MSB-first
//     key bytes            raw (opaque UTF-8; core never parses keys)
//     base bits            f32 raw bits, u32 MSB-first
//     derived bits         f32 raw bits, u32 MSB-first
//
// Reorders are NOT re-sorts: insertion order is part of the state (same
// contract as every ordered container here, AGENTS rule 5). What IS
// guaranteed — done-when (e): the hash is STABLE ACROSS RECOMPUTE ORDERS.
// recompute() only writes derived values; entry order and arithmetic walk
// order are fixed (6.4), so resolving attributes in any read order lands
// on bit-identical state. The test resolves in two different orders and
// pins identical hashes.
//
// Raw-bit policy inherited from 4.7: -0.0 and +0.0 hash differently, and
// the events bus emits that pair as a change — event stream and state
// hash never disagree about what counts as "changed".
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ve/core/sim/stats.h"

namespace ve::core {

// Append the canonical form of `s` to `out` (appends; pass an empty
// vector for the whole state, or reuse across entities for one blob).
void write_stats_canonical(const StatsSet& s, std::vector<std::byte>& out);

// FNV-1a-64 over write_stats_canonical's bytes for `s`.
[[nodiscard]] std::uint64_t stats_state_hash(const StatsSet& s);

}  // namespace ve::core

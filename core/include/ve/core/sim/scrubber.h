#pragma once
// Phase 6 task 6.15 (decision #16, Tier 1) — the replay scrubber API.
// A replay RECORD = the Phase 4 ReplayLog (per-tick command stream) plus
// periodic state CHECKPOINTS (full fixed-step sim states). The scrubber
// replays deterministically from the nearest covering checkpoint, so
// scrubbed-to-tick state hashes equal live-run hashes at the same tick —
// the same no-second-physics rule as 4.7: every step here goes through
// replay_step, nothing else.
//
// Tick conventions match the 4.7 run loop EXACTLY:
//   - log index i is the command for tick i; applying it advances the
//     post-step state of tick i-1 to the post-step state of tick i.
//   - a checkpoint cp.tick = t holds the post-step state of tick t;
//     cp.tick == -1 holds the initial (pre-step) state.
//   - hash() at cursor t == state_hash(t, state) == the live loop's hash.
// Backward scrub is cheap (re-replay from the nearest cp <= target), which
// is why there is no separate "step back" — jump_to covers both directions.

#include <cstdint>
#include <optional>
#include <vector>

#include "ve/core/math/quat.h"
#include "ve/core/math/vec.h"
#include "ve/core/sim/lod.h"
#include "ve/core/sim/replay.h"
#include "ve/core/sim/rigid.h"
#include "ve/core/sim/time.h"

namespace ve::core::sim {

// One fixed-step state snapshot (POD-cheap; the record size knob is how
// often checkpoints are taken, never what one costs).
struct sim_checkpoint {
    tick_t tick = -1;      // post-step tick; -1 = initial pre-step state
    rigid_state state{};
};

// State difference between two recorded ticks (scrubber panel inspector).
// att_delta is the rotation carrying from.att onto to.att (to = delta * from).
struct sim_state_diff {
    tick_t from = -1, to = -1;
    math::dvec3 dpos{}, dvel{}, domega{};
    math::dquat att_delta{};
    std::uint64_t hash_from = 0, hash_to = 0;
};

class ReplayScrubber {
public:
    // World + params are the scenario the record was captured under; the
    // scrubber re-derives states with them, storing no physics itself.
    ReplayScrubber(const rail_world& world, rigid_params params)
        : m_w(world), m_p(params) {}

    // Load a stored record (replaces any previous one). cp ticks must be
    // STRICTLY ascending, within [-1, log size). Rejects (keeps old record,
    // returns false) on any violation — load is all-or-nothing.
    bool load(ReplayLog log, std::vector<sim_checkpoint> cps);

    // 8.15 (#53c): load a COMPRESSED capture — the same tick-DENSE log and
    // checkpoints, plus the recorded divisor schedule (strictly ascending
    // ticks within [0, log size), divisors in [1, kLod_max_divisor]). The
    // scrubber then replays with the exact lattice the live compressed run
    // used (lod_replay_step per body phase rebuilt from sched), so scrub
    // hashes == live-compressed hashes bit-for-bit at every tick. All-
    // or-nothing like load(). An empty schedule = plain full-rate replay.
    bool load_compressed(ReplayLog log, std::vector<sim_checkpoint> cps,
                         std::vector<lod_transition> sched);

    std::size_t checkpoint_count() const { return m_cps.size(); }
    std::size_t log_size() const { return m_log.size(); }

    // --- cursor API (Tier 1) ---
    // Forward advance from the current cursor; fails closed at the log end.
    bool step_frame(std::int64_t frames = 1);
    // Land on any tick in [-1, log size): state is re-derived from the
    // nearest checkpoint with cp.tick <= target (exact cp hit = zero ops).
    bool jump_to(tick_t tick);

    bool has_cursor() const { return m_have; }
    tick_t cursor() const { return m_cursor; }             // valid if has_cursor
    const rigid_state& state() const { return m_s; }       // valid if has_cursor
    std::optional<std::uint64_t> hash() const;             // == live-loop hash

    // Extract the difference between two recorded ticks WITHOUT moving the
    // cursor (both ticks must be within range).
    std::optional<sim_state_diff> diff_between(tick_t a, tick_t b) const;

    // Total replay_step calls so far — checkpoint locality is observable
    // (a jump landing just after a cp must cost ~that gap in ops).
    std::uint64_t replay_ops() const { return m_ops; }
    void reset_replay_ops() { m_ops = 0; }

private:
    // Materialize the post-step state of tick t from the covering cp.
    std::optional<rigid_state> materialize(tick_t t) const;
    bool in_range(tick_t t) const {
        return !m_cps.empty() && m_log.size() > 0 && t >= -1 &&
               t < static_cast<tick_t>(m_log.size());
    }
    // Index of the greatest cp with cp.tick <= t (m_cps is ascending).
    std::size_t covering_cp(tick_t t) const;

    const rail_world& m_w;
    rigid_params m_p;
    ReplayLog m_log;                    // owned copy of the stored record
    std::vector<sim_checkpoint> m_cps;  // ascending (ordered container, #5)
    // 8.15 (#53c): compressed record — tick-dense log + recorded divisor
    // schedule; the cursor's incremental lattice phase (m_body/m_sched_i)
    // continues step_frame runs after a jump_to.
    bool m_compressed = false;
    std::vector<lod_transition> m_sched;
    lod_body m_body{};
    std::size_t m_sched_i = 0;
    bool m_have = false;
    tick_t m_cursor = -1;
    rigid_state m_s{};
    mutable std::uint64_t m_ops = 0;  // counted by const materialize() too
};

}  // namespace ve::core::sim

#pragma once
// Phase 4 task 4.7 (Q5) — record & replay + state hashes. The replay spine:
// a per-tick COMMAND stream (what the player/assists asked for) plus the
// integrator's determinism (4.6) reproduce state exactly; no sim state is
// stored in the log itself. FNV-1a-64 over RAW fp64 bits + tick gives a
// cheap, divergent-on-1-ULP integrity stamp per tick; hashes are the
// enforcement channel for replay/ghost-sim equality (4.10) — never floats.
//
// Raw-bit hashing is deliberate: -0.0 and +0.0 hash DIFFERENTLY, which is
// exactly why the field math returns exact +0.0 vectors (4.4 note) — the
// guard is what makes hashes stable across codepaths, not cosmetics.

#include <cstdint>
#include <vector>

#include "ve/core/math/vec.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/rigid.h"
#include "ve/core/sim/time.h"

namespace ve::core::sim {

// One tick of input for one ship (assist mask is a bit per assist; none
// exist until 4.8, but the command shape is frozen now so logs stay
// forward-compatible).
struct tick_command {
    math::dvec3 thrust_body{};   // N, ship frame
    math::dvec3 torque_body{};   // N*m, ship frame
    std::uint32_t assist_mask = 0;  // kAssist_* bits (assist.h, 4.8/4.9)
    // Autopilot waypoint when kAssist_autopilot is set (4.9): a rail body
    // id; the target position/velocity come from the rails at the current
    // tick, so the log stays tiny and the ghost sim inherits it free.
    std::int32_t autopilot_body = -1;
};

// Ordered per-tick command stream (#5: vector, indexed by tick offset).
class ReplayLog {
public:
    void append(tick_command cmd) { m_cmds.push_back(cmd); }
    std::size_t size() const { return m_cmds.size(); }
    const tick_command& at(std::size_t i) const { return m_cmds[i]; }
    // mutate a recorded command (tests: 1-ULP sensitivity canary).
    void set(std::size_t i, tick_command cmd) { m_cmds[i] = cmd; }

private:
    std::vector<tick_command> m_cmds;
};

// FNV-1a 64 over the RAW bits of (tick, pos, vel, att, omega).
std::uint64_t state_hash(tick_t tick, const rigid_state& s);

// Fold per-tick state hashes into one rolling stamp over [first, last]
// of a replay (inclusive). Returns hash of hashes: cheap "did the whole
// interval survive" check for scrubber/ghost equality (4.10).
std::uint64_t replay_interval_hash(const std::vector<std::uint64_t>& tick_hashes,
                                   std::size_t first, std::size_t last);

// Shared per-tick force/torque assembly: gravity from the rail world at
// `tick`, command thrust (ship->world), and the assist_mask-gated assist
// terms (4.8/4.9). replay_step AND ghost_preview both call THIS — it is
// the "no second physics implementation" enforcement point: preview and
// live differ only in integration rate, never in physics.
void assemble_tick_inputs(const rail_world& w, const tick_command& cmd, tick_t tick,
                          const rigid_params& p, const rigid_state& s,
                          math::dvec3& accel_world, math::dvec3& torque_body);

// Advance the ship one tick with the log's command for index i
// (thrust/torque in ship frame -> world; gravity from the rail world at
// that tick). The single replay step all replay/ghost code must share.
void replay_step(const rail_world& w, const ReplayLog& log, std::size_t i, tick_t tick,
                 const rigid_params& p, rigid_state& s);

} // namespace ve::core::sim

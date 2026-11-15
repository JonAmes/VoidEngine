#pragma once
// Phase 8 task 8.21.0 — station build plans, runtime side of #24b.
//
// A build plan is authored world-def glue (8.7 schema, 8.8 cook gates);
// this is the machinery that LANDS those steps in a live sim: a prefab
// module clamped onto a free station socket at a known tick, and the
// station's aggregate stats updated the same tick. Station defs stay plain
// loadout entities pinned to a rail (#24a) — nothing here touches rails.
//
// Two pieces:
//   1. the schedule (#20d pattern: authored timeline + seeded lane state):
//      clamp tick = first fixed tick >= at_time_s, plus a bounded delay
//      drawn deterministically from the world seed mixed with the live
//      lane congestion around the station. Pure function of (def, plan,
//      step) — no stored RNG state anywhere, so a replay recomputes the
//      identical schedule (determinism rule #5).
//   2. the landing: core::sim::Loadout is immutable after assemble, so
//      growth re-assembles a FRESH loadout (current mounts + the module
//      request) — assemble's total validation stays the runtime gate:
//      occupied socket, unknown socket, or category mismatch refuse with
//      the first violation, and the caller keeps the old loadout.
//
// Game-side (AGENTS rule 1: core never plans or names game concepts); the
// prefab attrs are already-SI numbers resolved by the host from cooked defs
// — this header stays cooklet-free like the debug panels (#16).
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ve/core/sim/loadout.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/time.h"
#include "ve/game/world_load.h"

namespace ve::game {

// Upper bound on the seeded lane-delay ([0, bound) ticks = one minute of
// congestion jitter on the canonical 1/60 s tick).
inline constexpr std::int64_t kMaxBuildDelayTicks = 3600;

// The fixed-tick clamp time for one build step (nullopt when step_idx is
// off the plan). base = the first tick whose time is >= at_time_s; the
// delay is deterministic FNV-1a-64 mixing (order fixed, MSB-first):
// seed, plan.id bytes, step_idx, congestion;
// delay = (hash >> 32) % kMaxBuildDelayTicks (high bits — FNV's low bits
// avalanche poorly). congestion = spawn rows (#39) whose node anchors on
// the body the station rails to — the lane state working the station.
[[nodiscard]] std::optional<core::sim::tick_t> build_step_clamp_tick(
    const WorldDef& system, const WorldDef& station,
    const WorldBuildPlan& plan, std::size_t step_idx);

// A cooked prefab module (itself a hull-with-interior unit, #30) distilled
// to what one station mount needs. The host resolves the plan's
// module_def_ref through the manifest/hull units; core stays concept-free.
struct prefab_module {
    std::string type_id;                        // catalog id (module hull content id)
    core::sim::socket_category category{};      // from the module's clamp-seam socket
    std::vector<core::sim::attr_pair> attrs;    // base stat values, SI (#42 applied)
    std::vector<std::string> tags;              // opaque gameplay/role tags
};

// Land one build step: assemble a FRESH station loadout = every current
// mount re-requested + the module mounted on step.target_socket. On
// failure the error is the assembler's first violation in the loadout_load
// wording ("loadout: <kind>") — the caller keeps the old loadout (growth
// is all-or-nothing per step). Catalog entries for existing mounts are
// rebuilt from their (type_id, category) pairs; the module's entry comes
// from the prefab. instance_id must be unique across the loadout.
[[nodiscard]] std::expected<core::sim::Loadout, std::string> apply_build_step(
    const core::sim::Loadout& station, const WorldBuildStep& step,
    const prefab_module& module, std::string_view instance_id);

// Station aggregate-stats update on the landing tick (done-when (e)):
// module attrs merge into the station StatsSet in attr insertion order —
// existing keys have their fp32 base += the module value (position kept),
// new keys append. All-or-nothing: any non-finite input or overflowing
// sum applies nothing and returns false.
bool merge_module_stats(core::StatsSet& stats,
                        std::span<const core::sim::attr_pair> attrs);

}  // namespace ve::game

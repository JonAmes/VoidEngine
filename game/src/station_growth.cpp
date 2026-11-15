// 8.21.0 — station build-plan scheduler + mount application (see header).
#include "ve/game/station_growth.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

#include "ve/core/fnv.h"

namespace ve::game {

namespace {

// Authored time -> first fixed tick >= it. Multiply by kSim_tick_hz and
// ceil: the * 60 is correctly rounded for def-parse time values (dyadic
// seconds scale exactly by 60), so hand-authored day-multiples (43200 s)
// land on exact tick pins (time.h's divide-by-60 discipline, #45a).
core::sim::tick_t base_tick(double at_time_s) {
    return static_cast<core::sim::tick_t>(
        std::ceil(at_time_s * core::sim::kSim_tick_hz));
}

// Lane congestion around a station (#20d "lane state"): spawn rows whose
// node anchors on the body the station rails to — waypoints by body_ref,
// market nodes by anchor_ref (both are def-ref spellings of the same
// body). Pure def data, so a replay recomputes it bit-identically.
std::uint64_t station_congestion(const WorldDef& system,
                                 const WorldDef& station) {
    const std::string& parent = station.parent_ref;
    std::uint64_t count = 0;
    for (const auto& sp : system.spawns) {
        bool anchored = false;
        for (const auto& w : system.waypoints)
            if (w.id == sp.at && w.body_ref == parent) anchored = true;
        for (const auto& m : system.markets)
            if (m.id == sp.at && m.anchor_ref == parent) anchored = true;
        if (anchored) ++count;
    }
    return count;
}

// Same wording as loadout_load.cpp's assemble error (the one door for
// "a loadout refused to build" errors).
const char* assemble_kind_name(core::sim::loadout_error e) {
    using E = core::sim::loadout_error;
    switch (e) {
        case E::duplicate_socket_id:     return "duplicate_socket_id";
        case E::unknown_component_type:  return "unknown_component_type";
        case E::unknown_socket:          return "unknown_socket";
        case E::socket_occupied:         return "socket_occupied";
        case E::category_mismatch:       return "category_mismatch";
        case E::duplicate_instance_id:   return "duplicate_instance_id";
        case E::duplicate_attr_key:      return "duplicate_attr_key";
        case E::duplicate_tag:           return "duplicate_tag";
        case E::invalid_grid:            return "invalid_grid";
        case E::invalid_hull_mass:       return "invalid_hull_mass";
    }
    return "unknown";  // unreachable: the enum is exhaustive
}

}  // namespace

std::optional<core::sim::tick_t> build_step_clamp_tick(
    const WorldDef& system, const WorldDef& station,
    const WorldBuildPlan& plan, std::size_t step_idx) {
    if (step_idx >= plan.steps.size()) return std::nullopt;
    const auto& step = plan.steps[step_idx];

    core::Fnv1a64 h;
    h.mix_u64(system.seed);
    h.mix_bytes(std::as_bytes(std::span(plan.id)));
    h.mix_u64(static_cast<std::uint64_t>(step_idx));
    h.mix_u64(station_congestion(system, station));
    // High bits: FNV-1a's low bits mix weakly (multiply-then-xor keeps
    // the low byte nearly periodic), the top 32 avalanche properly.
    const std::uint64_t delay = (h.value() >> 32) % kMaxBuildDelayTicks;
    return base_tick(step.at_time_s) + static_cast<core::sim::tick_t>(delay);
}

std::expected<core::sim::Loadout, std::string> apply_build_step(
    const core::sim::Loadout& station, const WorldBuildStep& step,
    const prefab_module& module, std::string_view instance_id) {
    // Rebuild the assemble inputs from the live loadout: sockets pass
    // through; each existing mount re-requests itself verbatim; the
    // catalog is deduped (type_id, category) pairs (mounts already carry
    // both — Loadout::assemble only uses the entries to gate requests).
    std::vector<core::sim::socket_desc> sockets = station.sockets();
    std::vector<core::sim::component_type> catalog;
    std::vector<core::sim::mount_request> mounts;
    for (const auto& mc : station.mounts()) {
        mounts.push_back(core::sim::mount_request{
            .instance_id = mc.instance_id,
            .type_id = mc.type_id,
            .socket_id = mc.socket_id,
            .attrs = mc.attrs,
            .tags = mc.tags,
        });
        if (!std::ranges::any_of(catalog, [&](const auto& ct) {
                return ct.type_id == mc.type_id;
            }))
            catalog.push_back(
                core::sim::component_type{.type_id = mc.type_id,
                                          .category = mc.category});
    }
    catalog.push_back(
        core::sim::component_type{.type_id = module.type_id,
                                  .category = module.category});
    mounts.push_back(core::sim::mount_request{
        .instance_id = std::string(instance_id),
        .type_id = module.type_id,
        .socket_id = step.target_socket,
        .attrs = module.attrs,
        .tags = module.tags,
    });

    auto grown = core::sim::Loadout::assemble(station.hull(),
                                              std::move(sockets),
                                              std::move(catalog),
                                              std::move(mounts));
    if (!grown.has_value())
        return std::unexpected(std::string("loadout: ") +
                               assemble_kind_name(grown.error()));
    return *grown;
}

bool merge_module_stats(core::StatsSet& stats,
                        std::span<const core::sim::attr_pair> attrs) {
    // All-or-nothing: validate every resulting value before writing any,
    // so a poisoned attrs list can never half-merge (the landing tick is
    // an atomic event, and stats_state_hash must never see a partial one).
    for (const auto& a : attrs) {
        if (!std::isfinite(a.value)) return false;
        const auto cur = stats.base(a.key);
        if (cur.has_value()) {
            const float merged =
                *cur + static_cast<float>(a.value);  // fp32 add, fixed order
            if (!std::isfinite(merged)) return false;
        }
    }
    for (const auto& a : attrs)
        stats.set_base(a.key,
                       static_cast<float>(a.value) +
                           stats.base(a.key).value_or(0.0f));
    return true;
}

}  // namespace ve::game

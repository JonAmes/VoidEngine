// 7.13 — airlock machine + actor-agnostic verbs. See doors.h for the
// composition contract: pump progress is chamber reservoir band facts,
// payloads never carry the actor, gates are caller-supplied tag strings.
#include "ve/core/sim/doors.h"

#include <algorithm>

namespace ve::core::sim {

namespace {

bool has_tag(const std::vector<std::string>& tags, std::string_view tag) {
    return std::ranges::find(tags, tag) != tags.end();
}

// Shared verb gate. Fixed order so scripted tests pin the FIRST failure
// reason: bounds -> facet -> suit-OK (#19d, on-foot only).
std::expected<void, verb_error> gate(const Loadout& hull, std::uint32_t mount,
                                     std::string_view action,
                                     std::string_view suit_ok_tag,
                                     const verb_actor& actor) {
    if (mount >= hull.mounts().size()) {
        return std::unexpected(verb_error::unknown_component);
    }
    if (!has_tag(hull.mounts()[mount].tags, action)) {
        return std::unexpected(verb_error::missing_action_facet);
    }
    if (actor.on_foot && !suit_ok_zone(actor.atm_rho0_kg_m3, actor.zone_tags, suit_ok_tag)) {
        return std::unexpected(verb_error::not_suit_ok);
    }
    return {};
}

}  // namespace

bool Airlock::cycle_out(EffectEventBus& bus, DoorField& doors) {
    if (m_phase != airlock_phase::sealed && m_phase != airlock_phase::open_in) {
        return false;  // never reverse mid-pump; never re-open outward
    }
    // Interlock first act: the ship-side door is shut before any venting.
    if (doors.open(m_def.inner_mount)) {
        doors.set_open(m_def.inner_mount, false);
        bus.emit(rail_kind::door_flip, m_def.inner_mount + 1, 0);
    }
    m_phase = airlock_phase::venting;
    return true;
}

bool Airlock::cycle_in(EffectEventBus& bus, DoorField& doors) {
    if (m_phase != airlock_phase::sealed && m_phase != airlock_phase::open_out) {
        return false;
    }
    if (doors.open(m_def.outer_mount)) {
        doors.set_open(m_def.outer_mount, false);
        bus.emit(rail_kind::door_flip, m_def.outer_mount + 1, 0);
    }
    m_phase = airlock_phase::pressurizing;
    return true;
}

void Airlock::step(EffectEventBus& bus, DoorField& doors, const chamber_state& chamber) {
    if (m_phase == airlock_phase::venting && chamber.vented) {
        doors.set_open(m_def.outer_mount, true);
        bus.emit(rail_kind::door_flip, m_def.outer_mount + 1, 1);
        // b=1: the chamber now opens OUTWARD. Last airlock_flip in the log
        // = current side, readable for scrubbing.
        bus.emit(rail_kind::airlock_flip, m_def.airlock_mount + 1, 1);
        m_phase = airlock_phase::open_out;
    } else if (m_phase == airlock_phase::pressurizing && chamber.pressurized) {
        doors.set_open(m_def.inner_mount, true);
        bus.emit(rail_kind::door_flip, m_def.inner_mount + 1, 1);
        bus.emit(rail_kind::airlock_flip, m_def.airlock_mount + 1, 0);
        m_phase = airlock_phase::open_in;
    }
}

bool suit_ok_zone(double atm_rho0_kg_m3, std::span<const std::string> zone_tags,
                  std::string_view suit_ok_tag) {
    if (atm_rho0_kg_m3 <= 0.0) {
        return true;  // airless world: the suit alone is enough (7.12)
    }
    return std::ranges::find(zone_tags, suit_ok_tag) != zone_tags.end();
}

std::expected<void, verb_error> toggle_door(EffectEventBus& bus, const Loadout& hull,
                                            DoorField& doors, std::uint32_t mount,
                                            std::string_view toggle_action,
                                            std::string_view suit_ok_tag,
                                            const verb_actor& actor,
                                            const airlock_def* interlocked) {
    if (auto g = gate(hull, mount, toggle_action, suit_ok_tag, actor); !g) {
        return g;
    }
    // Airlock-owned doors are actuated only by the machine — manual
    // toggling could break the never-both-open invariant.
    if (interlocked &&
        (mount == interlocked->inner_mount || mount == interlocked->outer_mount)) {
        return std::unexpected(verb_error::interlock);
    }
    const bool now_open = !doors.open(mount);
    doors.set_open(mount, now_open);
    bus.emit(rail_kind::door_flip, mount + 1, now_open ? 1 : 0);
    return {};
}

std::expected<void, verb_error> cycle_airlock(EffectEventBus& bus, const Loadout& hull,
                                              DoorField& doors, Airlock& lock, bool outward,
                                              std::string_view cycle_action,
                                              std::string_view suit_ok_tag,
                                              const verb_actor& actor) {
    if (auto g = gate(hull, lock.def().airlock_mount, cycle_action, suit_ok_tag, actor); !g) {
        return g;
    }
    const bool started = outward ? lock.cycle_out(bus, doors) : lock.cycle_in(bus, doors);
    if (!started) {
        return std::unexpected(verb_error::wrong_phase);
    }
    return {};
}

}  // namespace ve::core::sim

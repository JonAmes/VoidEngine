#pragma once
// Phase 7 task 7.13 — airlock state machine + on-foot verbs (#19a/#19d/#28).
//
// Airlock = the two-door state machine over door components (#19a). It
// consumes chamber BAND FACTS, not a fake pump timer: the airlock chamber
// is a Phase-6 section Reservoir, and the machine reads the same
// derived-tag facts the 7.11 junction rule consumed (section_state
// discipline: facts in, reservoir stays the source of truth).
//
// Verbs are actor-agnostic (#28b): the actor id never enters a payload, so
// a seated crew and an on-foot crew produce byte-identical rail streams
// and identical door-state deltas at the same tick. What DIFFERS is only
// the gate (#19d): on an atmosphere world (rails atm_rho0_kg_m3 != 0) an
// on-foot actor acts only inside a zone whose authored tags include the
// suit-OK tag. The tag string is caller-supplied cooked data — core never
// names game zones (AGENTS rule 1). Airless worlds (Phase 7 moons) are
// suit-OK anywhere, which is exactly the 7.12 walk case.
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ve/core/sim/effect_bus.h"
#include "ve/core/sim/loadout.h"

namespace ve::core::sim {

// ---- door live state -------------------------------------------------------
// Doors are loadout components; the mount index is their deterministic
// identity and bus payloads carry index + 1 (the seat-link convention:
// payload 0 stays free). Runtime state is one bit each: open or closed.
class DoorField {
public:
    explicit DoorField(std::size_t mount_count) : m_open(mount_count, 0) {}

    [[nodiscard]] bool open(std::uint32_t mount) const {
        return mount < m_open.size() && m_open[mount] != 0;
    }
    void set_open(std::uint32_t mount, bool open) {
        if (mount < m_open.size()) m_open[mount] = open ? 1 : 0;
    }

private:
    std::vector<std::uint8_t> m_open;
};

// Chamber facts derived from the chamber reservoir's band tags every tick.
// `vented` = chamber crossed its empty band; `pressurized` = chamber back
// within its nominal band. Hysteresis-free band evaluation (6.9) makes
// both land on hand-computable ticks.
struct chamber_state {
    bool vented = false;
    bool pressurized = false;
};

// Interlocked cycle: BOTH doors are never open at once. A cycle first
// closes the opposite door, and opens the target door only once the
// chamber fact says it is safe — mid-pump both doors are closed.
enum class airlock_phase : std::uint8_t {
    sealed,        // both closed (initial state)
    venting,       // closed; chamber heading for its empty band
    open_out,      // outer door open (chamber safe to space)
    pressurizing,  // closed; chamber heading for its nominal band
    open_in,       // inner door open (chamber safe to ship)
};

struct airlock_def {
    std::uint32_t airlock_mount;  // the airlock.* component (payload a)
    std::uint32_t inner_mount;    // ship-side door component
    std::uint32_t outer_mount;    // space-side door component
};

class Airlock {
public:
    explicit Airlock(const airlock_def& def) : m_def(def) {}

    [[nodiscard]] const airlock_def& def() const { return m_def; }
    [[nodiscard]] airlock_phase phase() const { return m_phase; }

    // Verb starts (gated by cycle_airlock below). Each closes the opposite
    // door as a door_flip if it was open, then enters the pump phase.
    // False = wrong_phase: no events, no state change.
    bool cycle_out(EffectEventBus& bus, DoorField& doors);  // ship -> space
    bool cycle_in(EffectEventBus& bus, DoorField& doors);   // space -> ship

    // Call every tick AFTER the caller stepped the chamber reservoir.
    // venting + chamber.vented      -> open outer,  door_flip b=1 then
    //                                  airlock_flip b=1 (space side open).
    // pressurizing + pressurized    -> open inner,  door_flip b=1 then
    //                                  airlock_flip b=0 (ship side open).
    // The airlock_flip payload makes "which side is open now" readable
    // from the log alone for scrubbing (6.6 contract).
    void step(EffectEventBus& bus, DoorField& doors, const chamber_state& chamber);

private:
    airlock_def m_def;
    airlock_phase m_phase = airlock_phase::sealed;
};

// ---- verbs ------------------------------------------------------------------
struct verb_actor {
    std::uint64_t id = 0;  // occupant/entity id — DELIBERATELY never in payloads
    bool on_foot = false;  // seated/hull-inside crew = false (their pressure
                           // problem is the 7.11 section junction, not here)
    double atm_rho0_kg_m3 = 0.0;  // world fact (7.6 rails field); 0 = airless
    std::vector<std::string> zone_tags;  // authored tags of the actor's zone
};

enum class verb_error : std::uint8_t {
    unknown_component,
    missing_action_facet,
    not_suit_ok,  // #19d gate: on foot, atmosphere, no suit-OK zone tag
    interlock,    // airlock-owned doors refuse manual toggling
    wrong_phase,
};

// #19d gate. Airless world -> the suit alone is enough anywhere (7.12).
// Atmosphere world -> only zones carrying the authored suit-OK tag pass
// (domes, pressurized rovers); open atmosphere is not EVA-OK.
bool suit_ok_zone(double atm_rho0_kg_m3, std::span<const std::string> zone_tags,
                  std::string_view suit_ok_tag);

// door.toggle verb. Gate order is fixed (first reason is what scripted
// tests pin): unknown_component -> missing_action_facet -> not_suit_ok
// (on-foot only) -> interlock. On success: flip + emit
// door_flip(a = mount + 1, b = new state). The payload never mentions the
// actor, so seated and on-foot runs are byte-identical (#28b).
std::expected<void, verb_error> toggle_door(EffectEventBus& bus, const Loadout& hull,
                                            DoorField& doors, std::uint32_t mount,
                                            std::string_view toggle_action,
                                            std::string_view suit_ok_tag,
                                            const verb_actor& actor,
                                            const airlock_def* interlocked = nullptr);

// airlock.cycle verb -> machine start (same gates; the facet is checked on
// the airlock component itself). After starting, the pump runs on sim
// time via Airlock::step — re-clicking while cycling is wrong_phase.
std::expected<void, verb_error> cycle_airlock(EffectEventBus& bus, const Loadout& hull,
                                              DoorField& doors, Airlock& lock, bool outward,
                                              std::string_view cycle_action,
                                              std::string_view suit_ok_tag,
                                              const verb_actor& actor);

}  // namespace ve::core::sim

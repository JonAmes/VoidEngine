#pragma once
// Phase 9 task 9.11 — damage pipeline core (PLAN #26):
//   hit -> shield pool drains up to its value -> the OVERFLOW lands on the
//   hit component's health pool -> at zero the component is OFFLINE:
//     * band tag on the component's OWN TagSet (the 9.10 gun_slot
//       precedent: per-component tags live with the component), and
//     * WiringReach kill_component — the SHIPPED function-zero source, so
//       engines stop thrust, guns stop firing, shields stop regen exactly
//       like a dark cascade (#26e: no new machinery; the systems already
//       read the derived attrs these modifiers zero).
// Everything here is the #19e reservoir machinery + 6.12 wiring: shield and
// health are Reservoirs, the zero-crossing is a band_dir::at_or_below band
// (set_value re-evals bands IMMEDIATELY, so a lethal hit tags the component
// without waiting for a step), the floor clamp makes health un-negative,
// and heal-above-zero un-throws the band + revives (the 9.15 round trip).
//
// Opaque keys (reservoir/gun doctrine): the core never names "shield",
// "health", or "offline" — callers hand in attr keys and the tag string.
// The game-side vocabulary is "shield.regen" / "shield.capacity" /
// "component.health" / "ship.component.offline" (attr registry 9.11).
//
// Composition note (9.10 synergy, pinned by test): zeroing a gun's
// weapon.rof through the wiring net makes gun_try_fire refuse through the
// 9.10 fail-safe (rof missing/<=0/non-finite never fires) — an offline gun
// stops firing with NO extra gate.
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ve/core/sim/bands.h"
#include "ve/core/sim/reservoir.h"
#include "ve/core/sim/tags.h"

namespace ve::core {
class StatsPipeline;  // 6.4 pipeline lives in ve::core (not sim)
}  // namespace ve::core

namespace ve::core::sim {

class WiringReach;

// One damageable component's health state. Not copy-relocatable (the
// reservoir points at the member TagSet, cf. gun_slot): store via
// unique_ptr; the sink's container is ordered (registration order) for
// deterministic lookup (AGENTS rule 5).
struct component_hp {
    std::string ref;                 // wiring-registry ref (opaque)
    TagSet tags;                     // offline band tag lands here
    Reservoir hp{0.0, {}, &tags};    // ceiling = authored max health

    component_hp() = default;
    component_hp(const component_hp&) = delete;
    component_hp& operator=(const component_hp&) = delete;
};

// One ship's damageable state: the shield pool (absorbs first, ship-wide)
// plus the component health pools. Not copyable (reservoirs borrow members).
struct damage_sink {
    TagSet ship_tags;                       // shield band tags live here
    Reservoir shield{0.0, {}, &ship_tags};  // ceiling 0 = no shield at all
    std::vector<std::unique_ptr<component_hp>> comps;  // registration order

    damage_sink() = default;
    damage_sink(const damage_sink&) = delete;
    damage_sink& operator=(const damage_sink&) = delete;
};

// Shield pool configuration. The regen supply attr is a PIPELINE attr, so
// killing the shield generator's function attr through the wiring net
// zeroes regen at the reservoir door — "shield stops regen" for free.
struct shield_setup {
    std::string regen_attr;      // supply attr (J/s); "" = never regens
    std::string capacity_attr;   // derived attr -> ceiling; missing/<=0/
                                 // non-finite = NO shield (fail-safe: a
                                 // ship with unconfigured shield absorbs
                                 // nothing; the hit lands on components)
    std::vector<reservoir_band> bands;  // e.g. {0, "shield.down", at_or_below}
};

// (Re-)arm the shield pool: rebuild the reservoir with these bands, bind
// the regen supply, ceiling + initial value = derived capacity (spawn law:
// shields come up full).
void sink_configure_shield(damage_sink& sink, StatsPipeline& pipe,
                           const shield_setup& setup);

// Register a damageable component. max_health must be finite and > 0 (a
// component with no authored health is simply never registered —
// non-damageable is the caller's choice, not a core default). ref must be
// unique. Pool spawns FULL; band {0, offline_tag, at_or_below} throws the
// tag on the component's OWN TagSet the instant health crosses zero.
// Returns false on bad input or duplicate ref.
bool sink_add_component(damage_sink& sink, std::string_view ref,
                        double max_health, std::string_view offline_tag);

// What one hit did, every number hand-computable from the inputs.
struct hit_report {
    std::string ref;
    double damage = 0.0;         // incoming J as given
    double absorbed = 0.0;       // J eaten by the shield pool
    double on_component = 0.0;   // damage - absorbed (J that reached the pool)
    double shield_after = 0.0;
    double hp_after = 0.0;       // floored at 0: health never goes negative
    bool went_offline = false;   // hp STRICTLY fell to <= 0 ON THIS HIT
};

// THE pipeline (PLAN #26, fixed order): shield drains min(damage, value)
// first, overflow hits the component pool. `wiring` (nullable) applies the
// #26e zeroing: when hp_after <= 0 the component is killed in the wiring
// net (idempotent — repeat hits cost no recompute). damage must be finite
// and >= 0; ref must name a registered component — either violation returns
// nullopt with ZERO state change. Lookup follows registration order.
std::optional<hit_report> sink_apply_hit(damage_sink& sink,
                                         WiringReach* wiring,
                                         std::string_view ref,
                                         double damage_J);

// Repair door (9.15 rides this): set health to a positive value (the
// set_value door clamps to the ceiling; the band un-throws above zero) and
// revive in the wiring net when one is given (idempotent). Unknown ref or
// non-positive/non-finite value = false, no state change.
bool sink_repair_component(damage_sink& sink, WiringReach* wiring,
                           std::string_view ref, double hp_value);

}  // namespace ve::core::sim

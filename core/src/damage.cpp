// Phase 9 task 9.11 — damage pipeline core (see damage.h). All arithmetic
// is fp64 at the reservoir door (the pools are Reservoirs); the pipeline
// attr reads are the pipeline's own fp32 derived values.
#include "ve/core/sim/damage.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "ve/core/sim/stats_pipeline.h"
#include "ve/core/sim/wiring.h"

namespace ve::core::sim {

void sink_configure_shield(damage_sink& sink, StatsPipeline& pipe,
                           const shield_setup& setup) {
    // Fail-safe ceiling: missing/invalid capacity attr = NO shield (hits
    // land straight on components). Same direction as the 9.10 rof
    // fail-safe: unconfigured protection never absorbs by accident.
    double cap = 0.0;
    if (!setup.capacity_attr.empty()) {
        const auto v = pipe.derived(setup.capacity_attr);
        if (v && std::isfinite(*v) && *v > 0.0f)
            cap = static_cast<double>(*v);
    }
    // Rebuild the pool so the authored bands attach (bands are a
    // construction input; re-arming re-evaluates them immediately). The
    // ctor takes the INITIAL value only — the capacity clamp rides
    // set_ceiling (regen/step must never overfill the shield).
    sink.shield = Reservoir(cap, setup.bands, &sink.ship_tags);
    sink.shield.set_ceiling(cap);
    sink.shield.bind(&pipe, reservoir_attrs{.supply_attr = setup.regen_attr,
                                            .load_attr = "",
                                            .leak_attr = ""});
}

bool sink_add_component(damage_sink& sink, std::string_view ref,
                        double max_health, std::string_view offline_tag) {
    if (ref.empty() || offline_tag.empty()) return false;
    if (!std::isfinite(max_health) || max_health <= 0.0) return false;
    for (const auto& c : sink.comps)
        if (c->ref == ref) return false;  // duplicate registration
    auto c = std::make_unique<component_hp>();
    c->ref = std::string(ref);
    // The zero-crossing IS the offline signal (PLAN #26): band at 0,
    // at_or_below, on the component's own TagSet. set_value re-evals
    // bands, so a lethal hit throws the tag inside the hit, not on step.
    c->hp = Reservoir(max_health,
                      {reservoir_band{0.0, std::string(offline_tag),
                                      band_dir::at_or_below}},
                      &c->tags);
    c->hp.set_ceiling(max_health);  // ctor sets value only; repairs clamp here
    sink.comps.push_back(std::move(c));
    return true;
}

std::optional<hit_report> sink_apply_hit(damage_sink& sink,
                                         WiringReach* wiring,
                                         std::string_view ref,
                                         double damage_J) {
    if (!std::isfinite(damage_J) || damage_J < 0.0) return std::nullopt;
    component_hp* hit = nullptr;
    for (auto& c : sink.comps) {           // registration order: deterministic
        if (c->ref == ref) { hit = c.get(); break; }
    }
    if (!hit) return std::nullopt;         // unknown ref: zero state change

    hit_report r;
    r.ref = std::string(ref);
    r.damage = damage_J;

    // Shield first (#26): drain up to its current value.
    r.absorbed = std::min(damage_J, sink.shield.value());
    sink.shield.set_value(sink.shield.value() - r.absorbed);
    r.on_component = damage_J - r.absorbed;

    // Overflow onto the component pool; the reservoir floor clamps at 0.
    const double before = hit->hp.value();
    hit->hp.set_value(before - r.on_component);
    r.shield_after = sink.shield.value();
    r.hp_after = hit->hp.value();
    r.went_offline = before > 0.0 && r.hp_after <= 0.0;

    // #26e zeroing: the shipped function-zero source (cf. dark cascade).
    // Called on every zeroed hit — kill_component refuses no-change ops, so
    // repeat hits cost no recompute.
    if (wiring && r.hp_after <= 0.0)
        wiring->kill_component(hit->ref);
    return r;
}

bool sink_repair_component(damage_sink& sink, WiringReach* wiring,
                           std::string_view ref, double hp_value) {
    if (hp_value <= 0.0 || !std::isfinite(hp_value)) return false;
    component_hp* hit = nullptr;
    for (auto& c : sink.comps) {
        if (c->ref == ref) { hit = c.get(); break; }
    }
    if (!hit) return false;
    // set_value clamps to the ceiling and re-evals the band: back above 0
    // un-throws the offline tag with no extra bookkeeping.
    hit->hp.set_value(hp_value);
    if (wiring) wiring->revive_component(hit->ref);  // idempotent
    return true;
}

}  // namespace ve::core::sim

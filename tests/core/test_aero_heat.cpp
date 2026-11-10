// test: aero_heat — 7.7 done-when: heat budget integrated per tick from the
// aero heat flux; hot vs survivable reentry profiles hand-computed (all
// profile numbers come from an independent Python port of this exact loop
// order); heat.tolerance is an ATTRIBUTE read via the pipeline every tick,
// so a modifier alone flips survival on an identical heat stream; over-
// tolerance damage is damage-as-data via the Phase 6 effect pipeline
// (apply_effect bundles, 6.x pattern).
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/aero.h"
#include "ve/core/sim/aero_heat.h"
#include "ve/core/sim/effects.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/rigid.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_pipeline.h"

namespace {
using namespace ve::core;
using namespace ve::core::sim;

// Hand-authored atmosphere: clean SI, H=10 km so the entry bake happens in
// the density ramp (the density *curve* itself is pinned in 7.6).
body_def world_body() {
    body_def b;
    b.surface_radius = 1'000'000.0;
    b.well_radius = 1'300'000.0;
    b.surface_g = 10.0;
    b.well_taper_start = 1'200'000.0;
    b.atm_rho0_kg_m3 = 1.0;
    b.atm_scale_height_m = 10'000.0;
    return b;
}

aero_params hull() {  // lifting body flat: pure drag this phase
    aero_params a;
    a.ref_area_m2 = 2.0;
    a.drag_coeff = 0.5;
    a.lift_coeff = 0.0;
    a.max_aoa_rad = 0.2;
    return a;
}

constexpr double kAbsorb = 1e-7;   // arcade absorption coeff (authored, opaque)
constexpr double kMass = 1'000.0;  // kg; accel carries /m by hand (7.6 pattern)

modifier_desc flat(std::string_view attr, std::string_view id, float v) {
    auto m = modifier_desc{};
    m.target_attr = std::string(attr);
    m.stack_id = std::string(id);
    m.op = mod_op::flat_add;
    m.value = v;
    m.policy = stack_policy::exclusive;
    return m;
}

struct Rig {  // per-hull stats: tolerance + integrity attrs (opaque names)
    StatsSet st;
    ModifierStack ms;
    StatsPipeline sp;
    Rig() : sp(st, ms) {
        st.set_base("heat.tolerance", 400.0f);
        st.set_base("hull.integrity", 100.0f);
    }
};

struct Run {
    std::int64_t ticks = 0;
    std::int64_t cross_tick = 0;  // 0 = never crossed tolerance
    std::int64_t over_ticks = 0;  // ticks spent over tolerance => damage ticks
    double heat = 0.0;
};

// Scripted 1-D vertical reentry: real 7.6 aero_forces + Phase 4 integrator +
// heat budget + attr tolerance + damage-as-data. Loop order IS the pin
// contract: flux/heat from CURRENT state -> over-check -> integrate.
Run reentry(double v0, Rig& rig) {
    const body_def body = world_body();
    const math::dvec3 origin{};
    rigid_state s;
    s.pos = {0.0, 1'250'000.0, 0.0};  // 250 km up, inside the 1.3e6 well
    s.vel = {0.0, -v0, 0.0};
    rigid_params p;  // default point-mass
    Run r;
    for (std::int64_t tick = 1; tick <= 20'000; ++tick) {
        r.ticks = tick;
        const auto a = aero_forces(body, origin, {}, s, hull());
        heat_budget_tick(r.heat, aero_heat_flux(a, kAbsorb));
        const float tol = *rig.sp.derived("heat.tolerance");  // attr, EVERY tick
        if (r.heat > static_cast<double>(tol)) {
            ++r.over_ticks;
            if (r.cross_tick == 0) r.cross_tick = tick;
            // damage-as-data: one effect per over-tick, -10 integrity.
            effect_desc burn;
            burn.effect_id = "heat.burn." + std::to_string(tick);
            burn.bundle = {flat("hull.integrity", "heat.burn." + std::to_string(tick), -10.0f)};
            CHECK(apply_effect(rig.ms, 100ULL + static_cast<std::uint64_t>(tick), burn) ==
                  effect_result::applied);
        }
        const math::dvec3 accel{0.0, -body.surface_g + a.force.y / kMass, 0.0};
        integrate_step(s, p, accel, {});
        if (s.pos.y <= body.surface_radius) break;  // landed
    }
    return r;
}
}  // namespace

TEST_CASE("aero_heat") {
    SUBCASE("flux math + gates (hand-computed, bit-exact)") {
        aero_result r;
        r.dyn_q = 5'000.0;   // 1/2 * 1 * 100^2 at rho=1, v=100
        r.speed_rel = 100.0;
        // absorb = 1/64 (dyadic): flux = 0.015625 * 5000 * 100 exact.
        CHECK(sim::aero_heat_flux(r, 0.015625) == 7'812.5);
        CHECK(sim::aero_heat_flux(r, 0.0) == 0.0);
        CHECK(sim::aero_heat_flux(r, -1.0) == 0.0);                        // negative coeff
        CHECK(sim::aero_heat_flux(r, std::nan("")) == 0.0);                // garbage coeff
        CHECK(sim::aero_heat_flux(aero_result{}, kAbsorb) == 0.0);         // no flow, no heat
        r.speed_rel = -1.0;
        CHECK(sim::aero_heat_flux(r, kAbsorb) == 0.0);                     // garbage speed
    }

    SUBCASE("budget tick: fixed-dt integration, poison-refusing") {
        double heat = 0.0;
        heat_budget_tick(heat, 60.0);  // 60 * (1/60) s = 1 unit, ~exact
        CHECK(heat == doctest::Approx(1.0).epsilon(1e-15));
        heat_budget_tick(heat, std::nan(""));      // refused...
        heat_budget_tick(heat, std::numeric_limits<double>::infinity());
        CHECK(heat == doctest::Approx(1.0).epsilon(1e-15));  // ...budget intact
        double big = 1.79e308;
        heat_budget_tick(big, 1.7e308);            // sum would exceed DBL_MAX
        CHECK(big == 1.79e308);                    // refused, budget stands
    }

    SUBCASE("#17: no atmosphere / outside well => heat stays zero") {
        body_def moon = world_body();
        moon.atm_rho0_kg_m3 = 0.0;  // airless moon authors none
        rigid_state s;
        s.pos = {0.0, 1'100'000.0, 0.0};
        s.vel = {0.0, -3'000.0, 0.0};
        const auto a = aero_forces(moon, {}, {}, s, hull());
        CHECK(sim::aero_heat_flux(a, kAbsorb) == 0.0);
        body_def b = world_body();
        s.pos = {0.0, 1'400'000.0, 0.0};  // outside well
        CHECK(sim::aero_heat_flux(aero_forces(b, {}, {}, s, hull()), kAbsorb) == 0.0);
    }

    SUBCASE("hot reentry burns through the budget; survivable one never does") {
        // Profiles pinned from an independent Python port of this loop.
        Rig hot_rig, surv_rig;
        const Run hot = reentry(3'000.0, hot_rig);
        CHECK(hot.ticks == 7'088);           // lands at tick 7088
        CHECK(hot.cross_tick == 4'146);      // tol 400 crossed mid-descent
        CHECK(hot.over_ticks == 2'943);
        CHECK(hot.heat == doctest::Approx(699.3269934099302).epsilon(1e-9));
        CHECK(*hot_rig.sp.derived("hull.integrity") == -29'330.0f);  // 100 - 10*2943

        const Run surv = reentry(1'200.0, surv_rig);
        CHECK(surv.ticks == 11'048);         // shallow entry: longer fall, less heat
        CHECK(surv.cross_tick == 0);
        CHECK(surv.over_ticks == 0);
        CHECK(surv.heat == doctest::Approx(321.00066064656676).epsilon(1e-9));
        CHECK(*surv_rig.sp.derived("hull.integrity") == 100.0f);  // untouched
    }

    SUBCASE("tolerance is an attr: a modifier alone flips the same heat stream") {
        // Identical v0=2000 entry, identical heat stream (449.098... final).
        Rig base_rig;
        const Run base = reentry(2'000.0, base_rig);  // tol 400: barely cooks
        CHECK(base.cross_tick == 6'096);
        CHECK(base.over_ticks == 2'766);
        CHECK(base.heat == doctest::Approx(449.0985855804102).epsilon(1e-9));
        CHECK(*base_rig.sp.derived("hull.integrity") == -27'560.0f);

        Rig mod_rig;
        mod_rig.ms.add_source(2);
        mod_rig.ms.add_modifier(2, flat("heat.tolerance", "old.hull", -150.0f));
        CHECK(*mod_rig.sp.derived("heat.tolerance") == 250.0f);  // per-modifier variation
        const Run mod = reentry(2'000.0, mod_rig);               // same stream...
        CHECK(mod.heat == doctest::Approx(449.0985855804102).epsilon(1e-9));  // ...bit-similar
        CHECK(mod.cross_tick == 5'618);                          // crosses 478 ticks sooner
        CHECK(mod.over_ticks == 3'244);
        CHECK(*mod_rig.sp.derived("hull.integrity") == -32'340.0f);
    }

    SUBCASE("determinism: heat stream run==run bit-exact") {
        Rig a, b;
        const Run ra = reentry(3'000.0, a);
        const Run rb = reentry(3'000.0, b);
        CHECK(ra.heat == rb.heat);           // bit-exact, not approximate
        CHECK(ra.cross_tick == rb.cross_tick);
        CHECK(ra.over_ticks == rb.over_ticks);
        CHECK(ra.ticks == rb.ticks);
    }
}

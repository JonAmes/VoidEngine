// Phase 6 task 6.6 — effects as atomic single-source modifier bundles and
// conditions as pure-data tag/attribute gates. Damage-style effects are
// just negative flat_adds: no combat vocabulary anywhere here.
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/effects.h"
#include "ve/core/sim/modifiers.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_pipeline.h"
#include "ve/core/sim/tags.h"

using namespace ve::core;

namespace {
modifier_desc mk(std::string_view attr, std::string_view id, mod_op op, float v,
                 stack_policy p = stack_policy::stack_n, std::uint32_t cap = 8) {
    auto m = modifier_desc{};
    m.target_attr = std::string(attr);
    m.stack_id = std::string(id);
    m.op = op;
    m.value = v;
    m.policy = p;
    m.stack_cap = cap;
    return m;
}
}  // namespace

TEST_CASE("effects_conditions") {
    SUBCASE("bundle lands as ONE source; remove drops all of it") {
        ModifierStack ms;
        effect_desc fx;
        fx.effect_id = "env.radiation";
        fx.bundle = {mk("pilot.stamina", "e.rad", mod_op::flat_add, -3.0f),
                     mk("pilot.reflex", "e.rad", mod_op::multiplier, 0.75f)};
        CHECK(apply_effect(ms, 77, fx) == effect_result::applied);
        CHECK(ms.has_source(77));
        CHECK(ms.modifier_count() == 2);
        CHECK(ms.remove_source(77));
        CHECK_FALSE(ms.has_source(77));
        CHECK(ms.modifier_count() == 0);
    }

    SUBCASE("timed effect expires as a unit at the hand-computed tick") {
        ModifierStack ms;
        effect_desc fx;
        fx.effect_id = "buff.adrenal";
        fx.expire_tick = 120;
        fx.bundle = {mk("pilot.reflex", "e.adr", mod_op::percent, 50.0f),
                     mk("pilot.stamina", "e.adr", mod_op::flat_add, 4.0f,
                        stack_policy::exclusive)};
        CHECK(apply_effect(ms, 5, fx) == effect_result::applied);
        CHECK(ms.expire_due(119) == 0);   // live while tick < expire
        CHECK(ms.modifier_count() == 2);
        CHECK(ms.expire_due(120) == 2);   // both drop together
        CHECK(ms.modifier_count() == 0);
    }

    SUBCASE("invalid bundle member: NOTHING lands, no orphan source") {
        ModifierStack ms;
        effect_desc fx;
        fx.effect_id = "bad";
        fx.bundle = {mk("a.b", "x", mod_op::flat_add, 1.0f),
                     mk("", "y", mod_op::flat_add, 2.0f)};  // empty attr
        CHECK(apply_effect(ms, 9, fx) == effect_result::invalid);
        CHECK_FALSE(ms.has_source(9));
        CHECK(ms.modifier_count() == 0);

        effect_desc empty;
        empty.effect_id = "none";
        CHECK(apply_effect(ms, 10, empty) == effect_result::invalid);
    }

    SUBCASE("policy reject rolls back the WHOLE effect") {
        ModifierStack ms;
        ms.add_source(1);
        ms.add_modifier(1, mk("a.b", "s.unique", mod_op::flat_add, 1.0f,
                              stack_policy::exclusive));
        effect_desc fx;
        fx.effect_id = "clash";
        fx.bundle = {mk("z.y", "ok", mod_op::flat_add, 5.0f),        // would add
                     mk("a.b", "s.unique", mod_op::flat_add, 9.0f,
                        stack_policy::exclusive)};                    // rejected
        CHECK(apply_effect(ms, 42, fx) == effect_result::rejected_policy);
        CHECK_FALSE(ms.has_source(42));       // rolled back fully
        CHECK(ms.modifier_count() == 1);      // only the pre-existing one
        CHECK(ms.sources()[0].mods[0].value == 1.0f);
    }

    SUBCASE("taken source id is refused atomically") {
        ModifierStack ms;
        ms.add_source(77);
        effect_desc fx;
        fx.effect_id = "e";
        fx.bundle = {mk("a.b", "x", mod_op::flat_add, 1.0f)};
        CHECK(apply_effect(ms, 77, fx) == effect_result::invalid);
        CHECK(ms.modifier_count() == 0);
    }

    SUBCASE("damage-as-data: negative flat_add stack + pipeline math") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        st.set_base("component.health", 100.0f);
        st.set_base("component.shield", 60.0f);
        ms.add_source(1);
        // hit 1: -30 to health, drains shield to 0 as -60 flat (pure data)
        effect_desc hit;
        hit.effect_id = "hit.roll1";
        hit.bundle = {mk("component.health", "dmg.roll1", mod_op::flat_add, -30.0f),
                      mk("component.shield", "dmg.roll1", mod_op::flat_add, -60.0f)};
        CHECK(apply_effect(ms, 101, hit) == effect_result::applied);
        CHECK(*p.derived("component.health") == 70.0f);
        CHECK(*p.derived("component.shield") == 0.0f);
        CHECK(p.derived("component.shield") != std::nullopt);
    }

    SUBCASE("condition: tags (ancestor), forbids, derived attribute gates") {
        StatsSet st;
        ModifierStack ms;
        StatsPipeline p(st, ms);
        st.set_base("pilot.stamina", 10.0f);
        TagSet tags;
        tags.add("pilot.stunned");
        tags.add("ship.shields.down");

        condition c;
        c.require_tags = {"pilot"};              // ancestor match via 6.2
        c.forbid_tags = {"seat.piloted"};
        c.attr_tests = {{"pilot.stamina", cmp_op::greater_equal, 5.0f}};
        CHECK(condition_met(c, &tags, &st));

        tags.add("seat.piloted");
        CHECK_FALSE(condition_met(c, &tags, &st));   // forbid answers
        tags.remove("seat.piloted");

        ms.add_source(1);
        ms.add_modifier(1, mk("pilot.stamina", "drain", mod_op::flat_add, -6.0f));
        (void)p.derived("pilot.stamina");            // derived = 4
        CHECK_FALSE(condition_met(c, &tags, &st));   // 4 >= 5 fails (DERIVED view)
        CHECK(st.base("pilot.stamina") == 10.0f);    // base untouched by the gate

        // Missing attribute = gate closed (fail-safe).
        condition miss;
        miss.attr_tests = {{"no.such.attr", cmp_op::less, 1.0f}};
        CHECK_FALSE(condition_met(miss, &tags, &st));

        // Null sections are vacuously satisfied; empty condition always true.
        CHECK(condition_met(condition{}, nullptr, nullptr));
        CHECK(condition_met(condition{}, &tags, &st));
    }

    SUBCASE("equal is a bit-exact fp32 comparison") {
        StatsSet st;
        st.set_base("k", 0.3f);
        condition c1;
        c1.attr_tests = {{"k", cmp_op::equal, 0.3f}};
        CHECK(condition_met(c1, nullptr, &st));
        condition c2;
        // binary32 gotcha: 0.1f+0.2f DOES equal 0.3f (unlike doubles) — use
        // 1 ULP off for a true bit-difference:
        c2.attr_tests = {{"k", cmp_op::equal, std::nextafter(0.3f, 0.0f)}};
        CHECK_FALSE(condition_met(c2, nullptr, &st));
    }
}

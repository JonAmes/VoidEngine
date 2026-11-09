// Phase 6 task 6.3 — the modifier stack: sources, the four stacking
// policies, tick-absolute expiry, tag queries, revision clock. Aggregation
// arithmetic itself is 6.4; this pins the data plane + policy semantics.
#include <cmath>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include "ve/core/sim/modifiers.h"

using namespace ve::core;

namespace {

modifier_desc mk(std::string attr, std::string stack_id, mod_op op, float v,
                 stack_policy pol = stack_policy::exclusive, std::uint32_t cap = 1,
                 std::optional<std::uint64_t> exp = std::nullopt,
                 std::vector<std::string> tags = {}) {
    return modifier_desc{std::move(attr), std::move(stack_id), op, v, pol, cap, exp,
                         std::move(tags)};
}

// Total ordered view of every live modifier as (source, attr, stack_id).
std::vector<std::string> view(const ModifierStack& s) {
    std::vector<std::string> out;
    for (const auto& src : s.sources())
        for (const auto& m : src.mods)
            out.push_back(std::to_string(src.id) + ":" + m.target_attr + ":" + m.stack_id +
                          ":" + std::to_string(m.value));
    return out;
}

}  // namespace

TEST_CASE("modifier_stack") {
    SUBCASE("sources: unique ids, remove drops their modifiers, unknown source rejected") {
        ModifierStack s;
        CHECK(s.add_source(1, {"seat.pilot"}));
        CHECK_FALSE(s.add_source(1));  // duplicate id
        CHECK(s.add_source(2));
        CHECK(s.source_count() == 2);
        CHECK(s.has_source(2));
        CHECK(s.add_modifier(3, mk("pilot.focus", "buff.x", mod_op::flat_add, 1.0f)) ==
              mod_add_result::source_unknown);
        CHECK(s.add_modifier(1, mk("pilot.focus", "buff.x", mod_op::flat_add, 1.0f)) ==
              mod_add_result::added);
        CHECK(s.remove_source(1));
        CHECK_FALSE(s.remove_source(1));
        CHECK(s.modifier_count() == 0);  // its modifier went with the source
    }

    SUBCASE("invalid descriptors leave the stack untouched") {
        ModifierStack s;
        s.add_source(1);
        CHECK(s.add_modifier(1, mk("", "id", mod_op::flat_add, 1.0f)) == mod_add_result::invalid);
        CHECK(s.add_modifier(1, mk("attr", "", mod_op::flat_add, 1.0f)) == mod_add_result::invalid);
        CHECK(s.add_modifier(1, mk("attr", "id", mod_op::flat_add, NAN)) == mod_add_result::invalid);
        CHECK(s.add_modifier(1, mk("attr", "id", mod_op::flat_add, 1.0f, stack_policy::stack_n, 0)) ==
              mod_add_result::invalid);
        CHECK(s.modifier_count() == 0);
    }

    SUBCASE("exclusive: first wins, later same-identity rejected (across sources)") {
        ModifierStack s;
        s.add_source(1);
        s.add_source(2);
        CHECK(s.add_modifier(1, mk("shield regen", "aura.regen", mod_op::flat_add, 5.0f)) ==
              mod_add_result::added);
        CHECK(s.add_modifier(2, mk("shield regen", "aura.regen", mod_op::multiplier, 2.0f)) ==
              mod_add_result::rejected_exclusive);
        CHECK(s.modifier_count() == 1);
        CHECK(s.sources()[0].mods[0].value == 5.0f);  // untouched
    }

    SUBCASE("stack_n: cap counts all live instances incl. the new one, cross-source") {
        ModifierStack s;
        s.add_source(1);
        s.add_source(2);
        auto burn = [](float v, std::uint32_t cap) {
            return mk("hull.health", "effect.burn", mod_op::flat_add, -v, stack_policy::stack_n, cap);
        };
        CHECK(s.add_modifier(1, burn(1.0f, 3)) == mod_add_result::added);
        CHECK(s.add_modifier(2, burn(2.0f, 3)) == mod_add_result::added);
        CHECK(s.add_modifier(1, burn(3.0f, 3)) == mod_add_result::added);
        CHECK(s.add_modifier(1, burn(4.0f, 3)) == mod_add_result::rejected_cap);
        CHECK(s.modifier_count() == 3);
        // After one instance goes away the cap reopens for a fresh one.
        CHECK(s.expire_due(10) == 0);
        s.remove_source(2);  // frees one slot
        CHECK(s.add_modifier(1, burn(9.0f, 3)) == mod_add_result::added);
        CHECK(s.modifier_count() == 3);  // colliding 2 < cap 3 -> inserted
    }

    SUBCASE("replace: drops every same-identity modifier, inserts exactly one") {
        ModifierStack s;
        s.add_source(1);
        s.add_source(2);
        s.add_modifier(1, mk("pilot.aim", "buff.aim", mod_op::percent, 10.0f, stack_policy::stack_n, 5));
        s.add_modifier(2, mk("pilot.aim", "buff.aim", mod_op::percent, 20.0f, stack_policy::stack_n, 5));
        CHECK(s.add_modifier(1, mk("pilot.aim", "buff.aim", mod_op::percent, 50.0f, stack_policy::replace)) ==
              mod_add_result::replaced);
        CHECK(s.modifier_count() == 1);
        CHECK(s.sources()[0].mods[0].value == 50.0f);
    }

    SUBCASE("refresh: values and positions kept, deadline overwritten (incl. -> permanent)") {
        ModifierStack s;
        s.add_source(1);
        s.add_source(2);
        // two different sources carrying the same stack identity:
        s.add_modifier(1, mk("hull.speed", "mod.afterburner", mod_op::multiplier, 1.5f,
                             stack_policy::stack_n, 5, 100));
        s.add_modifier(2, mk("hull.speed", "mod.afterburner", mod_op::multiplier, 2.0f,
                             stack_policy::stack_n, 5, 200));
        // Refresh from source 1 with a new deadline: existing entries keep
        // BOTH values, every deadline becomes 500.
        CHECK(s.add_modifier(1, mk("hull.speed", "mod.afterburner", mod_op::multiplier, 9.0f,
                                   stack_policy::refresh, 1, 500)) == mod_add_result::refreshed);
        CHECK(s.modifier_count() == 2);
        CHECK(s.sources()[0].mods[0].value == 1.5f);
        CHECK(s.sources()[1].mods[0].value == 2.0f);
        CHECK(s.sources()[0].mods[0].expire_tick == 500);
        CHECK(s.sources()[1].mods[0].expire_tick == 500);
        CHECK(s.expire_due(499) == 0);
        CHECK(s.expire_due(500) == 2);
        // refresh with no collision behaves like a plain insert.
        CHECK(s.add_modifier(1, mk("hull.speed", "mod.afterburner", mod_op::multiplier, 1.1f,
                                   stack_policy::refresh)) == mod_add_result::added);
        CHECK_FALSE(s.sources()[0].mods[0].expire_tick.has_value());  // permanent now
    }

    SUBCASE("expiry is tick-absolute: live while tick < expire_tick") {
        ModifierStack s;
        s.add_source(1);
        s.add_modifier(1, mk("a.attr", "x1", mod_op::flat_add, 1.0f, stack_policy::exclusive, 1, 10));
        s.add_modifier(1, mk("a.attr", "x2", mod_op::flat_add, 1.0f, stack_policy::exclusive, 1, 10));
        s.add_modifier(1, mk("a.attr", "x3", mod_op::flat_add, 1.0f));  // permanent
        CHECK(s.expire_due(9) == 0);   // not yet
        CHECK(s.expire_due(10) == 2);  // dropped exactly at T
        CHECK(s.modifier_count() == 1);
        CHECK(s.expire_due(10) == 0);  // idempotent
    }

    SUBCASE("tag queries (ancestor semantics) + dispel removal") {
        ModifierStack s;
        s.add_source(1, {"source.seat"});
        s.add_modifier(1, mk("pilot.aim", "e1", mod_op::percent, 5.0f, stack_policy::exclusive, 1,
                             std::nullopt, {"effect.buff.aim"}));
        s.add_modifier(1, mk("pilot.aim", "e2", mod_op::percent, 6.0f, stack_policy::exclusive, 1,
                             std::nullopt, {"effect.debuff.aim"}));
        CHECK(s.count_modifiers_tagged("effect") == 2);
        CHECK(s.count_modifiers_tagged("effect.buff") == 1);
        CHECK(s.count_modifiers_tagged("effect.buff.aim") == 1);
        CHECK(s.remove_modifiers_tagged("effect.debuff") == 1);
        CHECK(s.modifier_count() == 1);
        CHECK(s.source_tagged(1, "source.seat"));
        CHECK_FALSE(s.source_tagged(1, "source.gunner"));
        CHECK_FALSE(s.source_tagged(99, "source.seat"));
    }

    SUBCASE("ordered view is deterministic + revision bumps on every mutation") {
        auto build = [] {
            ModifierStack s;
            s.add_source(10);
            s.add_source(20);
            s.add_modifier(10, mk("stat.a", "s1", mod_op::flat_add, 1.0f, stack_policy::stack_n, 9));
            s.add_modifier(20, mk("stat.a", "s2", mod_op::flat_add, 2.0f, stack_policy::stack_n, 9));
            s.add_modifier(10, mk("stat.b", "s3", mod_op::multiplier, 3.0f, stack_policy::stack_n, 9));
            return s;
        };
        const ModifierStack a = build();
        const ModifierStack b = build();
        CHECK(view(a) == view(b));
        CHECK(view(a) == std::vector<std::string>{"10:stat.a:s1:1.000000", "10:stat.b:s3:3.000000",
                                                 "20:stat.a:s2:2.000000"});
        // source order (insertion) beats modifier insertion order — the
        // 6.4 pipeline and 6.5 bus will iterate exactly this.
        ModifierStack s;
        const std::uint64_t r0 = s.revision();
        s.add_source(1);
        CHECK(s.revision() == r0 + 1);
        s.add_modifier(1, mk("stat.a", "s1", mod_op::flat_add, 1.0f));
        CHECK(s.revision() == r0 + 2);
        s.expire_due(0);  // no-op drops do NOT bump
        CHECK(s.revision() == r0 + 2);
        s.remove_source(1);
        CHECK(s.revision() == r0 + 3);
    }
}

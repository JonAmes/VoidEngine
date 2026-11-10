// Phase 6 task 6.5 — canonical stats byte form + state hash. Done-when (e):
// the hash is STABLE ACROSS RECOMPUTE ORDERS (read attrs forward, reverse,
// or scrambled; the state hash lands bit-identical). The expected hash is
// recomputed here with an independent FNV loop over hand-built canonical
// bytes — pinning the byte FORMAT, not just self-consistency.
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/modifiers.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_hash.h"
#include "ve/core/sim/stats_pipeline.h"

using namespace ve::core;

namespace {

// Independent FNV-1a-64 (test-local on purpose: a shared implementation
// could not pin the format — a bug in the shared one would cancel out).
std::uint64_t ref_fnv(const std::vector<std::byte>& bs) {
    std::uint64_t h = 1469598103934665603ull;
    for (const auto& b : bs) {
        h ^= std::to_integer<std::uint8_t>(b);
        h *= 1099511628211ull;
    }
    return h;
}

void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::byte>(v >> shift));
}
void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::byte>(v >> shift));
}

modifier_desc mk(std::string_view attr, std::string_view id, mod_op op, float v) {
    auto m = modifier_desc{};
    m.target_attr = std::string(attr);
    m.stack_id = std::string(id);
    m.op = op;
    m.value = v;
    m.policy = stack_policy::stack_n;
    m.stack_cap = 16;
    return m;
}

}  // namespace

TEST_CASE("stats_hash") {
    SUBCASE("canonical form: exact byte layout (count, per-entry len/key/bits)") {
        StatsSet st;
        st.set_base("a.b", 1.5f);
        st.set_base("c", -0.25f);
        std::vector<std::byte> got;
        write_stats_canonical(st, got);

        std::vector<std::byte> want;
        put_u64(want, 2);
        put_u64(want, 3);
        for (char ch : std::string("a.b"))
            want.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
        put_u32(want, std::bit_cast<std::uint32_t>(1.5f));
        put_u32(want, std::bit_cast<std::uint32_t>(1.5f));  // pre-pipeline mirror
        put_u64(want, 1);
        want.push_back(static_cast<std::byte>(static_cast<unsigned char>('c')));
        put_u32(want, std::bit_cast<std::uint32_t>(-0.25f));
        put_u32(want, std::bit_cast<std::uint32_t>(-0.25f));
        CHECK(got == want);
        CHECK(stats_state_hash(st) == ref_fnv(want));  // hash == FNV over the form
    }

    SUBCASE("empty set has a stable, tiny canonical form") {
        StatsSet st;
        std::vector<std::byte> got;
        write_stats_canonical(st, got);
        CHECK(got.size() == 8);
        std::vector<std::byte> want;
        put_u64(want, 0);
        CHECK(got == want);
    }

    SUBCASE("hash stable across recompute ORDERS (done-when (e))") {
        auto build = [](std::vector<std::string> read_order, bool all) {
            StatsSet st;
            ModifierStack ms;
            st.set_base("ship.hull", 100.0f);
            st.set_base("ship.shield", 50.0f);
            st.set_base("pilot.reflex", 12.0f);
            st.set_base("crew.morale", 7.0f);
            ms.add_source(1);
            ms.add_source(2);
            ms.add_modifier(1, mk("ship.hull", "armor", mod_op::percent, 25.0f));
            ms.add_modifier(2, mk("ship.shield", "cap", mod_op::flat_add, 10.0f));
            ms.add_modifier(2, mk("pilot.reflex", "coffee", mod_op::multiplier, 2.0f));
            ms.add_modifier(1, mk("crew.morale", "anthem", mod_op::flat_add, 1.0f));
            StatsPipeline p(st, ms);
            if (all) {
                p.recompute_all();
            } else {
                for (const auto& k : read_order)
                    (void)p.derived(k);
            }
            return stats_state_hash(st);
        };
        const std::uint64_t forward = build({"ship.hull", "ship.shield", "pilot.reflex", "crew.morale"}, false);
        const std::uint64_t reverse = build({"crew.morale", "pilot.reflex", "ship.shield", "ship.hull"}, false);
        const std::uint64_t scrambled = build({"pilot.reflex", "ship.hull", "crew.morale", "ship.shield"}, false);
        const std::uint64_t via_all = build({}, true);
        CHECK(forward == reverse);
        CHECK(forward == scrambled);
        CHECK(forward == via_all);

        // A second identical entity hashes identically (no per-entity drift).
        CHECK(build({"ship.hull", "ship.shield", "pilot.reflex", "crew.morale"}, false) == forward);
    }

    SUBCASE("insertion order IS state: same attrs, different order -> different hash") {
        StatsSet a, b;
        a.set_base("x.y", 1.0f);
        a.set_base("z", 2.0f);
        b.set_base("z", 2.0f);
        b.set_base("x.y", 1.0f);
        CHECK(stats_state_hash(a) != stats_state_hash(b));
    }

    SUBCASE("1-ULP sensitivity and -0.0 vs +0.0 (raw-bit policy)") {
        StatsSet a, b, c;
        a.set_base("k", 1.0f);
        b.set_base("k", std::nextafter(1.0f, 2.0f));
        c.set_base("k", 1.0f);
        CHECK(stats_state_hash(a) != stats_state_hash(b));
        CHECK(stats_state_hash(a) == stats_state_hash(c));  // determinism

        StatsSet z1, z2;
        z1.set_base("k", 0.0f);
        z2.set_base("k", -0.0f);
        CHECK(stats_state_hash(z1) != stats_state_hash(z2));  // matches 4.7 policy
    }

    SUBCASE("derived recomputes change the hash; clean recompute does not") {
        StatsSet st;
        ModifierStack ms;
        st.set_base("a.b", 10.0f);
        const std::uint64_t before = stats_state_hash(st);
        ms.add_source(1);
        ms.add_modifier(1, mk("a.b", "f", mod_op::flat_add, 5.0f));
        StatsPipeline p(st, ms);
        (void)p.derived("a.b");
        const std::uint64_t after = stats_state_hash(st);
        CHECK(before != after);
        p.recompute_all();  // no dirty: stamps clean, no write
        CHECK(stats_state_hash(st) == after);
    }
}

// Phase 6 task 6.13 — market stock, price, transactions (done-when (g) pt 1).
// The market is pure COMPOSITION: stock = the 6.9 reservoir; lane traffic +
// production = modifiers on the supply attr; player transactions = ONE-TICK
// pulse effects through the 6.6 effect machinery (a buy is a load pulse of
// qty/dt; a sell is a supply pulse — the reservoir integrates it that very
// tick); price is NEVER stored state — every tick the glue re-derives
// curve(stock) into the price base attr, and the pipeline applies seat
// discounts (the #18 trader seat's "price modifier on that stack").
//
// All rates binary-exact so crossings land hand-computed:
//   flow +30/s production +30/s lane = +60/s = +1.0/tick; stock 128.
//   buy 16  @ tick 100: load pulse 960/s for one tick -> delta -15 -> 213.
//   sell 64 @ tick 140: supply pulse 3840/s -> delta +65 -> 317.
//   curve (0,10)(128,2)(256,1): dyadic breakpoints -> dyadic prices.
//   band ore.plentiful >= 256 flips EXACTLY at tick 140.
#include <bit>
#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ve/core/sim/curve.h>
#include <ve/core/sim/effects.h>
#include <ve/core/sim/modifiers.h>
#include <ve/core/sim/reservoir.h>
#include <ve/core/sim/stats.h>
#include <ve/core/sim/stats_pipeline.h>
#include <ve/core/sim/tags.h>
#include <ve/core/sim/time.h>

namespace {

using namespace ve::core;
using namespace ve::core::sim;

modifier_desc flat(std::string_view attr, std::string_view stack, float v,
                   std::uint32_t cap = 64) {
    modifier_desc m;
    m.target_attr = std::string(attr);
    m.stack_id = std::string(stack);
    m.op = mod_op::flat_add;
    m.value = v;
    m.policy = stack_policy::stack_n;
    m.stack_cap = cap;
    return m;
}

struct market_run {
    StatsSet st;
    ModifierStack ms;
    StatsPipeline p;
    TagSet tags;
    std::vector<reservoir_band> bands = {
        {256.0, "ore.plentiful", band_dir::at_or_above},
    };
    Reservoir stock;
    piecewise_curve price_curve;
    // per-tick post-step records:
    std::vector<double> level;
    std::vector<double> price_raw;   // curve output
    std::vector<float> price_net;    // after pipeline (seat discounts)
    std::vector<bool> plentiful;

    market_run()
        : p(st, ms), stock(128.0, bands, &tags),
          price_curve({{0.0, 10.0}, {128.0, 2.0}, {256.0, 1.0}}) {
        // Flow attrs exist at zero base (derived on an authored-away attr
        // fails closed; the 6.9 pattern), flows ride modifiers on top.
        st.set_base("ore.supply", 0.0f);
        st.set_base("ore.load", 0.0f);
    }
};

market_run run_market(std::int64_t ticks, bool trader_seat, bool transactions) {
    market_run r;
    // Authored flows: +30/s production, +30/s NPC lane traffic.
    r.ms.add_source(1);
    r.ms.add_modifier(1, flat("ore.supply", "prod", 30.0f));
    r.ms.add_source(2);
    r.ms.add_modifier(2, flat("ore.supply", "lane", 30.0f));
    r.stock.bind(&r.p, {"ore.supply", "ore.load", ""});  // no leak attr

    std::uint64_t next_fx = 100;
    for (std::int64_t t = 0; t < ticks; ++t) {
        r.ms.expire_due(static_cast<tick_t>(t));
        if (trader_seat && t == 50) {
            // The #18 trader seat rides the pipeline: -50% on the price attr.
            r.ms.add_source(3);
            modifier_desc disc;
            disc.target_attr = "ore.price";
            disc.stack_id = "seat.discount";
            disc.op = mod_op::percent;
            disc.value = -50.0f;
            disc.policy = stack_policy::exclusive;
            r.ms.add_modifier(3, disc);
        }
        if (transactions && t == 100) {  // player BUYS 16: one-tick load pulse
            effect_desc fx;
            fx.effect_id = "tx.buy";
            fx.expire_tick = static_cast<std::uint64_t>(t + 1);
            fx.bundle = {flat("ore.load", "tx", 960.0f, 1)};  // 16 / kSim_dt
            REQUIRE(apply_effect(r.ms, next_fx++, fx) == effect_result::applied);
        }
        if (transactions && t == 140) {  // player SELLS 64: one-tick supply pulse
            effect_desc fx;
            fx.effect_id = "tx.sell";
            fx.expire_tick = static_cast<std::uint64_t>(t + 1);
            fx.bundle = {flat("ore.supply", "tx", 3840.0f, 1)};  // 64 / kSim_dt
            REQUIRE(apply_effect(r.ms, next_fx++, fx) == effect_result::applied);
        }
        r.stock.step();
        // Price is derived at the tick of use, never stored ahead:
        const double raw = *r.price_curve.evaluate(r.stock.value());
        r.st.set_base("ore.price", static_cast<float>(raw));
        r.level.push_back(r.stock.value());
        r.price_raw.push_back(raw);
        r.price_net.push_back(*r.p.derived("ore.price"));
        r.plentiful.push_back(r.tags.contains("ore.plentiful"));
    }
    return r;
}

}  // namespace

TEST_CASE("market_price") {
    // --- plain market: flows + transactions, no seat discount --------------
    const auto m = run_market(160, false, true);

    // Stock landings are hand-computed, checked EXACT (==, no Approx).
    // level[t] = state after (t+1) steps: 128 + (t+1) while flow is pure.
    CHECK(m.level[49] == 178.0);      // 128 + 50 ticks
    CHECK(m.level[99] == 228.0);      // still pure +1/tick
    CHECK(m.level[100] == 213.0);     // buy 16: +1 - 16 in the same tick
    CHECK(m.level[101] == 214.0);     // pulse was one tick only
    CHECK(m.level[139] == 252.0);
    CHECK(m.level[140] == 317.0);     // sell 64: +1 + 64 in the same tick
    CHECK(m.level[141] == 318.0);
    CHECK(m.level[159] == 336.0);

    // Prices = authored curve over stock, re-derived every tick, dyadic:
    CHECK(m.price_raw[49] == 1.609375);    // 2 - 50/128
    CHECK(m.price_raw[99] == 1.21875);     // 2 - 100/128
    CHECK(m.price_raw[100] == 1.3359375);  // buy dip: 2 - 85/128
    CHECK(m.price_raw[139] == 1.03125);    // 2 - 124/128 (just under tail)
    CHECK(m.price_raw[140] == 1.0);        // sell spike past 256: clamped tail
    CHECK(m.price_raw[159] == 1.0);

    // No seat -> derived price is bit-identical to the raw curve value.
    for (std::size_t t = 0; t < m.level.size(); ++t)
        CHECK(std::bit_cast<std::uint32_t>(m.price_net[t]) ==
              std::bit_cast<std::uint32_t>(static_cast<float>(m.price_raw[t])));

    // Scarcity band flips at the hand-computed tick, hysteresis-free.
    CHECK_FALSE(m.plentiful[139]);  // 252 < 256
    CHECK(m.plentiful[140]);        // 317 >= 256: EXACT flip tick
    CHECK(m.plentiful[159]);

    // --- trader seat: the discount is a pipeline modifier, not a new path --
    const auto d = run_market(160, true, true);
    CHECK(std::bit_cast<std::uint32_t>(d.price_net[49]) ==
          std::bit_cast<std::uint32_t>(static_cast<float>(d.price_raw[49])));  // pre-seat
    CHECK(d.price_net[50] == 0.80078125f);  // 1.6015625 * (1 - 50/100), exact
    CHECK(d.price_net[100] == 0.66796875f);  // 1.3359375 / 2
    CHECK(d.price_net[159] == 0.5f);         // tail 1.0 / 2
    // Seat never touches the stock itself — flows are seat-free.
    CHECK(d.level[140] == 317.0);

    // --- determinism: same script twice, bit-identical everything ----------
    const auto m2 = run_market(160, true, true);
    REQUIRE(m2.level.size() == d.level.size());
    for (std::size_t t = 0; t < d.level.size(); ++t) {
        CHECK(std::bit_cast<std::uint64_t>(m2.level[t]) ==
              std::bit_cast<std::uint64_t>(d.level[t]));
        CHECK(std::bit_cast<std::uint32_t>(m2.price_net[t]) ==
              std::bit_cast<std::uint32_t>(d.price_net[t]));
        CHECK(m2.plentiful[t] == d.plentiful[t]);
    }

    // --- curve validity is all-or-nothing (authored data, validated) -------
    piecewise_curve bad;
    CHECK_FALSE(bad.set_points({{1.0, 1.0}}));                         // < 2 points
    CHECK_FALSE(bad.set_points({{0.0, 1.0}, {0.0, 2.0}}));             // equal x
    CHECK_FALSE(bad.set_points({{5.0, 1.0}, {1.0, 2.0}}));             // descending
    CHECK_FALSE(bad.evaluate(3.0).has_value());                        // still invalid
    REQUIRE(bad.set_points({{-1.0, 4.0}, {1.0, 6.0}}));
    CHECK(*bad.evaluate(-2.0) == 4.0);   // low tail clamps
    CHECK(*bad.evaluate(9.0) == 6.0);    // high tail clamps
    CHECK(*bad.evaluate(0.0) == 5.0);    // midpoint exact
}

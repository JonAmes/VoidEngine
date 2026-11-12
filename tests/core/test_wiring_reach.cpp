// test: wiring_reach — 6.12: power/signal flow = BFS over the hull def's
// authored wire network. Part 1 pins that load_hull_loadout now CARRIES the
// DEF0 wire table (was skipped); part 2 pins WiringReach behaviour on a
// hand-authored net: exact cascading dark sets (done-when (e) machinery),
// recompute-only-on-event, dark-not-dead bit-exact round-trips, mounted-vs-
// dark distinction, socket relays, and both-power-and-signal wire types.
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/cooked.h"
#include "ve/core/sim/loadout_load.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/stats_pipeline.h"
#include "ve/core/sim/wiring.h"

namespace {

using ve::core::cooked::Bytes;
using ve::core::cooked::Unit;
using namespace ve::core;
using namespace ve::core::sim;

// ---- local little-endian encoder (same fixture as loadout_load) ----
void pu8(Bytes& b, std::uint8_t v) { b.push_back(v); }
void pu32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void pi32(Bytes& b, std::int32_t v) { pu32(b, static_cast<std::uint32_t>(v)); }
void pf64(Bytes& b, double d) {
    std::uint64_t v = 0;
    static_assert(sizeof d == 8);
    std::memcpy(&v, &d, 8);
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void pstr(Bytes& b, const std::string& s) {
    pu32(b, static_cast<std::uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

Bytes sock_payload() {
    Bytes b;
    pstr(b, "test_ship.glb");
    pu32(b, 2);
    pu32(b, 3);
    pstr(b, "hp_engine"); pu8(b, 0); pu8(b, 0);
    pf64(b, 2.0); pf64(b, 0.0); pf64(b, -2.0);
    pf64(b, 0); pf64(b, 0); pf64(b, 0); pf64(b, 1);
    pstr(b, "hp_seat"); pu8(b, 0); pu8(b, 3);
    pf64(b, 0.0); pf64(b, 0.5); pf64(b, 0.0);
    pf64(b, 0); pf64(b, 0); pf64(b, 0); pf64(b, 1);
    pstr(b, "door_1"); pu8(b, 1); pu8(b, 3);
    pf64(b, 0.0); pf64(b, 0.0); pf64(b, 2.0);
    pf64(b, 0); pf64(b, 0.7071067811865476); pf64(b, 0); pf64(b, 0.7071067811865476);
    return b;
}

// DEF0 v2 with ONE authored wire: w1 power (cap 100) eng_1 -> socket hp_engine.
Bytes def_payload() {
    Bytes b;
    pu32(b, 2);
    pstr(b, "class.light_freighter");
    pu8(b, 1); pf64(b, 1000.0);
    pu8(b, 1); pf64(b, 0.0); pf64(b, 0.5); pf64(b, -0.5);
    pu8(b, 1); pf64(b, 4200.0); pf64(b, 9800.0); pf64(b, 11200.0);
    pf64(b, 2.0); pu32(b, 4); pf64(b, 2.2); pf64(b, 0.1);
    pu32(b, 1);
    pstr(b, "A"); pu32(b, 1);
    for (int v : {-1, -1, -1, 1, 1, 1}) pi32(b, v);
    pu32(b, 2);
    pstr(b, "eng_1"); pstr(b, "thruster.main"); pu8(b, 0);
    pstr(b, "hp_engine"); pu8(b, 0);
    pu32(b, 0);
    pu32(b, 1); pstr(b, "thrust.max_N"); pf64(b, 5000.0);
    pu32(b, 1);
    pstr(b, "operate.station"); pf64(b, 2.0); pu8(b, 0); pu8(b, 0);
    pstr(b, "door_1"); pstr(b, "door.hatch"); pu8(b, 3);
    pstr(b, "door_1"); pu8(b, 5);
    pu32(b, 2);
    pstr(b, "use.door"); pstr(b, "station.engineer");
    pu32(b, 0);
    pu32(b, 1);
    pstr(b, "door.toggle"); pf64(b, 2.0); pu8(b, 1); pu8(b, 0);
    pu32(b, 2);
    pstr(b, "thruster.main"); pu8(b, 0);
    pstr(b, "door.hatch"); pu8(b, 3);
    pu32(b, 1);  // wires: w1 power, eng_1 -> hp_engine
    pstr(b, "w1"); pu8(b, 0); pu8(b, 1); pf64(b, 100.0);
    pu8(b, 1); pstr(b, "eng_1"); pu8(b, 0); pstr(b, "hp_engine");
    pu32(b, 0);
    pu32(b, 1); pstr(b, "../samples/test_ship.glb");
    return b;
}

Unit mk_unit(std::uint32_t type, Bytes payload) {
    Unit u;
    u.id = "defs/test_ship.def.toml";
    u.type = type;
    u.payload = std::move(payload);
    return u;
}

wire_entry wire(std::string id, wire_kind k,
                std::uint8_t fk, std::string fref,
                std::uint8_t tk, std::string tref) {
    wire_entry w;
    w.id = std::move(id);
    w.kind = k;
    w.from = {fk, std::move(fref)};
    w.to = {tk, std::move(tref)};
    return w;
}
constexpr std::uint8_t kSocket = 0, kComp = 1;

}  // namespace

TEST_CASE("wiring_reach") {
    SUBCASE("loader CARRIES the DEF0 wire table (was skipped)") {
        auto r = load_hull_loadout(mk_unit(cooked::kUnitDef0, def_payload()),
                                   mk_unit(cooked::kUnitSock, sock_payload()),
                                   0xDEADBEEFCAFEULL);
        REQUIRE_MESSAGE(r.has_value(), r.error());
        REQUIRE(r->wires.size() == 1);
        const auto& w = r->wires[0];
        CHECK(w.id == "w1");
        CHECK(w.kind == wire_kind::power);
        REQUIRE(w.capacity.has_value());
        CHECK(*w.capacity == 100.0);  // verbatim, no reinterpretation
        CHECK(w.from.kind == kComp);
        CHECK(w.from.ref == "eng_1");
        CHECK(w.to.kind == kSocket);
        CHECK(w.to.ref == "hp_engine");
    }

    SUBCASE("BFS net: exact cascading dark sets, events-only recompute") {
        StatsSet st;
        st.set_base("eng.fn", 10.0f);
        st.set_base("eng.draw", 3.0f);
        st.set_base("sh.fn", 5.0f);
        st.set_base("ls.fn", 2.0f);
        st.set_base("guns.fn", 7.0f);
        ModifierStack ms;
        StatsPipeline sp(st, ms);

        std::vector<wire_entry> wires = {
            wire("w_r_j", wire_kind::power, kComp, "reactor", kComp, "junction"),
            wire("w_j_e", wire_kind::power, kComp, "junction", kComp, "engines"),
            wire("w_j_s", wire_kind::power, kComp, "junction", kComp, "shields"),
            wire("w_r_b", wire_kind::power, kComp, "reactor", kSocket, "bus_a"),
            wire("w_b_l", wire_kind::power, kSocket, "bus_a", kComp, "life_support"),
            wire("w_r_f", wire_kind::power_signal, kComp, "reactor", kComp, "fire_control"),
            wire("w_f_g", wire_kind::signal, kComp, "fire_control", kComp, "guns"),
        };
        std::vector<wired_component> comps = {
            {"reactor", "", "", false, false, true, false},
            {"junction", "", "", false, false, false, false},  // relay
            {"engines", "eng.fn", "eng.draw", true, false, false, false},
            {"shields", "sh.fn", "", true, false, false, false},
            {"life_support", "ls.fn", "", true, false, false, false},
            {"fire_control", "", "", true, false, false, true},  // signal src
            {"guns", "guns.fn", "", false, true, false, false},
        };
        WiringReach wr(sp, ms, std::move(wires), std::move(comps));

        // Construction = one recompute; nothing mounted -> nothing lit,
        // unmounted sources do not flood.
        CHECK(wr.recompute_count() == 1);
        CHECK(wr.dark_set().empty());
        CHECK_FALSE(wr.lit("engines"));
        CHECK_FALSE(wr.power_reach("reactor"));

        for (const char* ref : {"reactor", "junction", "engines", "shields",
                                "life_support", "fire_control", "guns"})
            REQUIRE(wr.mount(ref));
        CHECK(wr.recompute_count() == 8);  // one per event; ctor included

        // Full net lit. power_signal wire carries power (FC's ONLY power
        // feed is w_r_f) AND signal reach spans FC->guns.
        for (const char* ref : {"engines", "shields", "life_support",
                                "fire_control", "guns"})
            CHECK(wr.lit(ref));
        CHECK(wr.power_reach("fire_control"));
        CHECK(wr.signal_reach("guns"));
        CHECK_FALSE(wr.signal_reach("engines"));  // no signal path to engines

        // Clean reads NEVER re-walk (contract: recompute only on events).
        for (int t = 0; t < 600; ++t)
            (void)wr.dark_set(), (void)wr.lit("engines");
        CHECK(wr.recompute_count() == 8);

        const std::uint32_t fn10 = std::bit_cast<std::uint32_t>(10.0f);
        const std::uint32_t draw3 = std::bit_cast<std::uint32_t>(3.0f);
        const std::uint32_t zbits = std::bit_cast<std::uint32_t>(0.0f);
        CHECK(std::bit_cast<std::uint32_t>(*sp.derived("eng.fn")) == fn10);

        // Killed junction: EXACT dark set in registration order. LS is fed
        // through a SOCKET relay (w_r_b -> bus_a -> w_b_l): unaffected.
        // Signal flood is independent of power: guns stays lit.
        REQUIRE(wr.kill_component("junction"));
        CHECK(wr.recompute_count() == 9);
        const auto ds = wr.dark_set();
        REQUIRE(ds.size() == 2);
        CHECK(ds[0] == "engines");
        CHECK(ds[1] == "shields");
        CHECK(wr.lit("life_support"));
        CHECK(wr.lit("fire_control"));
        CHECK(wr.lit("guns"));
        CHECK(wr.lit("junction"));  // relay has no gates: dark NOT dead here
        // Dark-not-dead: function AND draw read ×0 bits.
        CHECK(std::bit_cast<std::uint32_t>(*sp.derived("eng.fn")) == zbits);
        CHECK(std::bit_cast<std::uint32_t>(*sp.derived("eng.draw")) == zbits);

        // Repaired: aggregates return to baseline BIT-exactly.
        REQUIRE(wr.revive_component("junction"));
        CHECK(wr.dark_set().empty());
        CHECK(std::bit_cast<std::uint32_t>(*sp.derived("eng.fn")) == fn10);
        CHECK(std::bit_cast<std::uint32_t>(*sp.derived("eng.draw")) == draw3);
        CHECK(wr.recompute_count() == 10);

        // Sever the LS bus leg: exactly {life_support} (socket chain broken).
        REQUIRE(wr.sever_wire("w_b_l"));
        const auto ds2 = wr.dark_set();
        REQUIRE(ds2.size() == 1);
        CHECK(ds2[0] == "life_support");
        REQUIRE(wr.repair_wire("w_b_l"));
        CHECK(wr.dark_set().empty());

        // Sever the signal leg: exactly {guns} (gate_signal).
        REQUIRE(wr.sever_wire("w_f_g"));
        const auto ds3 = wr.dark_set();
        REQUIRE(ds3.size() == 1);
        CHECK(ds3[0] == "guns");
        REQUIRE(wr.repair_wire("w_f_g"));

        // Mounted vs dark: an UNMOUNTED component is absent, not dark, and
        // holds no source.
        REQUIRE(wr.kill_component("junction"));
        REQUIRE(wr.unmount("engines"));
        const auto ds4 = wr.dark_set();
        REQUIRE(ds4.size() == 1);
        CHECK(ds4[0] == "shields");
        CHECK_FALSE(wr.mounted("engines"));
        REQUIRE(wr.mount("engines"));  // back into the SAME net -> dark again
        const auto ds5 = wr.dark_set();
        REQUIRE(ds5.size() == 2);
        CHECK(ds5[0] == "engines");
        CHECK(ds5[1] == "shields");

        // Source down: every gated consumer dark in registration order.
        // fire_control is itself dark but STILL EMITS signal (sources emit
        // while mounted+alive — documented Phase 6 simplification), so guns
        // (gate_signal only) stays lit.
        REQUIRE(wr.unmount("reactor"));
        const auto ds6 = wr.dark_set();
        REQUIRE(ds6.size() == 4);
        CHECK(ds6[0] == "engines");
        CHECK(ds6[1] == "shields");
        CHECK(ds6[2] == "life_support");
        CHECK(ds6[3] == "fire_control");
        CHECK(wr.lit("guns"));
        REQUIRE(wr.revive_component("junction"));
        REQUIRE(wr.mount("reactor"));
        CHECK(wr.dark_set().empty());

        // Unknown refs / no-op events: refused, NO recompute.
        const auto events = wr.recompute_count();
        CHECK_FALSE(wr.mount("nonexistent"));
        CHECK_FALSE(wr.sever_wire("no_such_wire"));
        CHECK_FALSE(wr.revive_component("engines"));  // not killed
        CHECK_FALSE(wr.unmount("nonexistent"));
        CHECK(wr.recompute_count() == events);        // refusals never re-walk

        // Kill a consumer outright (destroyed, not merely unpowered): the
        // flood excludes the killed node, its gates fail -> dark-not-dead
        // zeroes its function+draw; a killed relay also stops relaying.
        REQUIRE(wr.kill_component("engines"));
        CHECK_FALSE(wr.power_reach("engines"));  // killed node excluded
        const auto ds7 = wr.dark_set();
        REQUIRE(ds7.size() == 1);
        CHECK(ds7[0] == "engines");
        CHECK(std::bit_cast<std::uint32_t>(*sp.derived("eng.fn")) == zbits);
        REQUIRE(wr.revive_component("engines"));
        CHECK(wr.lit("engines"));
        CHECK(std::bit_cast<std::uint32_t>(*sp.derived("eng.fn")) == fn10);
    }
}

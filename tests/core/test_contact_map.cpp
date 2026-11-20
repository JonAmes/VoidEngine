// test: contact_measurement_seeded + contact_class_bands +
// contact_ghosts_never_targetable — 9.16 contact map (PLAN #31b–d).
//
// contact_measurement_seeded: a 60-tick measurement sequence replays
// BIT-IDENTICAL (positions, uncertainty, stamps) — the keyed stream
// (seed, tick, sensor, target) pins to a Python hand-port of the exact C++
// expression grouping (same splitmix64 constants as gun.cpp's spread
// chain); uncertainty == slant * cone closed-form bit-exact (grows 9x with
// distance — closed-form pins, plus strict ordering), the jitter always
// stays inside the authored cone (containment bound), assist_gain 0.5
// halves the uncertainty closed-form at the same 9000 m range, assist 0 is
// the EXACT passthrough (== on true position), tick-keyed streams diverge
// between consecutive ticks (contrast the gun's shot-keyed stream),
// seed and sensor id both re-key the draw, and fail-safes yield NOTHING:
// out of range, range <= 0, or non-finite res produce no record at all.
//
// contact_class_bands: with res 1 arcmin (cone = deg_to_rad(1/60), shared
// constexpr arithmetic) and hull 50 m, the class boundary is
// d* = 50/cone = 171887.338... m — hand-computed: resolves at 171800,
// unknown ring at 171900; assist 0.5 doubles the boundary (171900 resolves
// under lock); IFF flips at the authored 40000 m (<= law, exact at the
// boundary); hull_size 0 never classes; a perfect (res 0) sensor classes
// anything and measures EXACT.
//
// contact_ghosts_never_targetable: sensor feed dark after tick 99 -> the
// record FREEZES (bit-identical measured/uncertainty/stamp for its whole
// lifetime even while the real target moves closer), contact_targetable is
// false at every dark tick, ghost_ticks = 300 keeps it visible exactly
// through tick 399 (age 300) and prunes at 400 (age 301); a live re-feed
// makes it targetable again; ghost_ticks = 0 drops the record on the first
// missed tick.
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <doctest/doctest.h>

#include <ve/core/math/base.h>
#include <ve/core/sim/contact_map.h>

namespace {
using namespace ve::core;
using namespace ve::core::sim;
using namespace ve::core::math;

constexpr std::uint64_t kSeed = 0xC0FFEEull;
constexpr std::uint64_t kSeedB = 0xBADC0DEull;

// Same arithmetic chain as contact_map.cpp's arcmin_to_rad (deg_to_rad is
// a shared constexpr; the 1/60 division is the first rounding).
double cone_arcmin1() { return deg_to_rad(1.0 / 60.0); }

contact_target make_target(std::int64_t id, math::dvec3 pos, double hull,
                           std::string tag = "class.fighter",
                           std::uint64_t owner = 7, double gain = 1.0) {
    return contact_target{id, pos, hull, std::move(tag), owner, gain};
}

sensor_view base_sensor(std::int32_t id = 3) {
    return sensor_view{id, math::dvec3{0.0, 0.0, 0.0}, 20000.0, 1.0, 40000.0};
}
}  // namespace

TEST_CASE("contact_measurement_seeded") {
    const double cone = cone_arcmin1();
    sensor_view s = base_sensor();

    // --- 60-tick moving sequence replays BIT-IDENTICAL + uncertainty
    // closed-form + cone containment every tick.
    auto drive = [&](std::uint64_t seed, std::vector<contact_record>& out) {
        contact_board board;
        board.ghost_ticks = 300;
        for (tick_t t = 0; t < 60; ++t) {
            std::vector<contact_target> tg{make_target(
                11, math::dvec3{5000.0 + 100.0 * static_cast<double>(t), 0.0, 0.0}, 50.0)};
            contact_measure_tick(board, std::span<const sensor_view>(&s, 1),
                                 std::span<const contact_target>{tg}, t, seed);
            REQUIRE(board.find(11));
            out.push_back(*board.find(11));
        }
    };
    std::vector<contact_record> a, b;
    drive(kSeed, a);
    drive(kSeed, b);
    REQUIRE(a.size() == 60);
    for (std::size_t t = 0; t < 60; ++t) {
        CHECK(a[t].measured.x == b[t].measured.x);  // bit-identical replay
        CHECK(a[t].measured.y == b[t].measured.y);
        CHECK(a[t].measured.z == b[t].measured.z);
        CHECK(a[t].uncertainty == b[t].uncertainty);
        CHECK(a[t].seen == b[t].seen);
        // uncertainty closed form: slant (dyadic, sqrt-exact) * cone
        CHECK(a[t].uncertainty == (5000.0 + 100.0 * static_cast<double>(t)) * cone);
        // jitter stays inside the error cone (bounded error, #31b)
        const math::dvec3 truep{5000.0 + 100.0 * static_cast<double>(t), 0.0, 0.0};
        const double err = math::length(a[t].measured - truep);
        CHECK(err <= a[t].uncertainty + 1e-6);
    }

    // --- chain pins (Python hand-port of the exact expression grouping;
    // seed 0xC0FFEE, sensor 3, tick 0, target 11 at +X 5000 m, res 1').
    CHECK(a[0].measured.x == doctest::Approx(4999.9999913972915).epsilon(1e-9));
    CHECK(a[0].measured.y == doctest::Approx(0.15012114919469047).epsilon(1e-9));
    CHECK(a[0].measured.z == doctest::Approx(0.25197366136107957).epsilon(1e-9));
    CHECK(a[0].uncertainty == doctest::Approx(1.454441043328608).epsilon(1e-9));
    CHECK(a[0].seen == 0);
    CHECK(a[0].seen_by == 3);

    // --- tick-keyed (#31b: a sensor measures EVERY tick, so the tick is
    // the stream position — unlike the gun's shot-count key).
    CHECK(a[0].measured.y != a[1].measured.y);

    // --- uncertainty grows with distance (9x range): static pair, one pass.
    {
        contact_board board;
        board.ghost_ticks = 0;
        std::vector<sensor_view> wide{sensor_view{3, {}, 100000.0, 1.0, 0.0}};
        std::vector<contact_target> tg{make_target(11, {1000.0, 0.0, 0.0}, 50.0),
                                       make_target(12, {9000.0, 0.0, 0.0}, 50.0)};
        contact_measure_tick(board, std::span{wide}, std::span{tg}, 0, kSeed);
        REQUIRE(board.find(11)); REQUIRE(board.find(12));
        CHECK(board.find(12)->uncertainty == 9000.0 * cone);  // closed form
        CHECK(board.find(11)->uncertainty == 1000.0 * cone);
        CHECK_GT(board.find(12)->uncertainty, board.find(11)->uncertainty);
        // target-id re-keys the stream: same range scale, different draw.
        CHECK(board.find(12)->measured.x == doctest::Approx(8999.999928215871).epsilon(1e-9));
        CHECK(board.find(12)->measured.y == doctest::Approx(0.7104421546546271).epsilon(1e-9));
    }

    // --- assist buy-down at the authored range: gain 0.5 halves the
    // uncertainty CLOSED FORM at the same 9000 m slant, jitter still inside.
    {
        contact_board board;
        std::vector<sensor_view> wide{sensor_view{3, {}, 100000.0, 1.0, 0.0}};
        std::vector<contact_target> tg{make_target(13, {9000.0, 0.0, 0.0}, 50.0,
                                                   "class.fighter", 7, 0.5)};
        contact_measure_tick(board, std::span{wide}, std::span{tg}, 0, kSeed);
        const auto* r = board.find(13);
        REQUIRE(r);
        CHECK(r->uncertainty == 9000.0 * (cone * 0.5));  // exact half-cone
        const double err = math::length(r->measured - math::dvec3{9000.0, 0.0, 0.0});
        CHECK(err <= r->uncertainty + 1e-6);
        CHECK(r->measured.x == doctest::Approx(8999.999958592365).epsilon(1e-9));
        CHECK(r->measured.z == doctest::Approx(0.02703604205416082).epsilon(1e-9));
    }

    // --- assist_gain 0 = EXACT passthrough (measured == true, unc 0).
    {
        contact_board board;
        std::vector<contact_target> tg{make_target(14, {5000.0, 0.0, 0.0}, 50.0,
                                                   "class.fighter", 7, 0.0)};
        contact_measure_tick(board, std::span<const sensor_view>(&s, 1), std::span{tg}, 0, kSeed);
        const auto* r = board.find(14);
        REQUIRE(r);
        CHECK(r->measured.x == 5000.0);
        CHECK(r->measured.y == 0.0);
        CHECK(r->measured.z == 0.0);
        CHECK(r->uncertainty == 0.0);
    }

    // --- assist gain SANITIZE: negative -> 0 (exact passthrough), NaN and
    // >1 -> 1.0 (closed-form unc, no cone amplification, no fake-clean).
    {
        contact_board board;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        std::vector<contact_target> tg{
            make_target(15, {5000.0, 0.0, 0.0}, 50.0, "class.fighter", 7, -1.0),
            make_target(16, {9000.0, 0.0, 0.0}, 50.0, "class.fighter", 7, nan),
            make_target(17, {9000.0, 0.0, 0.0}, 50.0, "class.fighter", 7, 2.0)};
        contact_measure_tick(board, std::span<const sensor_view>(&s, 1), std::span{tg}, 0, kSeed);
        REQUIRE(board.find(15)); REQUIRE(board.find(16)); REQUIRE(board.find(17));
        CHECK(board.find(15)->uncertainty == 0.0);
        CHECK(board.find(15)->measured.x == 5000.0);   // full buy-down = exact
        CHECK(board.find(16)->uncertainty == 9000.0 * cone);   // NaN = no assist
        CHECK(board.find(17)->uncertainty == 9000.0 * cone);   // 2.0 clamps to 1.0
    }

    // --- MULTI-SENSOR nearest wins: two live sensors on one target, the
    // nearer one owns the record (seen_by + its slant*cone uncertainty).
    {
        std::vector<sensor_view> pair{base_sensor(3),
                                      sensor_view{4, {4000.0, 0.0, 0.0}, 20000.0, 1.0, 0.0}};
        std::vector<contact_target> tg{make_target(11, {5000.0, 0.0, 0.0}, 50.0)};
        contact_board board;
        contact_measure_tick(board, std::span{pair}, std::span{tg}, 0, kSeed);
        const auto* r = board.find(11);
        REQUIRE(r);
        CHECK(r->seen_by == 4);
        CHECK(r->uncertainty == 1000.0 * cone);  // slant from sensor 4, not 5000
        // reverse registration order: still sensor 4 wins (nearer, not first).
        std::vector<sensor_view> flipped{pair[1], pair[0]};
        contact_board b2;
        contact_measure_tick(b2, std::span{flipped}, std::span{tg}, 0, kSeed);
        REQUIRE(b2.find(11));
        CHECK(b2.find(11)->seen_by == 4);
    }

    // --- seed re-keys the draw (deterministic divergence, pinned).
    {
        std::vector<contact_record> d;
        drive(kSeedB, d);
        CHECK(d[0].measured.x == doctest::Approx(4999.99998428835).epsilon(1e-9));
        CHECK(d[0].measured.y == doctest::Approx(-0.37856265702407504).epsilon(1e-9));
        CHECK(d[0].measured.z == doctest::Approx(0.11750241634949304).epsilon(1e-9));
    }

    // --- sensor id re-keys: same everything else, different component id.
    {
        sensor_view s4 = base_sensor(4);
        std::vector<contact_target> tg{make_target(11, {5000.0, 0.0, 0.0}, 50.0)};
        contact_board board;
        contact_measure_tick(board, std::span<const sensor_view>(&s4, 1), std::span{tg}, 0, kSeed);
        const auto* r = board.find(11);
        REQUIRE(r);
        CHECK(r->measured.y != a[0].measured.y);
        CHECK(r->seen_by == 4);
    }

    // --- fail-safes: NO record at all. Out of range, dead range (the dark
    // sensor wire zeroes it through WiringReach — the band's zero lands
    // right here), corrupt res.
    {
        std::vector<contact_target> far{make_target(11, {20001.0, 0.0, 0.0}, 50.0)};
        contact_board board;
        contact_measure_tick(board, std::span<const sensor_view>(&s, 1), std::span{far}, 0, kSeed);
        CHECK(board.find(11) == nullptr);
    }
    {
        sensor_view dead = base_sensor();
        dead.range = 0.0;  // dark feed
        std::vector<contact_target> tg{make_target(11, {5000.0, 0.0, 0.0}, 50.0)};
        contact_board board;
        contact_measure_tick(board, std::span<const sensor_view>(&dead, 1), std::span{tg}, 0, kSeed);
        CHECK(board.find(11) == nullptr);
    }
    {
        sensor_view corrupt = base_sensor();
        corrupt.res_arcmin = std::numeric_limits<double>::quiet_NaN();
        std::vector<contact_target> tg{make_target(11, {5000.0, 0.0, 0.0}, 50.0)};
        contact_board board;
        contact_measure_tick(board, std::span<const sensor_view>(&corrupt, 1), std::span{tg}, 0, kSeed);
        CHECK(board.find(11) == nullptr);  // corrupt sensor = no contact, never a fake-clean one
    }
}

TEST_CASE("contact_class_bands") {
    const double cone = cone_arcmin1();
    // #31c boundary arithmetic: hull 50 m resolves once 50 >= d * cone.
    const double d_star = 50.0 / cone;  // 171887.338... — hand-computable
    CHECK(d_star > 171800.0);
    CHECK(d_star < 171900.0);

    sensor_view s{3, {}, 400000.0, 1.0, 40000.0};

    auto probe = [&](double dist, double gain = 1.0) {
        contact_board board;
        std::vector<contact_target> tg{make_target(11, {dist, 0.0, 0.0}, 50.0,
                                                   "class.fighter", 7, gain)};
        contact_measure_tick(board, std::span<const sensor_view>(&s, 1), std::span{tg}, 0, kSeed);
        REQUIRE(board.find(11));
        return *board.find(11);
    };

    // --- class band flips at the hand-computed range: resolves INSIDE the
    // boundary, unknown ring OUTSIDE (tag stays empty = nothing to draw).
    CHECK(probe(171800.0).class_resolved);
    CHECK(probe(171800.0).class_tag == "class.fighter");
    CHECK_FALSE(probe(171900.0).class_resolved);
    CHECK(probe(171900.0).class_tag.empty());

    // --- assist buys the silhouette band down with the same knob: cone
    // halved -> boundary doubled -> 171900 resolves under a 0.5 lock.
    CHECK(probe(171900.0, 0.5).class_resolved);

    // --- IFF band at the authored 40000 m (<= law): exact at the boundary,
    // flips one metre out; owner token only copied inside.
    CHECK(probe(39999.0).owner_resolved);
    CHECK(probe(40000.0).owner_resolved);
    CHECK(probe(40000.0).owner == 7);
    CHECK_FALSE(probe(40001.0).owner_resolved);
    CHECK(probe(40001.0).owner == 0);
    // at 39999 BOTH bands are open (well inside the class boundary)...
    CHECK(probe(39999.0).class_resolved);
    // ...and at 171900 NEITHER is: the unknown ring (#31c).
    CHECK_FALSE(probe(171900.0).owner_resolved);

    // --- hull_size 0 never classes (even up close with a clean picture).
    {
        contact_board board;
        std::vector<contact_target> tg{make_target(11, {1000.0, 0.0, 0.0}, 0.0, "class.fighter")};
        contact_measure_tick(board, std::span<const sensor_view>(&s, 1), std::span{tg}, 0, kSeed);
        const auto* r = board.find(11);
        REQUIRE(r);
        CHECK_FALSE(r->class_resolved);
    }

    // --- perfect sensor (res 0): zero cone -> EXACT measurement and
    // anything inside range classes (hull >= slant*0 for any hull > 0).
    {
        sensor_view perfect = s;
        perfect.res_arcmin = 0.0;
        contact_board board;
        std::vector<contact_target> tg{make_target(11, {171900.0, 0.0, 0.0}, 50.0),
                                       make_target(12, {1.0, 52.0, 0.0}, 50.0)};
        contact_measure_tick(board, std::span<const sensor_view>(&perfect, 1), std::span{tg}, 0, kSeed);
        const auto* r = board.find(11);
        REQUIRE(r);
        CHECK(r->measured.x == 171900.0);  // exact passthrough
        CHECK(r->uncertainty == 0.0);
        CHECK(r->class_resolved);
        // OFF-AXIS passthrough pin on a PROVABLY DRIFTING triple: slant =
        // sqrt(2705) and fl(fl(1/slant)*slant) = 0.9999999999999999, so the
        // normalize-then-rescale form CANNOT land here bit-exact — only
        // true-position passthrough can. (A 3-4-5 triple round-trips
        // exactly and would NOT discriminate.)
        const auto* r2 = board.find(12);
        REQUIRE(r2);
        CHECK(r2->measured.x == 1.0);
        CHECK(r2->measured.y == 52.0);
        CHECK(r2->measured.z == 0.0);
        CHECK(r2->uncertainty == 0.0);
    }
}

TEST_CASE("contact_ghosts_never_targetable") {
    sensor_view s = base_sensor();
    std::vector<contact_target> tg{make_target(11, {5000.0, 0.0, 0.0}, 50.0)};

    contact_board board;
    board.ghost_ticks = 300;  // authored lifetime (#31d)

    // --- live feed: every tick targetable (fresh at THIS tick).
    for (tick_t t = 0; t < 100; ++t) {
        contact_measure_tick(board, std::span<const sensor_view>(&s, 1), std::span{tg}, t, kSeed);
        REQUIRE(contact_targetable(board, 11, t));
    }
    REQUIRE(board.find(11));
    const contact_record frozen = *board.find(11);  // last live measurement, tick 99

    // --- feed DARK (#26b): the record freezes and is NEVER targetable,
    // even while the real target moves right next to the sensor.
    std::vector<contact_target> moving{make_target(11, {100.0, 0.0, 0.0}, 50.0)};
    for (tick_t t = 100; t <= 399; ++t) {
        contact_measure_tick(board, {}, std::span{moving}, t, kSeed);
        const auto* r = board.find(11);
        REQUIRE(r);  // still visible (age <= 300)
        CHECK_FALSE(contact_targetable(board, 11, t));
        // ghost NEVER updates: bit-identical to the tick-99 measurement.
        CHECK(r->measured.x == frozen.measured.x);
        CHECK(r->measured.y == frozen.measured.y);
        CHECK(r->measured.z == frozen.measured.z);
        CHECK(r->uncertainty == frozen.uncertainty);
        CHECK(r->seen == 99);
        CHECK(r->seen_by == frozen.seen_by);
        CHECK(r->class_resolved == frozen.class_resolved);
        CHECK(r->owner_resolved == frozen.owner_resolved);
    }
    // lifetime ends at age 301: present at 399 (age 300), GONE at 400.
    contact_measure_tick(board, {}, std::span{moving}, 400, kSeed);
    CHECK(board.find(11) == nullptr);

    // --- reacquisition: live feed again -> record rebuilt, targetable.
    contact_measure_tick(board, std::span<const sensor_view>(&s, 1), std::span{moving}, 401, kSeed);
    REQUIRE(board.find(11));
    CHECK(board.find(11)->seen == 401);
    CHECK(contact_targetable(board, 11, 401));
    // the fresh measurement tracks the MOVED target (not the old ghost).
    CHECK(std::abs(board.find(11)->measured.x - 100.0) <= board.find(11)->uncertainty + 1e-9);

    // --- ghost_ticks 0: the feed IS the truth — one missed tick drops the
    // record immediately (no ghost at all).
    contact_board strict;
    strict.ghost_ticks = 0;
    contact_measure_tick(strict, std::span<const sensor_view>(&s, 1), std::span{tg}, 0, kSeed);
    REQUIRE(contact_targetable(strict, 11, 0));
    contact_measure_tick(strict, {}, std::span{tg}, 1, kSeed);
    CHECK(strict.find(11) == nullptr);
}

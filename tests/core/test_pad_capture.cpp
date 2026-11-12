// 7.9 — capture zones + snap-to-rest (checklist 7.9; #29c-e, #41, #45e).
// Hand-authored pad on a hand-authored world (cooking is Phase 8). Tier 1:
// pure sim, no display, SI everywhere. Pins: pad-relative tolerances on a
// spinning moon (rides spin, #41), swept engagement sampled at the pinned
// tick, snap-to-rest pose/velocity exact, and a too-fast flyby simply never
// engages — nothing fails (#29e).

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/sim/capture.h"
#include "ve/core/sim/gravity.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/rigid.h"
#include "ve/core/sim/time.h"

namespace {

using namespace ve::core;
using namespace ve::core::sim;

// Moon fixture: same numbers as the 7.8 land-contact tests.
struct moon_world {
    rail_world rw;
    std::int32_t id = -1;

    moon_world(double spin_rate = 0.0, math::dvec3 spin_axis = {0.0, 1.0, 0.0}) {
        body_def b;
        b.well_radius = 1'300'000.0;
        b.well_taper_start = 1'100'000.0;
        b.surface_radius = 1'000'000.0;
        b.surface_g = 10.0;
        b.spin_axis = spin_axis;
        b.spin_rate_rad_s = spin_rate;
        const auto added = rw.add_body(std::move(b));
        REQUIRE(added.has_value());
        id = *added;
    }
};

pad_def test_pad(std::int32_t body, math::dvec3 local_dir) {
    return pad_def{
        .body = body,
        .local_dir = local_dir,
        .capture_radius_m = 100.0,
        .safe_speed_m_s = 2.0,
        .rest_depth_m = 0.0125,  // matches the 7.8 4-leg penalty squat depth
    };
}

void append_f64(std::string& out, double v) {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((bits >> (8 * i)) & 0xFF));
}

} // namespace

TEST_SUITE("pad_capture") {

TEST_CASE("capture_zone") {

    SUBCASE("pad-relative tolerance: a world-stationary hull is INFAST near a spinning pad (#41)") {
        moon_world mw(1e-3);  // spin about +Y: pad on +X EQUATOR, rim speed
                              // = 1e-3 * 1e6 = 1000 m/s (a polar pad would
                              // sit ON the axis and teach nothing)
        const pad_def pad = test_pad(mw.id, {1.0, 0.0, 0.0});
        const tick_t t = 0;
        const auto pw = pad_at(mw.rw, pad, t);
        REQUIRE(pw.has_value());
        // Pad snapshot rides spin (t=0: +X is unmoved, velocity is not).
        CHECK(pw->pos.x == doctest::Approx(1'000'000.0).epsilon(1e-9));
        // omega = 1e-3 yhat, r = 1e6 xhat -> v = omega x r = -1000 zhat.
        CHECK(pw->vel.z == doctest::Approx(-1'000.0).epsilon(1e-6));
        CHECK(pw->normal.x == doctest::Approx(1.0).epsilon(1e-9));

        // Hovering in world (vel 0) 20 m off the pad: world speed 0, but
        // pad-relative speed ~1000 m/s >> safe_speed -> NO engage. World-rest
        // tolerance would wrongly capture; pad-relative (#41) rejects.
        const auto above = pw->pos + pw->normal * 20.0;
        CHECK_FALSE(capture_engage(pad, *pw, above, {0.0, 0.0, 0.0}, t).has_value());

        // Matching the pad's velocity (still 20 m out, closing slow):
        // relative speed small -> engage.
        const auto slow = pw->vel + pw->normal * -1.0;  // pad-matched + 1 m/s
        // closure (pw->vel ~ -1000 zhat: a world-rest hull never matches)
        const auto hit = capture_engage(pad, *pw, above, slow, t);
        REQUIRE(hit.has_value());
    }

    SUBCASE("too-fast flyby never engages — nothing fails (#29e)") {
        moon_world mw;
        const pad_def pad = test_pad(mw.id, {0.0, 1.0, 0.0});
        const auto pw = pad_at(mw.rw, pad, 0);
        REQUIRE(pw.has_value());
        const rigid_state before{.pos = pw->pos + pw->normal * 50.0,
                                 .vel = {0.0, -10.0, 0.0}};
        // Straight through the zone at 10 m/s (safe_speed 2): never engages,
        // and the probe never mutates anything.
        auto p = before.pos;
        for (tick_t t = 0; t < 600; ++t) {
            CHECK_FALSE(capture_engage(pad, *pw, p, before.vel, t).has_value());
            p = p + before.vel * kSim_tick_dt;
        }
        CHECK(p.x == doctest::Approx(before.pos.x));
    }

    SUBCASE("zone sampled at the pinned tick: a moved-on pad is missed (#45e, #41)") {
        moon_world mw(1e-3);
        const pad_def pad = test_pad(mw.id, {1.0, 0.0, 0.0});  // rim pad
        const auto pw0 = pad_at(mw.rw, pad, 0);
        REQUIRE(pw0.has_value());
        // Hovering where the pad WAS at tick 0, pad-matched speed for tick 0
        // only: at a later tick the pad has spun away and the zone does not.
        const tick_t t_late = 6000;  // 100 s -> pad swept 1000 m/s * 100 s
        const auto pw_late = pad_at(mw.rw, pad, t_late);
        REQUIRE(pw_late.has_value());
        const double pad_arc = 1'000'000.0 * 1e-3 * 100.0;
        CHECK(pad_arc > 4.0 * pad.capture_radius_m);  // 1000 > 400
        // Hull sitting at tick 0's pad point with tick-0-matched velocity:
        CHECK(capture_engage(pad, *pw0, pw0->pos, pw0->vel, 0).has_value());
        // Same hull re-probed at the late tick against the late snapshot:
        // distance travelled >> radius -> no engage.
        CHECK_FALSE(capture_engage(pad, *pw_late, pw0->pos, pw0->vel, t_late).has_value());
    }

    SUBCASE("swept closest approach inside one fixed tick") {
        moon_world mw;
        const pad_def pad = test_pad(mw.id, {0.0, 1.0, 0.0});
        const auto pw = pad_at(mw.rw, pad, 0);
        REQUIRE(pw.has_value());
        // Hull skimming the zone edge tangentially at safe speed: closest
        // approach is mid-tick, not the endpoints.
        // rel = 90 m out on the normal, hull drifting along a tangent at 2 m/s;
        // radial closure 0.5 m/s -> closest approach at tau = ... within tick?
        // closure is small, so closest point clamps to tau=dt: pin that, plus
        // an exact mid-tick case: hull aimed at the pad centre at exactly
        // safe_speed from 99 m: tau clamps at dt (still approaching).
        const auto hit = capture_engage(
            pad, *pw, pw->pos + pw->normal * 99.0, pw->normal * -2.0, 0);
        REQUIRE(hit.has_value());
        CHECK(hit->t == kSim_tick_dt);  // clamped: still approaching all tick
        const double d = math::length(hit->pos - pw->pos);
        CHECK(d < 99.0);
        CHECK(d < 98.968);
        CHECK(d > 98.966);
        // normal points pad -> hull (outward).
        CHECK(hit->normal.y == doctest::Approx(1.0).epsilon(1e-12));
        // Just outside at tick start, not reached within the tick: no engage.
        CHECK_FALSE(capture_engage(pad, *pw, pw->pos + pw->normal * 100.9,
                                   pw->normal * -2.0, 0)
                        .has_value());
    }

    SUBCASE("snap-to-rest: exact pad-relative rest pose and velocity") {
        moon_world mw;
        const pad_def pad = test_pad(mw.id, {0.0, 1.0, 0.0});
        const auto pw = pad_at(mw.rw, pad, 0);
        REQUIRE(pw.has_value());
        // Hull tumbling in with junk attitude (30 deg roll) and drift.
        const math::dquat roll = math::quat_from_axis_angle({1.0, 0.0, 0.0}, 0.5236);
        const rigid_state ship{.pos = pw->pos + pw->normal * 40.0 +
                                    math::dvec3{3.0, 0.0, 2.0},
                               .vel = {1.0, -2.0, 0.5},
                               .att = roll,
                               .omega_body = {0.3, -0.1, 0.0}};
        const auto out = snap_to_rest(ship, *pw, pad);
        // Contact point (hull -Y) at rest_depth under the pad surface.
        const auto want_pos = pw->pos - pw->normal * pad.rest_depth_m;
        CHECK(out.pos.x == doctest::Approx(want_pos.x).epsilon(1e-12));
        CHECK(out.pos.y == doctest::Approx(want_pos.y).epsilon(1e-12));
        CHECK(out.pos.z == doctest::Approx(want_pos.z).epsilon(1e-12));
        CHECK(out.vel.x == pw->vel.x);  // pad-relative rest (bit-exact)
        CHECK(out.vel.y == pw->vel.y);
        CHECK(out.vel.z == pw->vel.z);
        CHECK(math::length(out.omega_body) == 0.0);
        // Hull local -Y now aims at the pad normal.
        const auto down = math::rotate(out.att, {0.0, -1.0, 0.0});
        CHECK(down.y > 0.999999);  // pad normal +Y
        CHECK(down.y <= 1.0);
        // The re-aim is the minimal rotation: roll preserved only as needed —
        // hull local +X stays in the original roll plane (no spin added):
        const auto right = math::rotate(out.att, {1.0, 0.0, 0.0});
        CHECK(right.y == doctest::Approx(0.0).epsilon(1e-9));
    }

    SUBCASE("snap-to-rest 180-degenerate: hull upside down gets a pinned flip") {
        moon_world mw;
        const pad_def pad = test_pad(mw.id, {0.0, 1.0, 0.0});
        const auto pw = pad_at(mw.rw, pad, 0);
        REQUIRE(pw.has_value());
        // Attitude: identity — hull -Y points DOWN (-Y world), pad normal is
        // +Y: exactly 180 deg off, the degenerate branch of minimal_rotation.
        const rigid_state ship{.pos = pw->pos + pw->normal * 10.0};
        const auto out = snap_to_rest(ship, *pw, pad);
        const auto down = math::rotate(out.att, {0.0, -1.0, 0.0});
        CHECK(down.y > 0.999999);
        CHECK(down.y <= 1.0);
        // Deterministic axis choice: cross(xhat, from=-yhat) = -zhat -> flip
        // about -Z; cos(pi)=0 makes the w component pure fp noise (sin ~ 1),
        // so only x/y/z + the down-vector pin are lockable here.
        CHECK(out.att.x == 0.0);
        CHECK(out.att.y == 0.0);
        CHECK(out.att.z == doctest::Approx(-1.0));
        CHECK(out.att.w == doctest::Approx(0.0));
    }

    SUBCASE("scripted braked descent lands inside tolerance, deterministically (#29c)") {
        // The 7.9 half of the "one scripted pad landing" pin: fuel-braked
        // descent (engines hold 1.5 m/s closure — safe_speed 2.0), capture
        // engages at the tick the hull crosses the 100 m shell, snap holds.
        // The takeoff/cruise half lands in 7.14's full sequence.
        moon_world mw;
        const pad_def pad = test_pad(mw.id, {0.0, 1.0, 0.0});

        auto run = [&](double nudge) {
            std::string h;
            const auto pw = pad_at(mw.rw, pad, 0);
            REQUIRE(pw.has_value());
            rigid_state s{.pos = pw->pos + pw->normal * 300.0,
                          .vel = {0.0, -1.0 + nudge, 0.0}};
            bool snapped = false;
            std::int64_t snap_tick = -1;
            for (tick_t t = 0; t < 40'000; ++t) {
                if (!snapped) {
                    // engine-braked descent: hold -1.5 m/s closure exactly
                    // (well accel is cancelled — the profile is authored,
                    // not solved), then integrate the position.
                    s.vel = s.vel + sim::well_accel(mw.rw, mw.id, s.pos, t) *
                                          kSim_tick_dt;
                    s.vel.y = -1.5 + (t == 0 ? nudge : 0.0);
                    s.pos = s.pos + s.vel * kSim_tick_dt;
                    const auto pt = pad_at(mw.rw, pad, t);
                    REQUIRE(pt.has_value());
                    if (const auto hit = capture_engage(pad, *pt, s.pos, s.vel, t)) {
                        s = snap_to_rest(s, *pt, pad);
                        snapped = true;
                        snap_tick = static_cast<std::int64_t>(t);
                    }
                }
                append_f64(h, s.pos.x);
                append_f64(h, s.pos.y);
                append_f64(h, s.pos.z);
            }
            append_f64(h, static_cast<double>(snap_tick));
            return std::pair{h, snap_tick};
        };

        const auto [hash_a, tick_a] = run(0.0);
        const auto [hash_b, tick_b] = run(0.0);
        CHECK(hash_a == hash_b);  // std::string compare is exact bits
        CHECK(tick_a == tick_b);
        // Analytic engagement tick: 200 m of closure at 1.5 m/s = 133.33 s
        // -> 8000 ticks; the tick-crossing shell lands inside a few ticks.
        CHECK(tick_a >= 7900);
        CHECK(tick_a <= 8060);
        // Sensitivity, pinned not hand-waved: engagement is a shell crossing,
        // so a 1e-3 m/s velocity nudge at tick 0 moves the crossing by
        // 300e-3/1.5^2 s ~ 8e-5 s (~5 ticks) and the hash by construction;
        // a 1e-9 nudge is far sub-tick and lands on the SAME tick (both
        // pinned: the pipeline neither damps chaos nor amplifies noise).
        const auto [hash_c, tick_c] = run(1e-3);
        CHECK(hash_c != hash_a);
        CHECK(tick_c >= tick_a - 1);
        CHECK(tick_c <= tick_a + 10);
        const auto [hash_d, tick_d] = run(1e-9);
        CHECK(tick_d == tick_a);
    }
}

} // TEST_SUITE

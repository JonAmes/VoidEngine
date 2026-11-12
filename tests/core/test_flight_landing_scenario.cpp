// Phase 7 task 7.14 — DONE-WHEN (a) harness: scripted takeoff -> cruise ->
// pad landing in ONE headless run composing spin + aero + contacts +
// capture. No production code: a composition proof over 7.1/7.6/7.8/7.9
// machinery via the shared Phase-4 replay spine (decision #12). The pilot
// is a pure per-tick command stream; determinism is run==run bit-identical
// hash streams; pins are invariants/bands, not hand-derived trajectory
// values (closed-loop flight — see notes.md 7.14 for the lessons).
//
// 7.15 refactor: the SIMULATION moved to tests/core/flight_scenario.h so
// the renderer visibility proof (gfx/test_flight_visible.cpp) replays the
// SAME flight. This file is the pin block, unchanged since 6e13864.

#include <cmath>

#include <doctest/doctest.h>

#include "flight_scenario.h"

using namespace ve_test::flight;

TEST_SUITE("flight_landing_scenario") {

TEST_CASE("takeoff_cruise_pad_landing") {
    const run_result A = run_scenario();
    const run_result B = run_scenario();

    INFO("engage_tick=" << A.engage_tick << " rel_v=" << A.engage_rel_speed
         << " world_v=" << A.engage_world_speed << " pad_v=" << A.pad_speed);
    INFO("max_alt=" << A.max_altitude << " max_aoa=" << A.max_aoa
         << " @tick=" << A.max_aoa_tick << " cl@aoamax=" << A.cl_at_max_aoa
         << " cl_max=" << A.cl_max << " max_q=" << A.max_dyn_q);
    INFO("glide v " << A.speed_glide_start << " -> " << A.speed_glide_end
         << ", alt_end=" << A.alt_glide_end
         << "; post: pos<=" << A.worst_pos << " v<=" << A.worst_v
         << " depth[" << A.worst_dlo << "," << A.worst_dhi << "]");
    CHECK(A.ok);
    CHECK((A.engage_tick >= kT_APPR && A.engage_tick < kT_END));
    SUBCASE("composes contacts: equilibrium squat at pad rest") {
        // snap rest depth = mg/(4k) = 0.0075 exactly; band tolerates the
        // pad ride + spin drag (relax-model band, notes 7.12).
        CHECK((A.rest_depth_30 > 0.005 && A.rest_depth_30 < 0.011));
    }
    SUBCASE("liftoff under thrust; cruise altitude reached") {
        CHECK((A.max_altitude > 300.0 && A.max_altitude < 450.0));
    }
    SUBCASE("aero live: stall region visited, rolloff forgiving, drag real") {
        CHECK(A.max_dyn_q > 100.0);
        // excursion crossed the stall angle...
        CHECK(A.max_aoa > kAero.max_aoa_rad);
        // ...but no deep tumble (aircraft frame never points back at flow)
        CHECK(A.max_aoa < 1.2);
        // forgiving rolloff: CL peaked in the lifting lobe and rolled off
        // to ZERO (never negative, never a cliff) even at max aoa.
        CHECK((A.cl_max > 0.5 && A.cl_max <= kAero.lift_coeff));
        CHECK((A.cl_at_max_aoa >= 0.0 && A.cl_at_max_aoa < kAero.lift_coeff));
        // drag real: unpowered glide bled mechanical energy 0.5 v^2 + g h
        // (ballistic flight would conserve it exactly).
        const double e0 = 0.5 * A.speed_glide_start * A.speed_glide_start
                        + 3.0 * A.alt_glide_start;
        const double e1 = 0.5 * A.speed_glide_end * A.speed_glide_end
                        + 3.0 * A.alt_glide_end;
        CHECK(e1 < e0 - 100.0);
        // lift load-bearing: ballistic fall from ~300 m hits ground in
        // sqrt(2*300/3) = 14 s; the glide survived 30 s at > 80 m.
        CHECK(A.alt_glide_end > 80.0);
    }
    SUBCASE("capture lands inside tolerance, pad-relative (#41)") {
        REQUIRE(A.engage_tick > 0);
        CHECK((A.engage_rel_speed > 0.1 && A.engage_rel_speed < 6.0));
        // spin composed: the pad itself moves ~1 m/s; world speed differs
        // from the pad-relative speed by that ride.
        CHECK(A.pad_speed > 0.4);
        CHECK(std::abs(A.engage_world_speed - A.pad_speed) < 7.0);
        // Post-capture rest: the 7.8 model has NO sustained tangential
        // force (by design, see test_land_contacts.cpp header), so on a
        // SPINNING pad the snapped stand coasts slowly across the surface
        // while the legs keep riding the curvature. Pins: the stand stays
        // on the pad (10 m << the 100 m capture radius), the load always
        // rides at least one leg in the relax band, relative speed stays
        // walking-pace slow.
        CHECK(A.worst_pos < 10.0);
        CHECK((A.worst_dlo > 0.002 && A.worst_dhi < 0.05));
        CHECK(A.worst_v < 0.5);
    }
    SUBCASE("run == run replay-hash determinism") {
        REQUIRE(A.hashes.size() == B.hashes.size());
        CHECK(A.hashes == B.hashes);
        CHECK(replay_interval_hash(A.hashes, 0, A.hashes.size() - 1) ==
              replay_interval_hash(B.hashes, 0, B.hashes.size() - 1));
        CHECK(A.engage_tick == B.engage_tick);
    }
}

} // TEST_SUITE flight_landing_scenario

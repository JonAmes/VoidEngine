// Phase 5 task 5.4 — game attr keys (game_attr_keys): the declared key set
// is well-formed, unique, and complete vs the checklist; plus #42 unit
// coercion pins (every ratified suffix, bare/unknown rejected).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "ve/game/attr_keys.h"
#include "ve/game/units.h"
#include "ve/game/facet_actions.h"

namespace {
using namespace ve::game;
}  // namespace

TEST_CASE("game_attr_keys") {
    // ---- format + uniqueness over the declared set ----
    for (auto k : kPhase5_attr_keys) {
        INFO("key: " << k);
        CHECK(is_valid_attr_key(k));
    }
    for (std::size_t i = 0; i < kPhase5_attr_keys.size(); ++i)
        for (std::size_t j = i + 1; j < kPhase5_attr_keys.size(); ++j)
            CHECK(kPhase5_attr_keys[i] != kPhase5_attr_keys[j]);
    CHECK(kPhase5_attr_keys.size() == 15);

    // every named constant is IN the set (the array is the registry).
    for (auto k : {kAttr_thrust_force, kAttr_thrust_isp, kAttr_power_supply,
                   kAttr_power_draw, kAttr_mass, kAttr_lift_coeff,
                   kAttr_shield_capacity, kAttr_ftl_speed_mult, kAttr_ftl_spool_s,
                   kAttr_ftl_flow, kAttr_sensor_range, kAttr_sensor_res,
                   kAttr_cargo_capacity, kAttr_interact_range})
        CHECK(std::find(kPhase5_attr_keys.begin(), kPhase5_attr_keys.end(), k) !=
              kPhase5_attr_keys.end());

    // ---- grammar gates ----
    CHECK(is_valid_attr_key("a"));
    CHECK(is_valid_attr_key("station.role_tag2"));
    CHECK(is_valid_attr_key("repair.target"));
    CHECK_FALSE(is_valid_attr_key(""));
    CHECK_FALSE(is_valid_attr_key(".x"));
    CHECK_FALSE(is_valid_attr_key("x."));
    CHECK_FALSE(is_valid_attr_key("x..y"));
    CHECK_FALSE(is_valid_attr_key("X"));        // uppercase
    CHECK_FALSE(is_valid_attr_key("1x"));       // digit start
    CHECK_FALSE(is_valid_attr_key("x-y"));      // bad char
    CHECK_FALSE(is_valid_attr_key("x y"));      // space

    // ---- #42 coercion pins ----
    CHECK(coerce_to_si(150.0, "km").value() == doctest::Approx(150000.0));
    CHECK(coerce_to_si(1.0, "AU").value() == doctest::Approx(149597870700.0));
    CHECK(coerce_to_si(2.0, "day").value() == doctest::Approx(172800.0));
    CHECK(coerce_to_si(180.0, "deg").value() == doctest::Approx(3.141592653589793));
    CHECK(coerce_to_si(1.0, "m").value() == 1.0);    // identity units
    CHECK(coerce_to_si(1.0, "s").value() == 1.0);
    CHECK(coerce_to_si(1.0, "rad").value() == 1.0);
    // 360 deg/day == Earth-ish spin rate in rad/s:
    CHECK(coerce_to_si(360.0, "deg/day").value() == doctest::Approx(7.272205e-5).epsilon(1e-4));

    // ---- bare / unknown / case-sensitivity gates ----
    CHECK_FALSE(coerce_to_si(5.0, "").has_value());
    CHECK(coerce_to_si(5.0, "").error() == unit_error::bare_suffix);
    CHECK(coerce_to_si(5.0, "bogus").error() == unit_error::unknown_suffix);
    CHECK_FALSE(coerce_to_si(5.0, "KM").has_value());   // case-sensitive
    CHECK_FALSE(coerce_to_si(5.0, "Day").has_value());
}

// #28a / 5.9b — the closed facet-action registry (#46f checkpoint decision).
TEST_CASE("game_facet_actions") {
    using namespace ve::game;
    CHECK(kPhase5_facet_actions.size() == 12);
    for (const auto a : kPhase5_facet_actions) CHECK(is_valid_facet_action(a));
    CHECK_FALSE(is_valid_facet_action("use.ladder"));   // grammar ok, not ratified
    CHECK_FALSE(is_valid_facet_action("Use.Seat"));     // case-sensitive
    CHECK_FALSE(is_valid_facet_action("use..seat"));    // bad grammar
    CHECK_FALSE(is_valid_facet_action(""));
    CHECK(is_valid_attr_key("use.ladder"));             // grammar vs membership
}

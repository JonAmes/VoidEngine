// Phase 5 task 5.1 — loadout data model (loadout_model) + mount validation
// rules (loadout_mount_rules). Concept-free structural checks only: table
// integrity, category equality, opaque ids/keys, insertion-order iteration.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "ve/core/sim/loadout.h"

namespace {

using namespace ve::core;
using ve::core::math::dquat;
using ve::core::math::dvec3;
using sim::Loadout;
using sim::attr_pair;
using sim::component_type;
using sim::grid_block;
using sim::hull_info;
using sim::loadout_error;
using sim::mount_request;
using sim::socket_category;
using sim::socket_desc;

hull_info test_hull() {
    hull_info h;
    h.mesh_content_id = 0x1234ABCDu;
    h.class_tag = "class.shuttle";
    h.grid = grid_block{2.0, 4, 2.5, 0.05};
    h.hull_mass = 500.0;
    h.hull_com_local = dvec3(0.0, 0.0, -0.3);
    return h;
}

std::vector<socket_desc> test_sockets() {
    // One of each category, plus a second main for twin engines.
    return {
        {"hp_main_L", socket_category::main_, dvec3(1.4, -0.4, -4.5), dquat{}},
        {"hp_main_R", socket_category::main_, dvec3(-1.4, -0.4, -4.5), dquat{}},
        {"hp_rcs_nose", socket_category::rcs_, dvec3(0.0, 0.4, 2.2), dquat{}},
        {"hp_util_1", socket_category::utility_, dvec3(0.0, 1.1, -0.7), dquat{}},
        {"hp_gun_1", socket_category::gun_, dvec3(2.4, -0.3, 0.0), dquat{}},
    };
}

std::vector<component_type> test_catalog() {
    return {
        {"thruster.main", socket_category::main_},
        {"thruster.rcs", socket_category::rcs_},
        {"reactor.core", socket_category::utility_},
        {"gun.fixed", socket_category::gun_},
    };
}

std::vector<mount_request> test_mounts() {
    return {
        {"eng_L", "thruster.main", "hp_main_L",
         {{"thrust.force", 90000.0}, {"thrust.isp", 300.0}, {"mass", 180.0}}},
        {"eng_R", "thruster.main", "hp_main_R",
         {{"thrust.force", 90000.0}, {"thrust.isp", 300.0}, {"mass", 180.0}}},
        {"reactor", "reactor.core", "hp_util_1",
         {{"power.supply", 400.0}, {"mass", 90.0}}},
    };
}

std::expected<Loadout, loadout_error> build(std::vector<socket_desc> s,
                                            std::vector<mount_request> m) {
    return Loadout::assemble(test_hull(), std::move(s), test_catalog(), std::move(m));
}

// Must-fail helper: REQUIRE first so a rule that STOPS rejecting a bad
// input fails cleanly instead of .error()-ing on a value (UB).
loadout_error err_of(std::expected<Loadout, loadout_error> r) {
    REQUIRE_FALSE(r.has_value());
    return r.error();
}

}  // namespace

TEST_CASE("loadout_model") {
    auto lo = build(test_sockets(), test_mounts());
    REQUIRE(lo.has_value());

    // Hull identity survives untouched (opaque tag, content id, grid, mass).
    CHECK(lo->hull().mesh_content_id == 0x1234ABCDu);
    CHECK(lo->hull().class_tag == "class.shuttle");
    CHECK(lo->hull().grid.subcell_ratio == 4);
    CHECK(lo->hull().hull_mass == doctest::Approx(500.0));

    // Sockets kept in authored order; queries resolve.
    REQUIRE(lo->sockets().size() == 5);
    CHECK(lo->sockets()[0].id == "hp_main_L");
    CHECK(lo->socket_index("hp_gun_1") == 4);
    CHECK(lo->socket_index("hp_missing") == -1);

    // Mounts in REQUEST order (#5 deterministic iteration), category from
    // the socket (equality-checked), attrs kept in insertion order.
    REQUIRE(lo->mounts().size() == 3);
    CHECK(lo->mounts()[0].instance_id == "eng_L");
    CHECK(lo->mounts()[2].instance_id == "reactor");
    CHECK(lo->mounts()[0].category == socket_category::main_);
    REQUIRE(lo->mounts()[0].attrs.size() == 3);
    CHECK(lo->mounts()[0].attrs[0].key == "thrust.force");  // not sorted: kept

    // Queries: by instance, by socket, attr lookup.
    const auto* mc = lo->mount_at_socket("hp_main_R");
    REQUIRE(mc != nullptr);
    CHECK(mc->instance_id == "eng_R");
    CHECK(lo->mount_at_socket("hp_gun_1") == nullptr);  // free
    CHECK(lo->mount("reactor") == lo->mount_at_socket("hp_util_1"));
    CHECK(lo->attr(*mc, "thrust.isp") == doctest::Approx(300.0));
    CHECK_FALSE(lo->attr(*mc, "no.such.key").has_value());
    CHECK(lo->free_socket_count() == 2);

    // Empty mount list is a legal loadout (bare hull).
    auto bare = build(test_sockets(), {});
    REQUIRE(bare.has_value());
    CHECK(bare->mounts().empty());
    CHECK(bare->free_socket_count() == 5);
}

TEST_CASE("loadout_mount_rules") {
    // Duplicate socket id.
    auto s_dup = test_sockets();
    s_dup[1].id = s_dup[0].id;
    auto e = err_of(build(s_dup, {}));
    CHECK(e == loadout_error::duplicate_socket_id);

    // Mount onto a socket that does not exist.
    e = err_of(build(test_sockets(), {{"x", "thruster.main", "hp_nope", {}}}));
    CHECK(e == loadout_error::unknown_socket);

    // Unknown component type (not in the catalog).
    e = err_of(build(test_sockets(), {{"x", "warp.core", "hp_main_L", {}}}));
    CHECK(e == loadout_error::unknown_component_type);

    // Two mounts, same socket.
    e = err_of(build(test_sockets(),
              {{"a", "thruster.main", "hp_main_L", {}},
               {"b", "thruster.main", "hp_main_L", {}}}));
    CHECK(e == loadout_error::socket_occupied);

    // Category mismatch: type says main, socket says rcs — pure equality,
    // core has no policy about what categories afford.
    e = err_of(build(test_sockets(), {{"a", "thruster.main", "hp_rcs_nose", {}}}));
    CHECK(e == loadout_error::category_mismatch);

    // Duplicate instance ids.
    e = err_of(build(test_sockets(),
              {{"same", "thruster.main", "hp_main_L", {}},
               {"same", "thruster.main", "hp_main_R", {}}}));
    CHECK(e == loadout_error::duplicate_instance_id);

    // Duplicate attribute key within one mount.
    e = err_of(build(test_sockets(),
              {{"a", "thruster.main", "hp_main_L",
                {{"mass", 1.0}, {"mass", 2.0}}}}));
    CHECK(e == loadout_error::duplicate_attr_key);

    // Grid block sanity (#44): non-positive cell, subcell < 1, negative tol.
    hull_info h = test_hull();
    h.grid.cell = 0.0;
    e = err_of(Loadout::assemble(h, test_sockets(), test_catalog(), {}));
    CHECK(e == loadout_error::invalid_grid);
    h = test_hull();
    h.grid.subcell_ratio = 0;
    e = err_of(Loadout::assemble(h, test_sockets(), test_catalog(), {}));
    CHECK(e == loadout_error::invalid_grid);
    h = test_hull();
    h.grid.tolerance = -0.01;
    e = err_of(Loadout::assemble(h, test_sockets(), test_catalog(), {}));
    CHECK(e == loadout_error::invalid_grid);

    // Negative hull mass.
    h = test_hull();
    h.hull_mass = -1.0;
    e = err_of(Loadout::assemble(h, test_sockets(), test_catalog(), {}));
    CHECK(e == loadout_error::invalid_hull_mass);

    // Check order is FIXED (deterministic error for multi-broken input):
    // bad socket id checked before mounts -> duplicate_socket_id wins even
    // though the mount also dangles.
    s_dup = test_sockets();
    s_dup[1].id = s_dup[0].id;
    e = err_of(build(s_dup, {{"a", "thruster.main", "hp_gone", {}}}));
    CHECK(e == loadout_error::duplicate_socket_id);
}

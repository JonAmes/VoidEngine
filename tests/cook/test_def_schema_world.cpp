// Editor kit EK.4.0 (#43 lock-step): the forms schema for world system defs
// must accept exactly what tools/cook/src/worlddef.cpp accepts — the same
// text flowing green through BOTH the forms parser and the cook gate is the
// proof. Also proves the edit direction: a forms mutation (set_leaf) re-emits
// text that passes the real gate and lands in the cooked/decoded def.
#include <ve/cook/cook.h>
#include <ve/core/cooked.h>
#include <ve/game/def_forms_panel.h>
#include <ve/game/def_schema.h>
#include <ve/game/world_load.h>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../ve_test_portability.h"  // getpid

using namespace ve;

namespace {

constexpr const char* kEmpty = R"toml(
world_class = "system"
name = "Empty"
seed = 1
members = []
)toml";

// One row of every kind the system parse accepts.
constexpr const char* kRich = R"toml(
world_class = "system"
name = "Rich"
seed = 9
members = ["bodies/vesper.toml"]

[[faction]]
name = "Vesper Union"
power = 5
stance = ["trade", "patrol"]

[[waypoint]]
id = "gate"
kind = "patrol"
at = [0, 0, 5000]

[[waypoint]]
id = "mine"
kind = "lane"
at = [8000, 0, 0]

[[edge]]
kind = "patrol"
from = "gate"
to = "mine"

[[market]]
id = "gate_market"
anchor = "bodies/vesper.toml"

[[market]]
id = "mine_market"
anchor = "bodies/vesper.toml"

[[production]]
market = "gate_market"
commodity = "iron"
rate = 12.5

[[lane]]
id = "iron_lane"
from = "gate_market"
to = "mine_market"
commodity = "iron"
flow = 3

[[job]]
id = "hauling"

[[job_step]]
job = "hauling"
verb = "haul"
target = "iron_lane"
amount = 3

[[spawn]]
id = "gate_guard"
at = "gate"
faction = "Vesper Union"
class = "skiff"
rate = 0.5
)toml";

// Minimal body def matching the shipped fixture pattern (test_worlddef):
// the system's member/anchor gates need a real def file on disk.
constexpr const char* kVesper = R"toml(
world_class = "body"
name = "Vesper Prime"
radius = "6200 km"
well_radius = "9000 km"
surface_g = 9.4
tags = []
)toml";

struct Tree {
    std::filesystem::path src, out;
    explicit Tree(const char* sys) {
        static int n = 0;
        auto base = std::filesystem::temp_directory_path() /
                    ("ve_dsw_" + std::to_string(ve_test::pid()) + "_" + std::to_string(++n));
        src = base / "src";
        out = base / "out";
        std::filesystem::remove_all(base);
        std::filesystem::create_directories(src / "world" / "bodies");
        std::ofstream f(src / "world" / "system.toml");
        f << sys;
        REQUIRE(f.good());
        std::ofstream b(src / "world" / "bodies" / "vesper.toml");
        b << kVesper;
        REQUIRE(b.good());
    }
    ~Tree() { std::filesystem::remove_all(src.parent_path()); }
};

core::cooked::Unit wrld_of(const Tree& t) {
    core::cooked::Unit u;
    std::string err;
    REQUIRE_MESSAGE(core::cooked::read_unit_file((t.out / "units/world/system.toml.ck").string(), u, &err), err);
    return u;
}

} // namespace

TEST_CASE("def_schema_world") {
    // Forms parse of the empty EK.3 fixture: scalars present, no rows.
    game::def_forms_doc doc;
    std::string err;
    REQUIRE_MESSAGE(doc.load(game::kWorld_system_def_schema, kEmpty, &err), err);
    CHECK(doc.value("world_class") == "system");
    CHECK(doc.value("name") == "Empty");
    CHECK(doc.value("seed") == "1");
    CHECK(doc.paths().size() == 3);  // members=[]: empty list has no leaves

    // Forms parse of a one-row-of-every-kind system: this is the SAME text
    // shape the cook parser accepts below — schema and parser agree.
    game::def_forms_doc rich;
    REQUIRE_MESSAGE(rich.load(game::kWorld_system_def_schema, kRich, &err), err);
    CHECK(rich.value("faction.0.name") == "Vesper Union");
    CHECK(rich.value("waypoint.1.at.0") == "8000");
    CHECK(rich.value("waypoint.0.at.2") == "5000");
    CHECK(rich.value("production.0.rate") == "12.5");
    CHECK(rich.value("spawn.0.class") == "skiff");
    CHECK(rich.value("edge.0.kind") == "patrol");

    // Unknown keys are refused: forms follow the schema, never invent (#43).
    game::def_forms_doc bad;
    CHECK_FALSE(bad.load(game::kWorld_system_def_schema, "world_class = \"system\"\nbogus = 7\n", &err));

    // Edit direction: forms mutation -> real gate -> cooked def.
    REQUIRE(rich.set_leaf("name", "Changed"));
    Tree t{rich.source().c_str()};
    auto res = cook::cook_incremental(t.src.string(), t.out.string());
    REQUIRE_MESSAGE(res.ok, res.error);
    auto dec = game::decode_world(wrld_of(t).payload);
    REQUIRE_MESSAGE(dec, dec.error());
    CHECK(dec->name == "Changed");
    CHECK(dec->members.size() == 1);
    CHECK(dec->factions.size() == 1);
    CHECK(dec->markets.size() == 2);
    CHECK(dec->lanes.size() == 1);
    CHECK(dec->waypoints.size() == 2);
    CHECK(dec->nav_edges.size() == 1);
    CHECK(dec->job_templates.size() == 1);
    CHECK(dec->spawns.size() == 1);
}

// Editor kit EK.3 (#54): the empty world — the smallest def that passes every
// production gate. One system.toml, zero members/stations/lanes/rows/markets,
// no defs/, no samples/: the ONLY cooked artifact is the WRLD unit. This is
// the tree ve_edit boots on; anything the empty world fails, ve_edit cannot
// assume away. Pins: cook green, wrldvalidate green (it runs inside cook),
// world_load decodes zero of every section, author_world opens it and its
// schematic has no nodes, re-cook is byte-stable.
#include <ve/cook/author_gizmos.h>
#include <ve/cook/cook.h>
#include <ve/core/cooked.h>
#include <ve/game/world_load.h>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "../ve_test_portability.h"  // getpid

using namespace ve;

namespace {

constexpr const char* kEmptySys = R"toml(
world_class = "system"
name = "Empty"
seed = 1
members = []
)toml";

struct EmptyTree {
    std::filesystem::path src, out;
    EmptyTree() {
        static int n = 0;
        auto base = std::filesystem::temp_directory_path() /
                    ("ve_empty_" + std::to_string(ve_test::pid()) + "_" + std::to_string(++n));
        src = base / "src";
        out = base / "out";
        std::filesystem::remove_all(base);
        std::filesystem::create_directories(src / "world");
        std::ofstream f(src / "world" / "system.toml");
        f << kEmptySys;
        REQUIRE(f.good());
    }
    ~EmptyTree() { std::filesystem::remove_all(src.parent_path()); }
};

core::cooked::Unit unit_of(const EmptyTree& t, const std::string& id) {
    core::cooked::Unit u;
    std::string err;
    REQUIRE_MESSAGE(core::cooked::read_unit_file((t.out / ("units/" + id + ".ck")).string(), u, &err), err);
    return u;
}

} // namespace

TEST_CASE("world_empty") {
    EmptyTree t;
    auto res = cook::cook_incremental(t.src.string(), t.out.string());
    REQUIRE_MESSAGE(res.ok, res.error);

    // Exactly one cooked world unit; no other world artifacts exist.
    const auto u = unit_of(t, "world/system.toml");
    CHECK(u.type == core::cooked::kUnitWrld);
    CHECK_FALSE(std::filesystem::exists(t.out / "units/world/bodies"));

    // world_load decodes zero rows everywhere (re-encode == encode holds the
    // exact payload across the empty-option edges of the codec).
    auto dec = game::decode_world(u.payload);
    REQUIRE_MESSAGE(dec, dec.error());
    CHECK(game::encode_world(*dec) == u.payload);
    CHECK(dec->world_class == game::WorldClass::system);
    CHECK(dec->name == "Empty");
    CHECK(dec->seed == 1u);
    CHECK(dec->members.empty());
    CHECK(dec->factions.empty());
    CHECK(dec->markets.empty());
    CHECK(dec->lanes.empty());
    CHECK(dec->build_plans.empty());

    // The authoring host's entry point opens it: the schematic is live but
    // node-less at tick 0, and the rails graph has no bodies to pick.
    cook::author_world aw;
    std::string err;
    std::map<std::string, std::string> sources{{"world/system.toml", kEmptySys}};
    REQUIRE_MESSAGE(aw.load(sources, t.src.string(), t.out.string(), &err), err);
    CHECK(aw.sys_def() != nullptr);
    CHECK(aw.schematic_nodes(0).empty());

    // Re-cook with nothing touched: byte-stable payload (hash-stable across
    // runs is what the editor's incremental loop leans on).
    auto res2 = cook::cook_incremental(t.src.string(), t.out.string());
    REQUIRE_MESSAGE(res2.ok, res2.error);
    CHECK(unit_of(t, "world/system.toml").payload == u.payload);
}

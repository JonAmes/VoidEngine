// test: incremental_recook — only content-hash-changed sources re-cook.
#include <doctest/doctest.h>

#include "../ve_test_portability.h" // getpid

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <ve/cook/cook.h>

namespace {
std::string tmp_root(const char* name) {
    static std::atomic<int> n{0};
    auto p = std::filesystem::temp_directory_path() /
             ("ve_cook_" + std::string(name) + "_" + std::to_string(ve_test::pid()) + "_" +
              std::to_string(n++));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << contents;
}
} // namespace

TEST_CASE("incremental_recook — hash-gated re-cook + dep graph in manifest") {
    auto src = tmp_root("inc_src");
    auto out = tmp_root("inc_out");

    write_file(std::filesystem::path(src) / "a.bin", "PNGFAKE-bytes-v1");
    write_file(std::filesystem::path(src) / "defs/x.toml",
               "# ship def\nkind = \"test\"\nrefs = [\"../a.bin\"] # the texture\n");

    auto r1 = ve::cook::cook_incremental(src, out);
    REQUIRE(r1.ok);
    CHECK(r1.counts.cooked == 2);
    CHECK(r1.counts.skipped == 0);

    // dep graph recorded: x.toml -> a.bin
    ve::cook::Manifest man;
    std::string err;
    REQUIRE(ve::cook::read_manifest(out + "/manifest.bin", man, &err));
    CHECK(man["defs/x.toml"].deps == std::vector<std::string>({"a.bin"}));

    // unit files exist by content ID
    CHECK(std::filesystem::exists(std::filesystem::path(out) / "units/a.bin.ck"));
    CHECK(std::filesystem::exists(std::filesystem::path(out) / "units/defs/x.toml.ck"));

    // second cook: everything hash-identical -> zero re-cook
    auto r2 = ve::cook::cook_incremental(src, out);
    REQUIRE(r2.ok);
    CHECK(r2.counts.cooked == 0);
    CHECK(r2.counts.skipped == 2);

    // change one source -> exactly that unit re-cooks
    write_file(std::filesystem::path(src) / "a.bin", "PNGFAKE-bytes-v2-CHANGED");
    auto r3 = ve::cook::cook_incremental(src, out);
    REQUIRE(r3.ok);
    CHECK(r3.counts.cooked == 1);
    CHECK(r3.counts.skipped == 1);

    // the cooked unit carries the NEW bytes
    ve::cook::Unit u;
    REQUIRE(ve::cook::read_unit_file(out + "/units/a.bin.ck", u, &err));
    std::string payload(u.payload.begin(), u.payload.end());
    CHECK(payload == "PNGFAKE-bytes-v2-CHANGED");

    std::filesystem::remove_all(src);
    std::filesystem::remove_all(out);
}

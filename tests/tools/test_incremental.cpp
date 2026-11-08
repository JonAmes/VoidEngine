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

#include "glb_fixture.h"       // hand-authored minimal .glb bytes (3.8)
#include "../gfx/gfx_golden.h" // write_png_rgba8

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

TEST_CASE("incremental_recook — glb texture dep: texture edit re-cooks only the texture") {
    // The manifest edge mesh -> texture (3.8) must survive incremental
    // re-cooks, and editing the texture alone must NOT re-cook the glb —
    // the mesh payload holds only a content-ID ref, so its own source hash
    // is unchanged.
    auto src = tmp_root("inctex_src");
    auto out = tmp_root("inctex_out");

    const auto glb = ve_test::make_glb({{"Paint", "paint.png", ""}});
    ve::core::cooked::write_file_bytes((std::filesystem::path(src) / "ship.glb").string(),
                                       glb, nullptr);
    auto write_tex = [&](unsigned char r, unsigned char b) {
        const unsigned char rgba[4] = {r, 0, b, 255};
        std::string werr;
        REQUIRE_MESSAGE(gfx_golden::write_png_rgba8(
                            (std::filesystem::path(src) / "paint.png").string(), rgba, 1, 1, werr),
                        werr);
    };
    write_tex(255, 0); // 1x1 red

    auto r1 = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(r1.ok, r1.error);
    CHECK(r1.counts.cooked == 2);

    ve::cook::Manifest man;
    std::string err;
    REQUIRE(ve::cook::read_manifest(out + "/manifest.bin", man, &err));
    REQUIRE(man.count("ship.glb"));
    CHECK(man["ship.glb"].deps == std::vector<std::string>({"paint.png"}));

    // edit ONLY the texture: glb source bytes identical -> hash-skip; the
    // preserved manifest entry keeps its dep edge.
    write_tex(0, 255); // 1x1 blue
    auto r2 = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(r2.ok, r2.error);
    CHECK(r2.counts.cooked == 1);
    CHECK(r2.counts.skipped == 1);

    REQUIRE(ve::cook::read_manifest(out + "/manifest.bin", man, &err));
    CHECK(man["ship.glb"].deps == std::vector<std::string>({"paint.png"}));

    // the re-cooked TEX0 carries the NEW pixels (w|h|fmt|rgba)
    ve::cook::Unit u;
    REQUIRE(ve::cook::read_unit_file(out + "/units/paint.png.ck", u, &err));
    REQUIRE(u.payload.size() == 12 + 4); // w|h|fmt (3 u32) + one RGBA pixel
    const auto px = u.payload.begin() + 12;
    CHECK(px[0] == 0);   // R: red -> blue
    CHECK(px[2] == 255);

    // and the glb unit is untouched: same MAT0 ref string after the edit
    REQUIRE(ve::cook::read_unit_file(out + "/units/ship.glb.ck", u, &err));
    std::string payload_str(u.payload.begin(), u.payload.end());
    CHECK(payload_str.find("paint.png") != std::string::npos);

    std::filesystem::remove_all(src);
    std::filesystem::remove_all(out);
}

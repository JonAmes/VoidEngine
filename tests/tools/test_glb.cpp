// test: glb_cook_poc — the Blender PoC hull (docs/tests/blender/report.md,
// copied to assets/samples/test_ship.glb) cooks into mesh + material +
// 5 socket records with socket names + extras preserved.
// Also: a def referencing an EXISTING texture records the dependency edge
// in the manifest (0.8 dep wiring); a def referencing a MISSING texture
// fails the cook cleanly (0.10 done-when bullet).
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <unistd.h> // getpid

#include <ve/cook/cook.h>

#ifndef VE_SOURCE_DIR
#error "VE_SOURCE_DIR must be defined by CMake"
#endif

namespace {

// Minimal reader for the kUnitMesh payload (layout documented in cooked.h).
struct Reader {
    const ve::cook::Bytes& b;
    std::size_t at = 0;
    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b[at + i]) << (8 * i);
        at += 4;
        return v;
    }
    float f32() {
        float f = 0;
        std::memcpy(&f, &b[at], 4);
        at += 4;
        return f;
    }
    std::string str() {
        const auto n = u32();
        std::string s(reinterpret_cast<const char*>(&b[at]), n);
        at += n;
        return s;
    }
};

std::string tmp_root(const char* name) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string("ve_glbt_") + name + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}

} // namespace

TEST_CASE("glb_cook_poc") {
    const std::string sample = std::string(VE_SOURCE_DIR) + "/assets/samples/test_ship.glb";
    REQUIRE_MESSAGE(std::filesystem::exists(sample), "missing " << sample);

    const std::string out = tmp_root("poc");
    auto res = ve::cook::cook_incremental(std::string(VE_SOURCE_DIR) + "/assets/samples", out);
    REQUIRE_MESSAGE(res.ok, res.error);
    REQUIRE(res.counts.cooked == 1);

    ve::cook::Unit u;
    std::string err;
    REQUIRE(ve::cook::read_unit_file(out + "/units/test_ship.glb.ck", u, &err));
    CHECK(u.type == ve::cook::kUnitMesh);

    Reader r2{u.payload};
    const auto mc2 = r2.u32();
    std::uint32_t tri_total = 0, mesh_total = 0;
    float extent[3] = {0, 0, 0};
    for (std::uint32_t m = 0; m < mc2; ++m) {
        (void)r2.str();
        (void)r2.u32();
        const auto vc = r2.u32();
        for (std::uint32_t v = 0; v < vc; ++v)
            for (int c = 0; c < 3; ++c)
                extent[c] = std::max(extent[c], std::abs(r2.f32()));
        r2.at += vc * 3 * 4 + vc * 2 * 4;
        const auto tc = r2.u32();
        tri_total += tc;
        ++mesh_total;
        r2.at += tc * 3 * 4;
    }

    // PoC report: 7 meshes (HullRoot carries mesh 6 too), 376 triangles total.
    CHECK(mesh_total == 7);
    CHECK(tri_total == 376);
    CHECK(extent[0] == doctest::Approx(3.6018f).epsilon(0.001f));
    CHECK(extent[1] == doctest::Approx(4.5f).epsilon(0.001f));
    CHECK(extent[2] == doctest::Approx(1.26f).epsilon(0.001f));

    // Baked-TRS bounds (identity Y-up->Y-up; axis convention decision still
    // pending, so assert magnitudes per axis, hand-derived from the export
    // tree): X 3.6018 (Wing_L: 2.4 + 2.2*0.5 + 2-deg rotation sliver),
    // Y 4.5 (Engine_L: -3.4 - 1.1), Z 1.26 (Cockpit: -0.72 - 0.6*0.9).

    // materials: 4; KHR emissiveStrength folded into the factor (decision #14).
    // engine_glow: raw emissive (1, .15, .05) x emissiveStrength 3 -> (3, .45, .15).
    const auto mat_count = r2.u32();
    CHECK(mat_count == 4);
    bool found_folded_emissive = false;
    for (std::uint32_t m = 0; m < mat_count; ++m) {
        const auto name = r2.str();
        for (int i = 0; i < 4; ++i) (void)r2.f32(); // base color
        (void)r2.f32(); // metallic
        (void)r2.f32(); // roughness
        const float e0 = r2.f32(), e1 = r2.f32(), e2 = r2.f32();
        (void)r2.u32(); // alpha mode
        if (name == "engine_glow")
            found_folded_emissive = e0 == doctest::Approx(3.0f) &&
                                    e1 == doctest::Approx(0.45f) &&
                                    e2 == doctest::Approx(0.15f);
    }
    CHECK(found_folded_emissive);

    // sockets: exactly the 5 authored hp_* empties, names + extras preserved.
    const auto socket_count = r2.u32();
    CHECK(socket_count == 5);
    std::vector<std::string> names;
    for (std::uint32_t s = 0; s < socket_count; ++s) {
        names.push_back(r2.str());
        (void)r2.f32(); (void)r2.f32(); (void)r2.f32(); // translation
        const auto extras = r2.str();
        CHECK(extras.find("socket_type") != std::string::npos);
        if (names.back() == "hp_dorsal_turret")
            CHECK(extras.find("dorsal") != std::string::npos);
        if (names.back() == "hp_engine_L")
            CHECK(extras.find("engine") != std::string::npos);
    }
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"hp_dorsal_turret", "hp_engine_L",
                                           "hp_engine_R", "hp_wingtip_L", "hp_wingtip_R"});

    // hull-level extras ride the root node.
    const auto root_extras = r2.str(); // str() consumes its own length prefix
    CHECK(root_extras.find("light_freighter") != std::string::npos);
    CHECK(root_extras.find("mass_t") != std::string::npos);
    CHECK(r2.at == u.payload.size()); // payload fully consumed: layout exact

    std::filesystem::remove_all(out);
}

TEST_CASE("def_dep_edge_and_missing_ref") {
    // existing-ref def: manifest carries the dependency (material/texture
    // dep wiring through defs, the path GLB texture deps will reuse later).
    const std::string src = tmp_root("deps");
    const std::string out = tmp_root("deps_out");
    std::filesystem::create_directories(std::filesystem::path(src) / "defs");
    {
        // a real 1x1 PNG so the ref target cooks as TEX0
        const unsigned char png[] = {
            0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R',
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15,
            0xc4, 0x89, 0x00, 0x00, 0x00, 0x0a, 'I', 'D', 'A', 'T', 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00,
            0x00, 0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 'I', 'E', 'N', 'D',
            0xae, 0x42, 0x60, 0x82};
        ve::cook::Bytes b(png, png + sizeof png);
        ve::core::cooked::write_file_bytes((std::filesystem::path(src) / "hue.png").string(), b, nullptr);
    }
    ve::core::cooked::write_file_bytes((std::filesystem::path(src) / "defs" / "ok.toml").string(),
            ve::cook::Bytes{'k','i','n','d',' ','=',' ','"','t','"','\n','r','e','f','s',' ','=',' ','[','"','.', '.', '/', 'h','u','e','.','p','n','g','"',']','\n'}, nullptr);

    auto res = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(res.ok, res.error);

    ve::cook::Manifest man;
    std::string err;
    REQUIRE(ve::cook::read_manifest(out + "/manifest.bin", man, &err));
    const auto it = man.find("defs/ok.toml");
    REQUIRE(it != man.end());
    REQUIRE(it->second.deps.size() == 1);
    CHECK(it->second.deps[0] == "hue.png"); // normalized id, dep edge recorded

    // missing-ref def: clean cook failure (no crash), error names the def + ref.
    ve::core::cooked::write_file_bytes((std::filesystem::path(src) / "defs" / "bad.toml").string(),
            ve::cook::Bytes{'k','i','n','d',' ','=',' ','"','t','"','\n','r','e','f','s',' ','=',' ','[','"','.', '.', '/', 'n','o','p','e','.','p','n','g','"',']','\n'}, nullptr);
    auto res2 = ve::cook::cook_incremental(src, out);
    CHECK_FALSE(res2.ok);
    CHECK(res2.counts.failures >= 1);
    CHECK(res2.error.find("nope.png") != std::string::npos);

    std::filesystem::remove_all(src);
    std::filesystem::remove_all(out);
}

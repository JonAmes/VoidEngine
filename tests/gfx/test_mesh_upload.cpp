// test: gfx_mesh_upload — 3.5. MESH payload parse (disk units from the real
// cooked test_ship.glb + a hand-built synthetic unit), loading strictly by
// content ID through the manifest (#13), corrupt units rejected cleanly,
// and GPU upload verified by mapped-buffer byte compare against payload.
#include <doctest/doctest.h>
#include <ve/gfx/device.h>
#include <ve/gfx/instance.h>
#include <ve/gfx/mesh.h>

#include "gfx_test_env.h"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <ve/cook/cook.h>
#include <ve/core/cooked.h>

namespace {
std::string tmp_root(const char* name) {
    static std::atomic<int> n{0};
    auto p = std::filesystem::temp_directory_path() /
             ("ve_mesh_" + std::string(name) + "_" + std::to_string(++n));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}

// Hand-build a MESH payload exactly per the cooked.h layout.
struct Sink {
    ve::core::cooked::Bytes b;
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF); }
    void f32(float f) { const auto* p = reinterpret_cast<const std::uint8_t*>(&f); b.insert(b.end(), p, p + 4); }
    void str(const std::string& s) { u32(static_cast<std::uint32_t>(s.size())); b.insert(b.end(), s.begin(), s.end()); }
};

// One triangle "Tri" with hand-picked vertex values; one material; no sockets.
ve::core::cooked::Bytes synth_payload() {
    Sink s;
    s.u32(1);                       // mesh_count
    s.str("Tri");
    s.u32(0);                       // material_index
    s.u32(3);                       // vertex_count
    const float pos[9] = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
    const float nrm[9] = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f};
    const float uv[6] = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f};
    for (float f : pos) s.f32(f);
    for (float f : nrm) s.f32(f);
    for (float f : uv) s.f32(f);
    s.u32(1);                       // tri_count
    s.u32(0); s.u32(1); s.u32(2);
    s.u32(1);                       // material_count
    s.str("Mat");
    s.f32(1.f); s.f32(0.f); s.f32(0.f); s.f32(1.f); // base_color
    s.f32(0.f); s.f32(0.5f);                         // metallic, roughness
    s.f32(0.f); s.f32(0.f); s.f32(0.f);              // emissive
    s.u32(0);                       // alpha_mode opaque
    s.u32(0);                       // socket_count
    s.u32(0);                       // root_extras_len
    return s.b;
}

// cooked_dir with manifest listing ids; unit per id (crc recomputed by
// write_unit_file, so payload edits survive as "corrupt but well-framed").
void write_unit_and_manifest(const std::string& cooked_dir,
                             const std::vector<std::pair<std::string, ve::core::cooked::Unit>>& units) {
    std::filesystem::create_directories(cooked_dir + "/units");
    ve::core::cooked::Manifest mf;
    std::string err;
    for (const auto& [id, u] : units) {
        REQUIRE(ve::core::cooked::write_unit_file(cooked_dir + "/units/" + id + ".ck", u, &err));
        mf[id] = ve::core::cooked::ManifestEntry{.id = id, .deps = {}, .source_sha = {}};
    }
    REQUIRE(ve::core::cooked::write_manifest(cooked_dir + "/manifest.bin", mf, &err));
}

ve::core::cooked::Unit mesh_unit(const std::string& id, ve::core::cooked::Bytes payload) {
    return ve::core::cooked::Unit{.id = id, .type = ve::core::cooked::kUnitMesh, .payload = std::move(payload)};
}
} // namespace

TEST_CASE("gfx_mesh_upload") {
    // ---- parse: the real cooked hull ------------------------------------------------
    const std::string sample_dir = std::string(VE_SOURCE_DIR) + "/assets/samples";
    REQUIRE_MESSAGE(std::filesystem::exists(sample_dir + "/test_ship.glb"), "missing test_ship.glb");
    const std::string ship_out = tmp_root("ship");
    REQUIRE(ve::cook::cook_incremental(sample_dir, ship_out).ok);

    const auto ship = ve::gfx::load_meshes_by_id(ship_out, "test_ship.glb");
    REQUIRE_MESSAGE(ship.has_value(), ship.error());
    CHECK(ship->size() == 7u); // PoC hull: 7 mesh nodes (glb_cook_poc pins mesh_total == 7)
    std::uint32_t total_tris = 0;
    for (const auto& m : *ship) {
        CHECK_FALSE(m.name.empty());
        CHECK_FALSE(m.vertices.empty());
        CHECK(m.indices.size() % 3u == 0u);
        for (const auto idx : m.indices) CHECK(idx < m.vertices.size()); // indices in range
        total_tris += static_cast<std::uint32_t>(m.indices.size() / 3u);
    }
    CHECK(total_tris > 0u);

    // manifest authority: a unit file that isn't in the manifest is NOT loadable.
    {
        const std::string dir = tmp_root("no_manifest");
        ve::core::cooked::Manifest empty_mf;
        std::string err;
        std::filesystem::create_directories(dir + "/units");
        REQUIRE(ve::core::cooked::write_unit_file(dir + "/units/ghost.glb.ck", mesh_unit("ghost.glb", synth_payload()), &err));
        REQUIRE(ve::core::cooked::write_manifest(dir + "/manifest.bin", empty_mf, &err));
        const auto g = ve::gfx::load_meshes_by_id(dir, "ghost.glb");
        CHECK_FALSE(g.has_value()); // not the crc — the manifest rejects it
    }

    // ---- synthetic unit: exact round-trip + corruption rejection --------------------
    const auto good = synth_payload();
    auto synth = ve::gfx::parse_mesh_payload(good);
    REQUIRE(synth.has_value());
    REQUIRE(synth->size() == 1u);
    CHECK((*synth)[0].name == "Tri");
    CHECK((*synth)[0].vertices.size() == 3u);
    CHECK((*synth)[0].indices == std::vector<std::uint32_t>{0, 1, 2});
    CHECK((*synth)[0].vertices[1].pos[0] == doctest::Approx(1.0f));
    CHECK((*synth)[0].vertices[2].pos[1] == doctest::Approx(1.0f));
    CHECK((*synth)[0].material_index == 0u);

    // truncation anywhere in the payload fails
    for (std::size_t cut : {good.size() - 1u, good.size() / 2u, std::size_t{4}}) {
        ve::core::cooked::Bytes t{good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut)};
        CHECK_FALSE(ve::gfx::parse_mesh_payload(t).has_value());
    }
    // garbage vertex_count: must fail BEFORE allocating anything
    {
        ve::core::cooked::Bytes t = good;
        // vertex_count u32 sits after: u32 mesh_count + u32 namelen + "Tri" + u32 matidx
        const std::size_t at = 4 + 4 + 3 + 4;
        t[at] = 0xFF; t[at + 1] = 0xFF; t[at + 2] = 0xFF; t[at + 3] = 0xFF;
        CHECK_FALSE(ve::gfx::parse_mesh_payload(t).has_value());
    }
    // corrupt material section (name_len -> huge): whole unit rejected even
    // though only meshes are consumed
    {
        ve::core::cooked::Bytes t = good;
        auto it = std::search(t.begin(), t.end(), std::string_view("Mat").begin(), std::string_view("Mat").end());
        REQUIRE(it != t.end());
        *(it - 4) = 0xFF; *(it - 3) = 0xFF; // name_len = 65535-ish
        CHECK_FALSE(ve::gfx::parse_mesh_payload(t).has_value());
    }

    // type mismatch through the manifest path
    {
        const std::string dir = tmp_root("wrongtype");
        ve::core::cooked::Unit tex{.id = "notmesh", .type = ve::core::cooked::kUnitTex0, .payload = synth_payload()};
        write_unit_and_manifest(dir, {{"notmesh", tex}});
        const auto r = ve::gfx::load_meshes_by_id(dir, "notmesh");
        CHECK_FALSE(r.has_value());
    }

    // ---- GPU upload: mapped readback byte-equals payload ----------------------------
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — GPU upload half skipped");
        return;
    }
    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = ve::gfx::device::create(*inst);
    REQUIRE(dev.has_value());

    for (const auto& src : {static_cast<const ve::gfx::cooked_mesh&>((*synth)[0]), ship->front()}) {
        auto gpu = ve::gfx::gpu_mesh::upload(*dev, src);
        REQUIRE_MESSAGE(gpu.has_value(), gpu.error());
        CHECK(gpu->vertex_count() == src.vertices.size());
        CHECK(gpu->index_count() == src.indices.size());
        CHECK(gpu->vertex_buffer() != VK_NULL_HANDLE);
        CHECK(gpu->index_buffer() != VK_NULL_HANDLE);
        // byte-identical: the mapped host view must equal the parsed payload
        CHECK(std::memcmp(gpu->mapped_vertices(), src.vertices.data(), src.vertices.size() * sizeof(ve::gfx::mesh_vertex)) == 0);
        CHECK(std::memcmp(gpu->mapped_indices(), src.indices.data(), src.indices.size() * sizeof(std::uint32_t)) == 0);
    }

    // empty mesh is a clean error, not a zero-size buffer
    ve::gfx::cooked_mesh empty_mesh{.name = "none", .material_index = 0xFFFFFFFFu, .vertices = {}, .indices = {}};
    CHECK_FALSE(ve::gfx::gpu_mesh::upload(*dev, empty_mesh).has_value());
}

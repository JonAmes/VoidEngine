// test: gfx_shader_module — cooked SHDR payload -> VkShaderModule on lvp;
// corrupt payloads rejected before Vulkan sees them.
#include <doctest/doctest.h>
#include <ve/gfx/shader.h>

#include "gfx_test_env.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <ve/cook/cook.h>

namespace {
std::string tmp_root(const char* name) {
    static std::atomic<int> n{0};
    auto p = std::filesystem::temp_directory_path() /
             ("ve_shdrmod_" + std::string(name) + "_" + std::to_string(++n));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
} // namespace

TEST_CASE("gfx_shader_module") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 shader test skipped");
        return;
    }

    // Produce the cooked unit the same way the game will: cooklet run over
    // sources, engine reads only the .ck payload (decision #13).
    const auto src = tmp_root("src");
    {
        std::ofstream f(std::filesystem::path(src) / "triangle.vert");
        f << "#version 450\n"
          << "void main() { gl_Position = vec4(0.0, 0.0, 0.5, 1.0); }\n";
    }
    const auto out = tmp_root("out");
    const auto cr = ve::cook::cook_incremental(src, out);
    REQUIRE(cr.ok);
    ve::cook::Unit unit;
    std::string err;
    REQUIRE(ve::cook::read_unit_file(out + "/units/triangle.vert.ck", unit, &err));
    REQUIRE(unit.type == ve::cook::kUnitShdr);

    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = ve::gfx::device::create(*inst);
    REQUIRE(dev.has_value());

    { // scoped: module must destroy cleanly (validation silent, ASan clean)
        auto mod = ve::gfx::shader_module::create(*dev, unit.payload);
        REQUIRE(mod.has_value());
        CHECK(mod->handle() != VK_NULL_HANDLE);
        CHECK(mod->stage() == ve::cook::kShdrStageVert);
        CHECK(mod->spirv_size() >= 20);
    }

    // Corrupt: bad magic must be a clean load error (no driver call).
    auto bad = unit.payload;
    bad[4] ^= 0xFFu;
    auto broken = ve::gfx::shader_module::create(*dev, bad);
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().find("magic") != std::string::npos);

    // Corrupt: truncated body rejected.
    auto shorty = unit.payload;
    shorty.resize(6);
    auto tshort = ve::gfx::shader_module::create(*dev, shorty);
    REQUIRE_FALSE(tshort.has_value());

    // Corrupt: unaligned body rejected.
    auto odd = unit.payload;
    odd.push_back(0u);
    auto todd = ve::gfx::shader_module::create(*dev, odd);
    REQUIRE_FALSE(todd.has_value());

    gfx_test::check_clean_validation(*inst);
}

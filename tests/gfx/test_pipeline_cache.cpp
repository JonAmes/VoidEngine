// test: gfx_pipeline_cache — 3.4. Same desc ⇒ same handle; any state or
// module/layout difference ⇒ a distinct live pipeline. Keys are
// byte-exact desc serializations (no hashing collisions by construction).
// Teardown: cache destructs (or clear()s) before the device — ASan/leak
// checks in the CI build cover the "destroyed before device" requirement.
#include <doctest/doctest.h>
#include <ve/gfx/pipeline_cache.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>

#include "gfx_test_env.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <ve/cook/cook.h>

namespace {
std::string tmp_root(const char* name) {
    static std::atomic<int> n{0};
    auto p = std::filesystem::temp_directory_path() /
             ("ve_pcache_" + std::string(name) + "_" + std::to_string(++n));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << contents;
}
constexpr const char* kVert = R"GLSL(#version 450
void main() { gl_Position = vec4(0.0, 0.0, 0.5, 1.0); }
)GLSL";
constexpr const char* kFrag = R"GLSL(#version 450
layout(location = 0) out vec4 c;
void main() { c = vec4(1.0); }
)GLSL";
} // namespace

TEST_CASE("gfx_pipeline_cache") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 pipeline cache test skipped");
        return;
    }

    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = ve::gfx::device::create(*inst);
    REQUIRE(dev.has_value());

    // Two independently-cooked, byte-identical shader pairs: modules a/b have
    // identical code but distinct handles — the key must separate them.
    const auto src_a = tmp_root("src_a");
    const auto src_b = tmp_root("src_b");
    // cook_shader writes sources; cook both dirs fully so both units exist.
    write_file(std::filesystem::path(src_a) / "s.vert", kVert);
    write_file(std::filesystem::path(src_a) / "s.frag", kFrag);
    write_file(std::filesystem::path(src_b) / "s.vert", kVert);
    write_file(std::filesystem::path(src_b) / "s.frag", kFrag);
    const auto out_a = tmp_root("out_a");
    const auto out_b = tmp_root("out_b");
    REQUIRE(ve::cook::cook_incremental(src_a, out_a).ok);
    REQUIRE(ve::cook::cook_incremental(src_b, out_b).ok);
    ve::cook::Unit a_v, a_f, b_v, b_f;
    std::string err;
    REQUIRE(ve::cook::read_unit_file(out_a + "/units/s.vert.ck", a_v, &err));
    REQUIRE(ve::cook::read_unit_file(out_a + "/units/s.frag.ck", a_f, &err));
    REQUIRE(ve::cook::read_unit_file(out_b + "/units/s.vert.ck", b_v, &err));
    REQUIRE(ve::cook::read_unit_file(out_b + "/units/s.frag.ck", b_f, &err));

    auto av = ve::gfx::shader_module::create(*dev, a_v.payload);
    auto af = ve::gfx::shader_module::create(*dev, a_f.payload);
    auto bv = ve::gfx::shader_module::create(*dev, b_v.payload);
    auto bf = ve::gfx::shader_module::create(*dev, b_f.payload);
    REQUIRE(av.has_value());
    REQUIRE(af.has_value());
    REQUIRE(bv.has_value());
    REQUIRE(bf.has_value());

    const VkPushConstantRange pcr{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = 64,
    };
    const VkPipelineLayoutCreateInfo lci0{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .setLayoutCount = 0, .pSetLayouts = nullptr,
        .pushConstantRangeCount = 0, .pPushConstantRanges = nullptr,
    };
    const VkPipelineLayoutCreateInfo lci1{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .setLayoutCount = 0, .pSetLayouts = nullptr,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr,
    };
    VkPipelineLayout layout0 = VK_NULL_HANDLE, layout1 = VK_NULL_HANDLE;
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &lci0, nullptr, &layout0) == VK_SUCCESS);
    REQUIRE(vkCreatePipelineLayout(dev->handle(), &lci1, nullptr, &layout1) == VK_SUCCESS);

    const VkFormat bgra = VK_FORMAT_B8G8R8A8_UNORM;
    const VkFormat d32 = ve::gfx::offscreen_presenter::kDepthFormat;

    ve::gfx::pipeline_desc base{};
    base.layout = layout0;
    base.stages = {
        {VK_SHADER_STAGE_VERTEX_BIT, av->handle(), "main"},
        {VK_SHADER_STAGE_FRAGMENT_BIT, af->handle(), "main"},
    };
    base.color_formats = {bgra};
    base.depth_format = d32;
    base.blends = {}; // attachment-less default desc gets pushed below
    base.blends.emplace_back();
    base.dynamic_mask = (1u << VK_DYNAMIC_STATE_VIEWPORT) | (1u << VK_DYNAMIC_STATE_SCISSOR);

    {
        // Scope: cache must die before dev (declared after => destructs first).
        ve::gfx::pipeline_cache cache(*dev);

        // --- same desc, twice: one handle, one entry -----------------------
        auto p1 = cache.get(base);
        REQUIRE(p1.has_value());
        CHECK(*p1 != VK_NULL_HANDLE);
        auto p2 = cache.get(base);
        REQUIRE(p2.has_value());
        CHECK(*p2 == *p1); // identity, not just validity
        CHECK(cache.size() == 1u);
        CHECK(base.key() == base.key()); // key() deterministic across calls

        // --- differences that MUST fork the key ----------------------------
        std::set<VkPipeline> pipes{*p1};
        auto distinct = [&](ve::gfx::pipeline_desc d) {
            CHECK(d.key() != base.key());
            auto p = cache.get(d);
            REQUIRE_MESSAGE(p.has_value(), d.key().size());
            CHECK(*p != VK_NULL_HANDLE);
            for (const auto seen : pipes) CHECK(*p != seen);
            pipes.insert(*p);
        };

        auto d = base; d.cull = VK_CULL_MODE_NONE;                distinct(d);
        d = base; d.depth_compare = VK_COMPARE_OP_LESS;           distinct(d);
        d = base; d.layout = layout1;                             distinct(d); // layout fork
        d = base; d.stages[0].module = bv->handle();              distinct(d); // module fork
        d = base; d.stages[1].module = bf->handle();              distinct(d);
        d = base; d.blends[0].blend_enable = true;                distinct(d);
        d = base; d.blends[0].write_mask = VK_COLOR_COMPONENT_R_BIT; distinct(d);
        d = base; d.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;   distinct(d);
        d = base; d.samples = VK_SAMPLE_COUNT_4_BIT;              distinct(d);
        d = base; d.front = VK_FRONT_FACE_CLOCKWISE;              distinct(d);
        d = base; d.line_width = 2.0f;                            distinct(d);
        d = base; d.stencil = true;                               distinct(d);
        d = base; d.depth_test = false;                           distinct(d);
        d = base; d.view_mask = 0b11u;                            distinct(d); // per-eye multiview bits
        d = base; d.color_formats = {VK_FORMAT_R8G8B8A8_UNORM};   distinct(d);

        // dynamic-state presence also forks (bit added)
        d = base; d.dynamic_mask |= (1u << VK_DYNAMIC_STATE_LINE_WIDTH); distinct(d);

        CHECK(cache.size() == pipes.size());

        // --- re-request a forked desc: hits its own entry, no growth -------
        auto again = cache.get(d);
        REQUIRE(again.has_value());
        CHECK(pipes.count(*again) == 1u);
        CHECK(cache.size() == pipes.size());

        // --- clear: all pipelines destroyed; re-get recreates --------------
        cache.clear();
        CHECK(cache.size() == 0u);
        auto p3 = cache.get(base);
        REQUIRE(p3.has_value());
        CHECK(*p3 != VK_NULL_HANDLE);
        CHECK(cache.size() == 1u);
    } // cache destructed here: every pipeline gone before device teardown

    vkDestroyPipelineLayout(dev->handle(), layout0, nullptr);
    vkDestroyPipelineLayout(dev->handle(), layout1, nullptr);
}

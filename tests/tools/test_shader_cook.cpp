// test: cook_shader_golden — GLSL -> SHDR cooked unit; two compiles of the
// same source are byte-identical; a missing tool is a clean cook failure
// (no crash, actionable message).
#include <doctest/doctest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <ve/cook/cook.h>
#include "../ve_test_portability.h" // set_env/clear_env

namespace {
std::string tmp_root(const char* name) {
    static std::atomic<int> n{0};
    auto p = std::filesystem::temp_directory_path() /
             ("ve_shdr_" + std::string(name) + "_" + std::to_string(++n));
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
layout(location = 0) out vec2 uv;
void main() {
    vec2 p[3] = vec2[3](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.0, 0.5));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
    uv = p[gl_VertexIndex];
}
)GLSL";
constexpr const char* kFrag = R"GLSL(#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() { color = vec4(uv, 0.5, 1.0); }
)GLSL";
} // namespace

TEST_CASE("cook_shader_golden") {
    // The GLSL compiler is an external tool, so its absence is an environment
    // gap rather than a cooklet fault — same tiering as the gfx suites' ICD
    // probe (#36). The missing-tool PATH is still asserted unconditionally at the
    // bottom of this case, with VE_GLSLANG pointed at a nonexistent file, so
    // gating the golden on tool presence loses no coverage of the failure path.
    const std::string env_tool = [] {
        const char* v = std::getenv("VE_GLSLANG");
        return (v != nullptr && *v != '\0') ? std::string(v) : std::string("glslangValidator");
    }();
    if (!ve_test::tool_on_path(env_tool)) {
        MESSAGE("no GLSL compiler reachable ('" << env_tool
                << "') — golden skipped; install glslang-tools or point VE_GLSLANG at one");

        return;
    }

    auto src = tmp_root("src");

    write_file(std::filesystem::path(src) / "triangle.vert", kVert);
    write_file(std::filesystem::path(src) / "triangle.frag", kFrag);

    auto out1 = tmp_root("out1");
    auto r1 = ve::cook::cook_incremental(src, out1);
    REQUIRE(r1.ok);
    CHECK(r1.counts.cooked == 2);

    ve::cook::Unit u1;
    std::string err;
    REQUIRE(ve::cook::read_unit_file(out1 + "/units/triangle.vert.ck", u1, &err));
    CHECK(u1.type == ve::cook::kUnitShdr);
    REQUIRE(u1.payload.size() > 24); // stage word + SPIR-V header minimum
    CHECK(u1.payload[0] == ve::cook::kShdrStageVert);
    // SPIR-V magic 0x07230203 LE: 03 02 23 07
    CHECK(u1.payload[4] == 0x03u);
    CHECK(u1.payload[5] == 0x02u);
    CHECK(u1.payload[6] == 0x23u);
    CHECK(u1.payload[7] == 0x07u);
    CHECK((u1.payload.size() - 4) % 4u == 0u);

    ve::cook::Unit f1;
    REQUIRE(ve::cook::read_unit_file(out1 + "/units/triangle.frag.ck", f1, &err));
    CHECK(f1.type == ve::cook::kUnitShdr);
    CHECK(f1.payload[0] == ve::cook::kShdrStageFrag);

    // Determinism: separate cook out of the same sources == byte-identical.
    auto out2 = tmp_root("out2");
    auto r2 = ve::cook::cook_incremental(src, out2);
    REQUIRE(r2.ok);
    ve::cook::Unit u2;
    REQUIRE(ve::cook::read_unit_file(out2 + "/units/triangle.vert.ck", u2, &err));
    CHECK(u1.payload == u2.payload);

    // Missing tool: clean failure naming the shader, no crash, cook not ok.
    ve_test::set_env("VE_GLSLANG", "/nonexistent/glslangValidator-ve-test");
    auto out3 = tmp_root("out3");
    auto r3 = ve::cook::cook_incremental(src, out3);
    ve_test::clear_env("VE_GLSLANG");
    CHECK_FALSE(r3.ok);
    CHECK(r3.counts.failures == 2);
    CHECK(r3.error.find("triangle.vert") != std::string::npos);
}

#include "shader.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// popen/_popen is the only process call we need (the cooklet is a dev tool;
// the engine runtime never spawns processes). Wait-status semantics differ
// per platform, so success is decided by (a) a readable, magic-carrying
// SPIR-V file — never by status bit twiddling (WEXITSTATUS would need
// <sys/wait.h>, POSIX-only, off-limits under #38). A missing tool yields
// empty output from the shell and no file, which is the clean failure path.
int run_tool(const std::string& cmd, std::string& output) {
#ifdef _WIN32
    // One extra quote pair around the whole line, and it is load-bearing.
    // _popen hands cmd.exe a command line that is ALREADY wrapped in quotes, and
    // this command starts with the quoted tool path. cmd's /c parser then
    // consumes a quote pair of its own, leaving the tool path and the -o target
    // mismatched: measured on MSVC, the shell answered "The filename, directory
    // name, or volume label syntax is incorrect." and glslang never ran — which
    // surfaced as "produced no SPIR-V (not found, or GLSL error)", i.e. a real
    // Windows bug wearing an env-problem costume. One more pair survives the
    // strip. The test suite hit the identical quirk (tests/ve_test_portability.h,
    // shell_cmd) — this is the production-side counterpart.
    FILE* p = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (p == nullptr) {
        return -1;
    }
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), p) != nullptr) {
        output += buf;
    }
#ifdef _WIN32
    return _pclose(p);
#else
    return pclose(p);
#endif
}

} // namespace

bool compile_glsl(const std::string& src_path, ve::core::cooked::Bytes& spirv_out,
                  std::string* err) {
    const auto fail = [&](const std::string& msg) {
        if (err != nullptr) {
            *err = msg;
        }
        return false;
    };

    const char* env_tool = std::getenv("VE_GLSLANG");
    const std::string tool = (env_tool != nullptr && *env_tool != '\0') ? env_tool : "glslangValidator";

    // Temp name + lifetime are load-bearing, not cosmetics.
    //
    // A process-local counter alone restarts at 1 in EVERY process, so two runs
    // of the cooklet pick the same temp name. The hole that opens is silent and
    // nasty: if the tool run fails to write (missing PATH entry, GLSL error,
    // quoting), spv_exists still answers yes from the PREVIOUS run's leftover,
    // and the magic/size checks pass — so the unit gets valid SPIR-V for the
    // WRONG source. Measured here: a demo frame loop drew a stale triangle, and
    // the only reason it looked like a GPU bug is that the stale bytes were
    // indistinguishable from a working compile. That breaks the cooklet's core
    // promise (same sources in, byte-identical cooked bytes out).
    //
    // So: the name is keyed by the source path (two processes cooking different
    // files never collide, and a parallel build cannot trample), and the file is
    // removed BEFORE the tool runs, which makes "the tool wrote nothing" the only
    // way the read path can be reached.
    static std::uint64_t counter = 0;
    const auto spv_path = std::filesystem::temp_directory_path() /
                          ("ve_glslang_" + std::to_string(std::filesystem::hash_value(src_path)) +
                           "_" + std::to_string(++counter) + ".spv");
    std::error_code remove_ec;
    std::filesystem::remove(spv_path, remove_ec); // no stale bytes may survive into the read

    // -V = Vulkan GLSL semantics; --quiet drops the stage banner; binary
    // SPIR-V out. Stage comes from the source extension.
    const std::string cmd = "\"" + tool + "\" -V --quiet -o \"" + spv_path.string() +
                            "\" \"" + src_path + "\" 2>&1";

    std::string tool_output;
    if (run_tool(cmd, tool_output) == -1) {
        return fail("could not launch '" + tool + "' (install glslang-tools, or set VE_GLSLANG)");
    }

    std::error_code ec;
    const bool spv_exists = std::filesystem::exists(spv_path, ec);
    ve::core::cooked::Bytes spirv;
    if (spv_exists) {
        std::ifstream f(spv_path, std::ios::binary);
        spirv.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        std::filesystem::remove(spv_path, ec);
    }

    if (!spv_exists || spirv.size() < 20) {
        return fail("'" + tool + "' produced no SPIR-V (not found, or GLSL error)" +
                    (tool_output.empty() ? "" : (": " + tool_output)));
    }
    // SPIR-V magic 0x07230203 stored little-endian -> bytes 03 02 23 07.
    if (spirv[0] != 0x03u || spirv[1] != 0x02u || spirv[2] != 0x23u || spirv[3] != 0x07u) {
        return fail("output is not SPIR-V (bad magic)");
    }
    if ((spirv.size() % 4u) != 0u) {
        return fail("SPIR-V length not word-aligned");
    }

    spirv_out = std::move(spirv);
    return true;
}

// ve_demo — checklist 2.9: the windowed rotating triangle + capture evidence
// (decision #36). This is the first thing in the repo that RUNS the WSI
// presenter: GLFW window (platform seam) -> VkSurfaceKHR -> swapchain (FIFO) ->
// render pass into the presenter's own render target -> blit RT -> swapchain
// image. The seam from 2.4 holds untouched: this file renders into
// pres->color_view() and never learns what a swapchain is.
//
// NOT a ctest case — it needs a display, and ctest must not (AGENTS). On the dev
// box tools/dev/xvfb_run.sh drives it under Xvfb + lavapipe and drops the
// evidence into docs/phase-02-vulkan/evidence/. Decision #38 keeps it buildable
// on MSVC, which is also how it was first run (a real window on a real RTX 4060).
//
// Exit codes make the evidence self-attesting:
//   0  N frames presented, the two captures DIFFER (it really spun), validation silent
//   2  setup failed (window / instance / surface / device / swapchain / cook) — reason printed
//   3  the validation layer reported errors or warnings (2.7's policy, enforced app-side)
//   4  the two captures are identical — the spin is not actually spinning
#include <ve/core/log.h>
#include <ve/core/math/mat4.h>
#include <ve/core/math/quat.h>
#include <ve/cook/cook.h>
#include <ve/demo/png_out.h>
#include <ve/gfx/device.h>
#include <ve/gfx/instance.h>
#include <ve/gfx/presenter.h>
#include <ve/gfx/shader.h>
#include <ve/gfx/wsi_presenter.h>
#include <ve/platform/window.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* kUsage =
    "usage: ve_demo [--frames N] [--width W] [--height H] [--shaders DIR]\n"
    "               [--cook DIR] [--out DIR] [--resize-at FRAME]\n"
    "  --out DIR also writes evidence: frame PNGs, frames.csv, summary.txt\n"
    "  --resize-at N forces a window resize at frame N (swapchain rebuild proof);\n"
    "  --resize-at 0 disables it. Default: 2/3 through the run.\n"
    "  --alternate-clear: diagnostic. Clear even/odd frames with two different\n"
    "  colors, which separates a static render target from a stale readback.\n";

// The push-constant block: one mat4, same contract 2.6 pinned (the vertex shader
// declares 'layout(push_constant) uniform Push { mat4 mvp; }').
struct PushMvp {
    float m[16];
};

std::int64_t abs_diff(std::uint8_t a, std::uint8_t b) {
    return static_cast<std::int64_t>(a > b ? a - b : b - a);
}

// How different two captures are. This is the "frames differ (rotation)" check
// 2.9 asks for, computed here rather than eyeballed later: mean absolute
// difference over every byte plus the share of bytes that changed at all.
struct capture_diff {
    double mean_abs = 0.0;
    double changed_pct = 0.0;
    std::size_t bytes = 0;
};

capture_diff compare_captures(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    capture_diff d;
    if (a.size() != b.size() || a.empty()) {
        d.mean_abs = -1.0; // size mismatch: not comparable, caller must treat as failure
        return d;
    }
    std::int64_t sum = 0;
    std::size_t changed = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const std::int64_t delta = abs_diff(a[i], b[i]);
        sum += delta;
        changed += delta != 0 ? 1u : 0u;
    }
    d.bytes = a.size();
    d.mean_abs = static_cast<double>(sum) / static_cast<double>(a.size());
    d.changed_pct = 100.0 * static_cast<double>(changed) / static_cast<double>(a.size());
    return d;
}

// A capture's fingerprint. Comparing ONE pair (first vs last) cannot tell three
// very different failures apart, and 2.9's "frames differ (rotation)" bullet is
// worthless if it cannot say which one happened:
//   - every probe identical  -> the render target never changed (the spin is not
//                              reaching the image, or the readback is stale)
//   - probes differ, first==last -> the spin ran but wrapped to the same pose
//   - off_background == 0    -> the capture is one flat color: nothing rendered
// mean/off_background come from the same pass, so this costs nothing extra.
struct capture_probe {
    std::uint32_t frame = 0;
    std::uint64_t hash = 0;
    double mean = 0.0;         // average byte value: 0 means an empty capture
    std::uint64_t off_background = 0; // pixels not equal to the top-left pixel
    std::uint64_t pixels = 0;
};

capture_probe probe_capture(std::uint32_t frame, const std::vector<std::uint8_t>& px) {
    capture_probe p;
    p.frame = frame;
    if (px.empty()) {
        return p;
    }
    std::uint64_t h = 146959810393466560ULL; // FNV-1a 64: same hash family as the cooklet
    std::uint64_t sum = 0;
    for (const std::uint8_t byte : px) {
        h ^= byte;
        h *= 1099511628211ULL;
        sum += byte;
    }
    p.hash = h;
    p.mean = static_cast<double>(sum) / static_cast<double>(px.size());
    if (px.size() >= 4u) {
        for (std::size_t i = 0; i + 3u < px.size(); i += 4u) {
            const bool flat = px[i] == px[0] && px[i + 1] == px[1] && px[i + 2] == px[2] &&
                              px[i + 3] == px[3];
            p.off_background += flat ? 0u : 1u;
            ++p.pixels;
        }
    }
    return p;
}

// Unit files live at <out>/units/<id>.ck. Built from the shader file names, with
// a directory scan as the fallback so a cooklet naming change cannot silently
// strand the demo on a stale path.
std::filesystem::path find_unit(const std::filesystem::path& units_dir, const std::string& prefix) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(units_dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0 && name.size() > 3 && name.substr(name.size() - 3) == ".ck") {
            return entry.path();
        }
    }
    return {};
}

// The 2.6 pipeline, same shape: dynamic viewport+scissor (so a resize never
// rebuilds it), no vertex input (gl_VertexIndex triangle), no depth, VK1.3
// dynamic rendering (no VkRenderPass object, decision #37). Duplicated rather
// than shared through a header on purpose: gfx/ exposes no pipeline builder, and
// inventing one inside the engine for a demo's convenience would put a
// demo-shaped API in front of the real renderer.
VkPipeline build_pipeline(const ve::gfx::device& dev, VkPipelineLayout layout, VkShaderModule vert,
                          VkShaderModule frag, VkFormat color_format) {
    const VkPipelineShaderStageCreateInfo stages[2]{
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
    };
    const VkPipelineVertexInputStateCreateInfo vi{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = 0,
        .pVertexBindingDescriptions = nullptr,
        .vertexAttributeDescriptionCount = 0,
        .pVertexAttributeDescriptions = nullptr,
    };
    const VkPipelineInputAssemblyStateCreateInfo ia{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };
    const VkPipelineViewportStateCreateInfo vp{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .viewportCount = 1,
        .pViewports = nullptr, // dynamic
        .scissorCount = 1,
        .pScissors = nullptr, // dynamic
    };
    const VkPipelineRasterizationStateCreateInfo rs{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0.0f,
        .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = 0.0f,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo ms{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 1.0f,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };
    const VkPipelineDepthStencilStateCreateInfo ds{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthTestEnable = VK_FALSE,
        .depthWriteEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
        .front = {},
        .back = {},
        .minDepthBounds = 0.0f,
        .maxDepthBounds = 1.0f,
    };
    const VkPipelineColorBlendAttachmentState blend{
        .blendEnable = VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo cb{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &blend,
        .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
    };
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dsy{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = 2,
        .pDynamicStates = dyn,
    };
    // Value-init + assignment, never designated init: these structs gain fields
    // across SDK header revisions (the 1.4 resolveMode lesson from 2.6, and
    // MSVC's headers are not GCC's — decision #38).
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.viewMask = 0;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;
    rendering.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    rendering.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.pNext = &rendering;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dsy;
    pci.layout = layout;
    pci.renderPass = VK_NULL_HANDLE; // dynamic rendering (VK1.3)
    pci.subpass = 0;
    pci.basePipelineIndex = -1;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev.handle(), VK_NULL_HANDLE, 1, &pci, nullptr, &pipe) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return pipe;
}
} // namespace

// CMake defines VE_SOURCE_DIR (the same trick tests/ uses) so the demo finds
// assets/shaders with no flags; the fallback keeps the file buildable outside
// CMake, which is how it was first compile-checked on MSVC (#38).
#ifndef VE_SOURCE_DIR
#define VE_SOURCE_DIR "."
#endif

namespace {

std::uint32_t parse_u32(const char* text, bool& ok) {
    std::uint32_t value = 0;
    const auto res = std::from_chars(text, text + std::strlen(text), value);
    ok = res.ec == std::errc{} && res.ptr != text && *res.ptr == '\0';
    return value;
}

const char* format_name(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: return "VK_FORMAT_B8G8R8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB: return "VK_FORMAT_B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "VK_FORMAT_R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB: return "VK_FORMAT_R8G8B8A8_SRGB";
    default: return "VK_FORMAT(<other>)";
    }
}

// One frame = one command buffer: the pass renders into the presenter's render
// target, then the presenter blits that target into the swapchain image it
// acquired and presents. Byte-for-byte the same shape as the 2.6 golden test's
// frame, which is the whole point of the seam: headless golden and windowed
// demo differ ONLY in which presenter object stands in front.
//
// Ownership: the command buffer belongs to the presenter's pool, so the demo
// never frees it. present() submits it and the next acquire() returns it; on the
// failure paths here the buffer is left to ~wsi_presenter, whose
// vkDestroyCommandPool reclaims everything the pool ever handed out.
bool record_frame(ve::gfx::wsi_presenter& pres, VkPipeline pipe, VkPipelineLayout layout,
                  std::uint32_t width, std::uint32_t height, float angle_rad,
                  const float (&clear_rgba)[4], PushMvp* echo = nullptr) {
    VkCommandBuffer cmd = pres.begin_command();
    if (cmd == VK_NULL_HANDLE) {
        VE_LOG_ERROR("demo: no command buffer for this frame");
        return false;
    }
    if (!pres.begin_pass(cmd)) {
        VE_LOG_ERROR("demo: begin_pass refused the frame");
        return false;
    }

    // Value-init + assignment (the resolveMode lesson from 2.6, #38): designated
    // init pins this code to one header revision.
    VkRenderingAttachmentInfo att{};
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = pres.color_view(); // the seam: a render target, never a backbuffer
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color.float32[0] = clear_rgba[0];
    att.clearValue.color.float32[1] = clear_rgba[1];
    att.clearValue.color.float32[2] = clear_rgba[2];
    att.clearValue.color.float32[3] = clear_rgba[3];

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {width, height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);

    // Viewport+scissor are DYNAMIC in the pipeline, so a resize rebuilds the
    // swapchain and the target but never this pipeline (2.8's resize note).
    const VkViewport vpt{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &vpt);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

    // The spin: one push constant per frame, no buffer rebuild. Aspect-corrected
    // so a 16:9 window does not shear the hull (Phase 1 rule #34: the app owns
    // the projection; core/math has no opinion about viewports).
    const ve::core::math::mat4f rot = ve::core::math::to_mat4(ve::core::math::quat_roll(angle_rad));
    ve::core::math::mat4f aspect = ve::core::math::mat4f::zero();
    aspect.m[0] = static_cast<float>(height) / static_cast<float>(width);
    aspect.m[5] = 1.0f;
    aspect.m[10] = 1.0f;
    aspect.m[15] = 1.0f;
    const ve::core::math::mat4f mvp = rot * aspect;

    PushMvp push{};
    std::memcpy(push.m, mvp.m.data(), sizeof(push.m)); // column-major straight into GLSL's mat4
    if (echo != nullptr) {
        *echo = push; // the loop logs these: proof the CPU side of the spin varies
    }
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    if (!pres.end_pass(cmd)) {
        VE_LOG_ERROR("demo: end_pass refused the frame");
        return false;
    }
    return pres.present(cmd);
}

} // namespace

// The surface belongs to the presenter's caller (documented in wsi_presenter.h)
// and must die before the VkInstance. Declaring this owner BETWEEN the instance
// and the device/presenter makes C++ scope-exit order get it right on every
// return path: the presenter (declared later) dies first, so its swapchain is
// gone before the surface it was built from.
struct surface_owner {
    const ve::gfx::instance* inst = nullptr;
    VkSurfaceKHR* handle = nullptr;
    ~surface_owner() {
        if (inst != nullptr && handle != nullptr && *handle != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(inst->handle(), *handle, nullptr);
            *handle = VK_NULL_HANDLE;
        }
    }
};

// Pipeline + layout live on the logical device, so they must die before it does.
// Declaring this right after they are built means every later return path
// (including the frame loop's failure exits) pays for the destroy exactly once.
struct pipeline_guard {
    VkDevice dev = VK_NULL_HANDLE;
    VkPipeline* pipe = nullptr;
    VkPipelineLayout* layout = nullptr;
    ~pipeline_guard() {
        if (dev == VK_NULL_HANDLE) {
            return;
        }
        if (pipe != nullptr && *pipe != VK_NULL_HANDLE) {
            vkDestroyPipeline(dev, *pipe, nullptr);
            *pipe = VK_NULL_HANDLE;
        }
        if (layout != nullptr && *layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(dev, *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
    }
};

// Every early-exit path pays for closing the log, so the exit code is set here
// once and the cleanup happens exactly once too.
int finish(int code) {
    ve::core::log_close();
    return code;
}

int main(int argc, char** argv) {
    std::uint32_t frames = 120;
    std::uint32_t width = 640;
    std::uint32_t height = 360;
    // Where the run asks the window to change size. 0 = never. Resolved after the
    // argument loop, because it is expressed as a fraction of --frames.
    std::uint32_t resize_at = 0;
    bool resize_at_set = false;
    bool alternate_clear = false;
    std::string shaders_dir;
    std::string cook_dir;
    std::string out_dir;
    // Filled once a device exists; printed in summary.txt as run evidence.
    std::string device_line;
    std::string ext_line;
    std::string tool_line;      // layers/tools the loader reports as intercepting
    bool layer_is_active = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::fputs(kUsage, stdout);
            return 0;
        }
        if (arg == "--alternate-clear") { // flag: takes no value
            alternate_clear = true;
            continue;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "%s needs a value\n%s", arg.c_str(), kUsage);
            return 2;
        }
        const std::string value = argv[++i];
        if (arg == "--frames" || arg == "--width" || arg == "--height" || arg == "--resize-at") {
            bool ok = false;
            const std::uint32_t v = parse_u32(value.c_str(), ok);
            if (!ok) {
                std::fprintf(stderr, "%s: '%s' is not a non-negative integer\n", arg.c_str(),
                             value.c_str());
                return 2;
            }
            if (arg == "--frames") {
                frames = v;
            } else if (arg == "--width") {
                width = v;
            } else if (arg == "--height") {
                height = v;
            } else {
                resize_at = v;
                resize_at_set = true;
            }
        } else if (arg == "--shaders") {
            shaders_dir = value;
        } else if (arg == "--cook") {
            cook_dir = value;
        } else if (arg == "--out") {
            out_dir = value;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n%s", arg.c_str(), kUsage);
            return 2;
        }
    }
    if (frames < 2u || width == 0u || height == 0u) {
        std::fputs("need --frames >= 2 and a non-zero --width/--height\n", stderr);
        return 2;
    }
    if (!resize_at_set) {
        resize_at = (frames * 2u) / 3u; // 0 only when --frames is tiny
    }

    std::error_code ec;
    const std::filesystem::path shaders = shaders_dir.empty()
        ? std::filesystem::path(VE_SOURCE_DIR) / "assets" / "shaders"
        : std::filesystem::path(shaders_dir);
    const std::filesystem::path out =
        out_dir.empty() ? std::filesystem::path{} : std::filesystem::path(out_dir);
    std::filesystem::path cook;
    if (!cook_dir.empty()) {
        cook = std::filesystem::path(cook_dir);
    } else {
        std::error_code tec;
        cook = (out.empty() ? std::filesystem::temp_directory_path(tec) : out) / "cook";
    }
    if (!out.empty()) {
        std::filesystem::create_directories(out, ec);
    }
    std::filesystem::create_directories(cook, ec);

    std::string log_path;
    if (!out.empty()) {
        // Not ".log": the repo gitignores *.log, and this file is committed
        // evidence for 2.9, not scratch.
        log_path = (out / "demo_log.txt").string();
    }
    ve::core::log_init(ve::core::LogLevel::info, log_path.empty() ? nullptr : log_path.c_str());

    const auto fail = [](const std::string& why) {
        std::fprintf(stderr, "ve_demo: %s\n", why.c_str());
        VE_LOG_ERROR("demo: %s", why.c_str());
    };

    // --- 1. shaders are cooked, never compiled here (decision #13) ---------
    if (!std::filesystem::is_directory(shaders, ec)) {
        fail("shader source directory not found: " + shaders.string());
        return finish(2);
    }
    const auto cooked = ve::cook::cook_incremental(shaders.string(), cook.string());
    if (!cooked.ok) {
        fail("cook failed: " + cooked.error +
             "   (GLSL compiler: glslangValidator on PATH, or point VE_GLSLANG at one)");
        return finish(2);
    }
    VE_LOG_INFO("demo: cooked %d, unchanged %d -> %s", cooked.counts.cooked, cooked.counts.skipped,
                cook.string().c_str());

    const std::filesystem::path units_dir = cook / "units";
    const std::filesystem::path vert_path = find_unit(units_dir, "demo_tri.vert");
    const std::filesystem::path frag_path = find_unit(units_dir, "demo_tri.frag");
    if (vert_path.empty() || frag_path.empty()) {
        fail("no cooked demo_tri.vert/.frag unit under " + units_dir.string());
        return finish(2);
    }
    ve::core::cooked::Unit vert_unit;
    ve::core::cooked::Unit frag_unit;
    std::string err;
    if (!ve::core::cooked::read_unit_file(vert_path.string(), vert_unit, &err) ||
        !ve::core::cooked::read_unit_file(frag_path.string(), frag_unit, &err)) {
        fail("reading a cooked unit: " + err);
        return finish(2);
    }
    if (vert_unit.type != ve::core::cooked::kUnitShdr || frag_unit.type != ve::core::cooked::kUnitShdr) {
        fail("a demo shader unit is not SHDR");
        return finish(2);
    }

    // --- 2. platform seam: window -> extensions -> instance -> surface -----
    auto win = ve::platform::window::create(static_cast<int>(width), static_cast<int>(height),
                                            "void-engine 2.9 - rotating triangle");
    if (!win) {
        fail(win.error());
        return finish(2);
    }
    // The FRAMEBUFFER size is what a swapchain must match: HiDPI scaling and
    // window managers both get to disagree with the requested size.
    if (const auto [fw0, fh0] = win->frame_size(); fw0 > 1u && fh0 > 1u) {
        width = fw0;
        height = fh0;
    }
    const auto exts = win->required_instance_extensions();
    if (exts.empty()) {
        fail("GLFW reported no instance extensions - no WSI path on this platform");
        return finish(2);
    }
    auto inst = ve::gfx::instance::create("ve_demo", exts);
    if (!inst) {
        fail(inst.error());
        return finish(2);
    }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (auto made = win->create_surface(inst->handle()); made) {
        surface = *made;
    } else {
        fail(made.error());
        return finish(2);
    }
    const surface_owner surface_guard{&*inst, &surface};

    auto dev = ve::gfx::device::create(*inst, surface);
    if (!dev) {
        fail(dev.error());
        return finish(2);
    }

    // "0 validation problems" is a claim about the layer being IN the chain, not
    // about it being requested. Vulkan 1.4 made vkGetPhysicalDeviceToolProperties
    // core, and it reports the tools/layers actually intercepting this device — so
    // the summary can name the validation layer as active instead of hoping.
    // volk resolves commands to null when the loader cannot provide one, hence the
    // pointer guard rather than a bare call on an older loader.
    if (vkGetPhysicalDeviceToolProperties != nullptr) {
        std::uint32_t tool_count = 0;
        if (vkGetPhysicalDeviceToolProperties(dev->physical(), &tool_count, nullptr) == VK_SUCCESS &&
            tool_count > 0u) {
            std::vector<VkPhysicalDeviceToolProperties> tools(tool_count);
            for (auto& t : tools) {
                t.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TOOL_PROPERTIES;
            }
            if (vkGetPhysicalDeviceToolProperties(dev->physical(), &tool_count, tools.data()) == VK_SUCCESS) {
                tools.resize(tool_count);
                for (const auto& t : tools) {
                    // VkPhysicalDeviceToolProperties names the TOOL, not a layer —
                    // but the Khronos layer registers itself here under its own
                    // layer name, which is exactly what makes it checkable.
                    const std::string layer(t.name);
                    if (layer.empty()) {
                        continue; // a tool that advertises nothing injects nothing to see
                    }
                    tool_line += (tool_line.empty() ? "" : ", ") + layer;
                    if (layer == "VK_LAYER_KHRONOS_validation") {
                        layer_is_active = true;
                    }
                }
            }
        }
    }

    // Evidence, not decoration: the summary names the WSI extensions that actually
    // reached the VkInstance and the device that answered. "0 validation problems"
    // is only meaningful if the instance was built with the surface extensions
    // (otherwise no swapchain path exists at all) and on a named device.
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(dev->physical(), &props);
    const std::uint32_t api = props.apiVersion;
    device_line = std::string(props.deviceName) + "  (api " + std::to_string(VK_API_VERSION_MAJOR(api)) + "." +
                  std::to_string(VK_API_VERSION_MINOR(api)) + "." + std::to_string(VK_API_VERSION_PATCH(api)) +
                  ", present family " + std::to_string(dev->present_queue_family()) + ")";
    for (std::size_t i = 0; i < exts.size(); ++i) {
        ext_line += (i == 0 ? "" : ", ") + exts[i];
    }

    // --- 3. the WSI presenter: the first code that ever RUNS 2.8 -----------
    auto pres = ve::gfx::wsi_presenter::create(*dev, surface, width, height);
    if (!pres) {
        fail(pres.error());
        return finish(2);
    }
    VE_LOG_INFO("demo: presenter target %ux%u %s", pres->width(), pres->height(),
                format_name(pres->color_format()));

    // --- 4. cooked modules -> pipeline (same shape as the 2.6 golden) ------
    auto vert = ve::gfx::shader_module::create(*dev, vert_unit.payload);
    auto frag = ve::gfx::shader_module::create(*dev, frag_unit.payload);
    if (!vert || !frag) {
        fail("shader module load: " + (vert ? frag.error() : vert.error()));
        return finish(2);
    }
    if (vert->stage() != ve::core::cooked::kShdrStageVert ||
        frag->stage() != ve::core::cooked::kShdrStageFrag) {
        fail("cooked stage words do not match demo_tri.vert/.frag");
        return finish(2);
    }
    const VkPushConstantRange pcr{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(PushMvp),
    };
    const VkPipelineLayoutCreateInfo lci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 0,
        .pSetLayouts = nullptr,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev->handle(), &lci, nullptr, &layout) != VK_SUCCESS) {
        fail("vkCreatePipelineLayout failed");
        return finish(2);
    }
    VkPipeline pipe = build_pipeline(*dev, layout, vert->handle(), frag->handle(), pres->color_format());
    if (pipe == VK_NULL_HANDLE) {
        fail("vkCreateGraphicsPipelines failed for the demo pipeline");
        vkDestroyPipelineLayout(dev->handle(), layout, nullptr);
        return finish(2);
    }
    const pipeline_guard pipeline_owner{dev->handle(), &pipe, &layout};

    // --- 5. the frame loop: two frames in flight (2.10), FIFO, spin via push const --
    std::vector<double> frame_ms;
    frame_ms.reserve(frames);
    std::vector<std::uint32_t> image_indices;
    image_indices.reserve(frames);
    std::vector<std::uint8_t> capture_first;
    std::uint32_t presented = 0;
    std::uint32_t recreates = 0;
    std::uint32_t acquire_refusals = 0;
    // Motion probes: a handful of captures spread across the run, fingerprinted.
    // Cap the count so a long run cannot turn a diagnostic into a readback storm.
    std::vector<capture_probe> probes;
    const std::uint32_t probe_step = frames > 16u ? frames / 16u : 1u;
    // The spin proof on the CPU side, deliberately independent of the capture
    // path: if these signatures do not vary, the demo itself is at fault; if they
    // vary while the capture probes do not, the fault is on the GPU side (push
    // constant, pipeline layout, or the capture itself). One run, two verdicts.
    std::set<std::uint32_t> spin_signatures;
    float spin_min = 0.0f;
    float spin_max = 0.0f;
    bool spin_seen = false;
    // The resize contract made visible: every extent the presenter actually ran
    // at, in order. A rebuild that never changed the extent is a rebuild of the
    // same thing, so the trail is what proves "resize -> recreate" rather than
    // just counting recreate() calls.
    std::vector<std::string> extents_seen;
    const std::uint32_t start_w = pres->width();
    const std::uint32_t start_h = pres->height();
    auto prev = std::chrono::steady_clock::now();

    std::uint32_t frame = 0;
    while (frame < frames) {
        win->poll_events();
        if (win->should_close()) {
            VE_LOG_INFO("demo: window closed after %u presented frame(s)", presented);
            break;
        }
        // The 2.8 resize contract (rebuild swapchain + RT, pipeline untouched) has
        // never been exercised live, so the evidence run asks for one explicitly —
        // and then asks BACK, because the first/last captures compared below have to
        // share an extent for their diff to mean anything. A window manager free to
        // ignore the request is exactly why the loop below reacts to frame_size()
        // changing rather than to this call.
        if (resize_at != 0u && frame == resize_at) {
            win->set_size(static_cast<int>(width) - 128, static_cast<int>(height) - 72);
        }
        if (resize_at != 0u && frame == (frames * 11u) / 12u &&
            (width != pres->width() || height != pres->height())) {
            win->set_size(static_cast<int>(width), static_cast<int>(height));
        }
        // Resize, or a surface the WSI called stale: rebuild the chain. The
        // pipeline and the (nonexistent) vertex buffer are untouched —
        // viewport+scissor are dynamic, which is exactly what let 2.8 promise a
        // resize with no pipeline rebuild.
        const auto [fw, fh] = win->frame_size();
        if (pres->needs_recreate() || fw != pres->width() || fh != pres->height()) {
            if (!pres->recreate(fw, fh)) {
                fail("swapchain rebuild refused the new extent");
                return finish(2);
            }
            ++recreates;
            extents_seen.push_back(std::to_string(pres->width()) + "x" +
                                   std::to_string(pres->height()) + "@f" + std::to_string(frame));
        }
        auto idx = pres->acquire();
        if (!idx) {
            ++acquire_refusals;
            if (!pres->needs_recreate() || acquire_refusals > 8u) {
                fail("acquire: " + idx.error());
                return finish(2);
            }
            const auto [rw, rh] = win->frame_size();
            if (!pres->recreate(rw, rh)) {
                fail("recreate after a stale acquire failed");
                return finish(2);
            }
            continue; // same frame number: nothing has been presented yet
        }
        // Background: the default near-black, or --alternate-clear flips it every
        // other frame. That flip is the capture-freshness discriminator — a probe
        // series that ignores it is reading memory that never changed, which pins
        // the fault on the capture rather than on the draw.
        float clear_rgba[4] = {0.02f, 0.02f, 0.06f, 1.0f};
        if (alternate_clear && (frame % 2u) != 0u) {
            clear_rgba[0] = 0.45f;
            clear_rgba[1] = 0.05f;
            clear_rgba[2] = 0.05f;
        }
        const float angle = 0.35f * static_cast<float>(frame);
        PushMvp echo{};
        if (!record_frame(*pres, pipe, layout, pres->width(), pres->height(), angle, clear_rgba,
                          &echo)) {
            fail("recording/present failed at frame " + std::to_string(frame));
            return finish(2);
        }
        // Fingerprint exactly the 64 bytes the vertex stage was handed.
        std::uint32_t sig = 0;
        for (const float v : echo.m) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            sig ^= bits + 0x9e3779b9u + (sig << 6) + (sig >> 2);
        }
        spin_signatures.insert(sig);
        for (const float v : {echo.m[0], echo.m[1], echo.m[4], echo.m[5]}) {
            if (!spin_seen) {
                spin_min = v;
                spin_max = v;
                spin_seen = true;
            }
            spin_min = std::min(spin_min, v);
            spin_max = std::max(spin_max, v);
        }
        image_indices.push_back(*idx);
        const auto now = std::chrono::steady_clock::now();
        frame_ms.push_back(std::chrono::duration<double, std::milli>(now - prev).count());
        prev = now;
        if (presented == 0u) {
            capture_first = pres->capture_rgba8(); // between frames, never mid-pass
        }
        // capture_rgba8() is legal exactly here: the frame is presented, the pass
        // is closed, and the presenter waits its own fence before touching the
        // target. A probe is therefore a real measurement of what each frame drew.
        if (frame % probe_step == 0u && probes.size() < 24u) {
            VE_LOG_INFO("demo: frame %u probe: push m0=%.4f m1=%.4f m4=%.4f (angle %.3f rad)", frame,
                        echo.m[0], echo.m[1], echo.m[4], angle);
            probes.push_back(probe_capture(frame, pres->capture_rgba8()));
        }
        ++presented;
        ++frame;
    }

    // --- 6. evidence: two captures far apart in the spin must DIFFER ---------
    std::vector<std::uint8_t> capture_last;
    if (presented > 0u) {
        capture_last = pres->capture_rgba8(); // between frames: the RT is idle here
    }
    if (capture_first.empty() || capture_last.empty()) {
        fail("capture_rgba8() returned nothing — no evidence to attest to");
        return finish(2);
    }
    const capture_diff spin = compare_captures(capture_first, capture_last);
    if (spin.mean_abs < 0.0) {
        fail("captures are not even the same size");
        return finish(2);
    }

    // Distinct image indices seen: proof the swapchain is actually cycling
    // images rather than presenting into one over and over.
    std::vector<std::uint32_t> distinct = image_indices;
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());

    // B8G8R8A8_* is what capture_rgba8() hands back in swapchain component order,
    // so the PNG writer has to swap R/B for those formats and only those.
    const bool swap_rb = pres->color_format() == VK_FORMAT_B8G8R8A8_UNORM ||
                         pres->color_format() == VK_FORMAT_B8G8R8A8_SRGB;

    bool wrote_pngs = false;
    if (!out.empty()) {
        const auto w = static_cast<std::uint32_t>(pres->width());
        const auto h = static_cast<std::uint32_t>(pres->height());
        const bool a = ve::demo::write_png_rgba8((out / "frame_first.png").string().c_str(), w, h,
                                                 capture_first.data(), swap_rb);
        const bool b = ve::demo::write_png_rgba8((out / "frame_last.png").string().c_str(), w, h,
                                                 capture_last.data(), swap_rb);
        wrote_pngs = a && b;
        if (!wrote_pngs) {
            fail("could not write the evidence PNGs into " + out.string());
        }

        // frames.csv: the raw per-frame record, so the timing claim is checkable.
        std::ofstream csv(out / "frames.csv");
        csv << "frame,present_ms,image_index\n";
        for (std::size_t i = 0; i < frame_ms.size(); ++i) {
            csv << i << ',' << frame_ms[i] << ',' << image_indices[i] << '\n';
        }
    }

    std::vector<double> sorted_ms = frame_ms;
    std::sort(sorted_ms.begin(), sorted_ms.end());
    const double median_ms = sorted_ms.empty() ? 0.0 : sorted_ms[sorted_ms.size() / 2];
    const double min_ms = sorted_ms.empty() ? 0.0 : sorted_ms.front();
    const double max_ms = sorted_ms.empty() ? 0.0 : sorted_ms.back();

    // 2.9's fps claim, computed rather than eyeballed. The first frames carry the
    // cost of the first submits, so the average skips a short warmup; "stable" is
    // reported as the share of frames that actually held 55 fps or better, which a
    // median alone would hide (one 100 ms hitch still leaves a 60 fps median).
    const std::size_t warmup = std::min<std::size_t>(10, frame_ms.size());
    double steady_sum = 0.0;
    std::size_t steady_n = 0;
    std::size_t held_55 = 0;
    for (std::size_t i = 0; i < frame_ms.size(); ++i) {
        if (frame_ms[i] <= 1000.0 / 55.0) {
            ++held_55;
        }
        if (i >= warmup) {
            steady_sum += frame_ms[i];
            ++steady_n;
        }
    }
    const double steady_ms = steady_n != 0 ? steady_sum / static_cast<double>(steady_n) : median_ms;
    const double avg_fps = steady_ms > 0.0 ? 1000.0 / steady_ms : 0.0;
    const double held_pct =
        frame_ms.empty()
            ? 0.0
            : 100.0 * static_cast<double>(held_55) / static_cast<double>(frame_ms.size());

    // --- 7. 2.7's policy, enforced app-side: a clean run validated clean -----
    const auto problems = inst->validation_problems();

    std::string summary;
    summary += "ve_demo - checklist 2.9 WSI evidence\n";
    summary += "device present: " + std::string(win ? "yes" : "no") + '\n';
    summary += "device: " + (device_line.empty() ? std::string("(no device created)") : device_line) + '\n';
    summary += "instance extensions beyond VK_EXT_debug_utils: " +
               (ext_line.empty() ? std::string("(none)") : ext_line) + '\n';
    summary += "extent at close: " + std::to_string(pres->width()) + 'x' + std::to_string(pres->height()) +
               "  format: " + format_name(pres->color_format()) + '\n';
    summary += "frames requested: " + std::to_string(frames) +
               "  presented: " + std::to_string(presented) +
               "  acquire refusals: " + std::to_string(acquire_refusals) +
               "  swapchain rebuilds: " + std::to_string(recreates) + '\n';
    summary += "distinct swapchain images used: " + std::to_string(distinct.size()) + '\n';
    summary += "frame ms min/median/max: " + std::to_string(min_ms) + " / " +
               std::to_string(median_ms) + " / " + std::to_string(max_ms) + '\n';
    summary += "avg fps: " + std::to_string(avg_fps) + " (first " + std::to_string(warmup) +
               " frame(s) excluded as warmup, mean frame time " + std::to_string(steady_ms) +
               " ms); frames at >=55fps: " + std::to_string(held_pct) + "%\n";
    summary += "resize requested at frame: " + std::to_string(resize_at) +
               " (0 = none requested)\n";
    // The extent trail, not just a rebuild count: this is 2.8's resize contract
    // shown as pixels changing under a pipeline that was never rebuilt.
    summary += "extent trail: " + std::to_string(start_w) + 'x' + std::to_string(start_h) +
               " (start)";
    for (const auto& e : extents_seen) {
        summary += " -> " + e;
    }
    summary += '\n';
    summary += "push constants handed to the vertex stage: " +
               std::to_string(spin_signatures.size()) + " distinct signature(s) over " +
               std::to_string(presented) + " presented frame(s); matrix term span " +
               std::to_string(spin_min) + " .. " + std::to_string(spin_max) + "\n";
    if (alternate_clear) {
        summary += "NOTE: --alternate-clear is on (diagnostic). The first-vs-last diff below\n"
                   "      therefore includes the background flip and is NOT a pure spin proof.\n";
    }
    summary += "capture diff first-vs-last: mean_abs=" + std::to_string(spin.mean_abs) +
               " changed=" + std::to_string(spin.changed_pct) + "% over " +
               std::to_string(spin.bytes) + " bytes\n";
    // Distinct fingerprints across the run: the diagnosis behind a zero diff.
    std::vector<std::uint64_t> probe_hashes;
    probe_hashes.reserve(probes.size());
    for (const auto& p : probes) {
        probe_hashes.push_back(p.hash);
    }
    std::sort(probe_hashes.begin(), probe_hashes.end());
    probe_hashes.erase(std::unique(probe_hashes.begin(), probe_hashes.end()), probe_hashes.end());
    summary += "capture probes: " + std::to_string(probes.size()) + " sample(s), " +
               std::to_string(probe_hashes.size()) + " distinct frame image(s)\n";
    for (const auto& p : probes) {
        summary += "  frame " + std::to_string(p.frame) + ": hash=" + std::to_string(p.hash) +
                   " mean=" + std::to_string(p.mean) + " off_background=" +
                   std::to_string(p.off_background) + "/" + std::to_string(p.pixels) + '\n';
    }
    summary += "evidence PNGs written: " + std::string(wrote_pngs ? "yes" : "no") + '\n';
    summary += "active tool layers on this device: " +
               (tool_line.empty() ? std::string("(loader offered no tool list to verify)") : tool_line) + '\n';

    // Author matters for the verdict. The loader's own diagnostics ("Loader Message:"
    // prefixed) are environment reports, not validation findings: measured on this box,
    // the loader emits
    //   windows_read_data_files_in_registry: Registry lookup failed to get layer manifest files
    // while its log shows it located and loaded the very manifest one line later — the
    // message comes from its per-display-device registry scan (HKLM/HKCU
    // ...\Khronos\Vulkan\ExplicitLayers under each DISPLAY device). Counting that as a
    // validation finding would fail a run that validated clean, so the gate is the
    // layer's findings and the loader's are printed as notes. 2.7's messenger policy is
    // untouched: every message is still recorded and shown; only the verdict separates
    // their authors.
    std::vector<std::string> layer_problems;
    std::vector<std::string> loader_notes;
    for (const auto& p : problems) {
        if (p.rfind("Loader Message:", 0) == 0u) {
            loader_notes.push_back(p);
        } else {
            layer_problems.push_back(p);
        }
    }
    summary += "validation problems (Khronos layer): " + std::to_string(layer_problems.size()) + '\n';
    for (const auto& p : layer_problems) {
        summary += "  " + p + '\n';
    }
    if (layer_problems.empty() && layer_is_active) {
        // The claim worth making: the layer was observed IN the chain (tool list), so an
        // empty finding list means the frame loop actually ran validated. instance::create
        // already refuses an unvalidated instance (2.2); this closes the other half.
        summary += "  (VK_LAYER_KHRONOS_validation was reported active on the device, so an\n"
                   "   empty list here means validated — not \"nothing was checking\")\n";
    }
    summary += "loader notes (environment, not gated): " + std::to_string(loader_notes.size()) + '\n';
    for (const auto& p : loader_notes) {
        summary += "  " + p + '\n';
    }

    if (!out.empty()) {
        std::ofstream sf(out / "summary.txt");
        sf << summary;
    }
    std::fputs(summary.c_str(), stdout);
    VE_LOG_INFO("demo: %u frame(s) presented, median %.3f ms, %u validation problem(s)", presented,
                median_ms, static_cast<std::uint32_t>(layer_problems.size()));

    // The gate is the LAYER's findings, never the loader's environment notes (see the
    // author split above): gating on `problems` would fail a run that validated clean
    // purely because of a registry-scan diagnostic.
    if (!layer_problems.empty()) {
        fail("the validation layer found " + std::to_string(layer_problems.size()) +
             " problem(s) — see summary.txt");
        return finish(3);
    }
    if (presented < 2u) {
        fail("fewer than 2 frames presented (" + std::to_string(presented) + ") — nothing to attest");
        return finish(2);
    }
    if (spin.mean_abs == 0.0) {
        // The zero diff has three possible causes and they need different fixes,
        // so the failure names the one the probes actually support.
        if (probe_hashes.size() <= 1u) {
            const bool flat = !probes.empty() && probes.front().off_background == 0u;
            fail(flat
                     ? "every capture is one flat color AND byte-identical: nothing is rendering "
                       "into the presenter's target"
                     : "the render target is identical across " + std::to_string(probes.size()) +
                           " probe(s): the spin is not reaching the image (capture or push constant)");
            return finish(4);
        }
        fail("first and last captures match but " + std::to_string(probe_hashes.size()) +
             " distinct frames were sampled: the spin wrapped, the evidence pair is badly chosen");
        return finish(4);
    }
    if (distinct.empty()) {
        fail("no swapchain image index was ever reported");
        return finish(2);
    }
    // 2.9's "fps stable >= 55" bullet. Judged only once there are enough frames for
    // the number to mean anything: a short run is setup cost, not a framerate. A
    // box whose display refresh is not 60 Hz (or a software ICD with no vsync to
    // wait on) moves this number either way, so the verdict lands with exit 5 — a
    // perf claim, never a correctness failure that could be read as a device bug.
    if (frame_ms.size() >= 30u && avg_fps < 55.0) {
        fail("avg fps is " + std::to_string(avg_fps) + ", under the 55 fps 2.9 asks for");
        return finish(5);
    }
    return finish(0);
}







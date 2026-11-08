#pragma once
#include <ve/gfx/device.h>

#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <vector>

namespace ve::gfx {

// 3.4 pipeline state cache. VkPipeline creation is among the most expensive
// CPU work a frame can contain; the render framework draws the same material
// state over and over, so pipelines are keyed by an engine-side description
// and reused. Deliberately *not* a hash map: the key is a byte-exact
// serialization of the whole desc and lives in an ordered map, so iteration
// (teardown order, debug stats) is deterministic — same discipline as sim
// containers (AGENTS determinism rule).
//
// OWNERSHIP: the cache does not own shader modules or pipeline layouts. The
// VkShaderModule/VkPipelineLayout handles embedded in a desc must outlive
// every pipeline created from it. Destroy/clear the cache before the device.

// Fixed blend state for one color attachment (dynamic-ish per-material color
// rides uniforms/push, never pipeline state — this is pure state).
struct blend_attachment_state {
    bool blend_enable = false;
    VkBlendFactor src_color = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dst_color = VK_BLEND_FACTOR_ZERO;
    VkBlendOp color_op = VK_BLEND_OP_ADD;
    VkBlendFactor src_alpha = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dst_alpha = VK_BLEND_FACTOR_ZERO;
    VkBlendOp alpha_op = VK_BLEND_OP_ADD;
    VkColorComponentFlags write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
};

// One pipeline stage: cooked module + entry point. Module identity is the
// handle; entry strings participate in the key.
struct stage_ref {
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT;
    VkShaderModule module = VK_NULL_HANDLE;
    std::string entry = "main";
};

// Engine-side graphics pipeline description. Only the state this engine
// actually uses is representable (no tessellation, no legacy subpasses) —
// the gaps are features the framework forbids, not TODOs. Dynamic-state
// presence is a bitmask over VkDynamicState values.
struct pipeline_desc {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<stage_ref> stages;

    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;

    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool primitive_restart = false;

    uint32_t viewport_count = 1; // viewport/scissor *contents* dynamic;
    uint32_t scissor_count = 1;  // the count is baked into the layout side

    bool depth_clamp = false;
    bool rasterizer_discard = false;
    VkPolygonMode polygon = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cull = VK_CULL_MODE_BACK_BIT;
    VkFrontFace front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool depth_bias_enable = false;
    float depth_bias_constant = 0.0f;
    float depth_bias_clamp = 0.0f;
    float depth_bias_slope = 0.0f;
    float line_width = 1.0f;

    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool alpha_to_coverage = false;

    bool depth_test = true;
    bool depth_write = true;
    VkCompareOp depth_compare = VK_COMPARE_OP_GREATER; // reversed Z (3.3)
    bool depth_bounds = false;
    float min_depth = 0.0f;
    float max_depth = 1.0f;
    bool stencil = false;
    VkCompareOp stencil_compare = VK_COMPARE_OP_ALWAYS;
    std::uint32_t stencil_read_mask = 0xFFFFFFFFu;
    std::uint32_t stencil_write_mask = 0xFFFFFFFFu;
    VkStencilOp stencil_fail_op = VK_STENCIL_OP_KEEP;
    VkStencilOp stencil_pass_op = VK_STENCIL_OP_KEEP;
    VkStencilOp stencil_depth_fail_op = VK_STENCIL_OP_KEEP;

    std::vector<blend_attachment_state> blends; // one per color attachment

    // Dynamic state: bit (1 << VkDynamicState) set = state is dynamic.
    std::uint32_t dynamic_mask = 0;

    // Dynamic-rendering attachment formats (VK1.3; replaces the renderpass).
    std::uint32_t view_mask = 0; // multiview per-eye view bits (VR rule 2)
    std::vector<VkFormat> color_formats;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    VkFormat stencil_format = VK_FORMAT_UNDEFINED;

    // Byte-exact key. Fixed-size POD fields append raw bytes (same endianness
    // everywhere this engine builds); strings/vectors append length-prefixed.
    [[nodiscard]] std::string key() const;
};

class pipeline_cache {
public:
    explicit pipeline_cache(const device& dev) : device_(&dev) {}
    // Pipelines are created against the device; the cache must be destroyed
    // (explicitly, or naturally as a member declared after the device)
    // before the device goes away. clear() exists for deterministic teardown.
    ~pipeline_cache();

    pipeline_cache(const pipeline_cache&) = delete;
    pipeline_cache& operator=(const pipeline_cache&) = delete;

    // Get or create. Same desc ⇒ same handle (guaranteed). A create failure
    // is an error string, not a throw (core/gfx rule: std::expected).
    std::expected<VkPipeline, std::string> get(const pipeline_desc& desc);

    [[nodiscard]] std::size_t size() const { return pipes_.size(); }
    void clear(); // destroy all cached pipelines (idempotent)

private:
    const device* device_;
    std::map<std::string, VkPipeline> pipes_;
};

} // namespace ve::gfx

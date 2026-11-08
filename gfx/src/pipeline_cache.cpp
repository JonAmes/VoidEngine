#include <ve/gfx/pipeline_cache.h>

#include <cstdint>

namespace ve::gfx {

namespace {
// Key building appends scalars only (never struct memcpy): Vk structs like
// VkVertexInputAttributeDescription carry trailing alignment padding whose
// bytes are uninitialized — raw-appending them would make the key depend on
// stack garbage. Every field goes in explicitly, in a fixed order.
template <typename T>
void append_raw(std::string& k, const T& v) {
    k.append(reinterpret_cast<const char*>(&v), sizeof v);
}
void append_str(std::string& k, const std::string& s) {
    append_raw(k, static_cast<std::uint64_t>(s.size()));
    k.append(s.data(), s.size());
}
} // namespace

std::string pipeline_desc::key() const {
    std::string k;
    append_raw(k, layout);
    append_raw(k, static_cast<std::uint64_t>(stages.size()));
    for (const auto& s : stages) {
        append_raw(k, s.stage);
        append_raw(k, s.module);
        append_str(k, s.entry);
    }
    append_raw(k, static_cast<std::uint64_t>(bindings.size()));
    for (const auto& b : bindings) {
        append_raw(k, b.binding);
        append_raw(k, b.stride);
        append_raw(k, b.inputRate);
    }
    append_raw(k, static_cast<std::uint64_t>(attributes.size()));
    for (const auto& a : attributes) {
        append_raw(k, a.location);
        append_raw(k, a.binding);
        append_raw(k, a.format);
        append_raw(k, a.offset);
    }
    append_raw(k, topology);
    append_raw(k, primitive_restart);
    append_raw(k, viewport_count);
    append_raw(k, scissor_count);
    append_raw(k, depth_clamp);
    append_raw(k, rasterizer_discard);
    append_raw(k, polygon);
    append_raw(k, cull);
    append_raw(k, front);
    append_raw(k, depth_bias_enable);
    append_raw(k, depth_bias_constant);
    append_raw(k, depth_bias_clamp);
    append_raw(k, depth_bias_slope);
    append_raw(k, line_width);
    append_raw(k, samples);
    append_raw(k, alpha_to_coverage);
    append_raw(k, depth_test);
    append_raw(k, depth_write);
    append_raw(k, depth_compare);
    append_raw(k, depth_bounds);
    append_raw(k, min_depth);
    append_raw(k, max_depth);
    append_raw(k, stencil);
    append_raw(k, stencil_compare);
    append_raw(k, stencil_read_mask);
    append_raw(k, stencil_write_mask);
    append_raw(k, stencil_fail_op);
    append_raw(k, stencil_pass_op);
    append_raw(k, stencil_depth_fail_op);
    append_raw(k, static_cast<std::uint64_t>(blends.size()));
    for (const auto& b : blends) {
        append_raw(k, b.blend_enable);
        append_raw(k, b.src_color);
        append_raw(k, b.dst_color);
        append_raw(k, b.color_op);
        append_raw(k, b.src_alpha);
        append_raw(k, b.dst_alpha);
        append_raw(k, b.alpha_op);
        append_raw(k, b.write_mask);
    }
    append_raw(k, dynamic_mask);
    append_raw(k, view_mask);
    append_raw(k, static_cast<std::uint64_t>(color_formats.size()));
    for (const auto f : color_formats) append_raw(k, f);
    append_raw(k, depth_format);
    append_raw(k, stencil_format);
    return k;
}

pipeline_cache::~pipeline_cache() { clear(); }

void pipeline_cache::clear() {
    for (const auto& [k, p] : pipes_) {
        vkDestroyPipeline(device_->handle(), p, nullptr);
    }
    pipes_.clear();
}

std::expected<VkPipeline, std::string> pipeline_cache::get(const pipeline_desc& desc) {
    const std::string k = desc.key();
    if (const auto it = pipes_.find(k); it != pipes_.end()) {
        return it->second; // same desc => same handle (the whole point)
    }

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    stages.reserve(desc.stages.size());
    for (const auto& s : desc.stages) {
        stages.push_back(VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = s.stage,
            .module = s.module,
            .pName = s.entry.c_str(),
            .pSpecializationInfo = nullptr,
        });
    }

    const VkPipelineVertexInputStateCreateInfo vi{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = static_cast<std::uint32_t>(desc.bindings.size()),
        .pVertexBindingDescriptions = desc.bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(desc.attributes.size()),
        .pVertexAttributeDescriptions = desc.attributes.data(),
    };
    const VkPipelineInputAssemblyStateCreateInfo ia{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .topology = desc.topology,
        .primitiveRestartEnable = desc.primitive_restart ? VK_TRUE : VK_FALSE,
    };
    const VkPipelineViewportStateCreateInfo vp{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .viewportCount = desc.viewport_count,
        .pViewports = nullptr, // contents dynamic (framework sets per eye/RT)
        .scissorCount = desc.scissor_count,
        .pScissors = nullptr,
    };
    const VkPipelineRasterizationStateCreateInfo rs{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthClampEnable = desc.depth_clamp ? VK_TRUE : VK_FALSE,
        .rasterizerDiscardEnable = desc.rasterizer_discard ? VK_TRUE : VK_FALSE,
        .polygonMode = desc.polygon,
        .cullMode = desc.cull,
        .frontFace = desc.front,
        .depthBiasEnable = desc.depth_bias_enable ? VK_TRUE : VK_FALSE,
        .depthBiasConstantFactor = desc.depth_bias_constant,
        .depthBiasClamp = desc.depth_bias_clamp,
        .depthBiasSlopeFactor = desc.depth_bias_slope,
        .lineWidth = desc.line_width,
    };
    const VkPipelineMultisampleStateCreateInfo ms{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = desc.samples,
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 1.0f,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = desc.alpha_to_coverage ? VK_TRUE : VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };
    const VkStencilOpState stencil_state{
        .failOp = desc.stencil_fail_op,
        .passOp = desc.stencil_pass_op,
        .depthFailOp = desc.stencil_depth_fail_op,
        .compareOp = desc.stencil_compare,
        .compareMask = desc.stencil_read_mask,
        .writeMask = desc.stencil_write_mask,
        .reference = 0,
    };
    const VkPipelineDepthStencilStateCreateInfo ds{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = desc.depth_compare,
        .depthBoundsTestEnable = desc.depth_bounds ? VK_TRUE : VK_FALSE,
        .stencilTestEnable = desc.stencil ? VK_TRUE : VK_FALSE,
        .front = stencil_state,
        .back = stencil_state, // single-sided stencil: framework never uses two-sided
        .minDepthBounds = desc.min_depth,
        .maxDepthBounds = desc.max_depth,
    };
    std::vector<VkPipelineColorBlendAttachmentState> blends;
    blends.reserve(desc.blends.size());
    for (const auto& b : desc.blends) {
        blends.push_back(VkPipelineColorBlendAttachmentState{
            .blendEnable = b.blend_enable ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = b.src_color,
            .dstColorBlendFactor = b.dst_color,
            .colorBlendOp = b.color_op,
            .srcAlphaBlendFactor = b.src_alpha,
            .dstAlphaBlendFactor = b.dst_alpha,
            .alphaBlendOp = b.alpha_op,
            .colorWriteMask = b.write_mask,
        });
    }
    const VkPipelineColorBlendStateCreateInfo cb{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = static_cast<std::uint32_t>(blends.size()),
        .pAttachments = blends.data(),
        .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
    };
    std::vector<VkDynamicState> dyn;
    for (std::uint32_t bit = 0; bit < 32u; ++bit) {
        if (desc.dynamic_mask & (1u << bit)) {
            dyn.push_back(static_cast<VkDynamicState>(bit));
        }
    }
    const VkPipelineDynamicStateCreateInfo dsy{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = static_cast<std::uint32_t>(dyn.size()),
        .pDynamicStates = dyn.data(),
    };
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .pNext = nullptr,
        .viewMask = desc.view_mask,
        .colorAttachmentCount = static_cast<std::uint32_t>(desc.color_formats.size()),
        .pColorAttachmentFormats = desc.color_formats.data(),
        .depthAttachmentFormat = desc.depth_format,
        .stencilAttachmentFormat = desc.stencil_format,
    };
    const VkGraphicsPipelineCreateInfo pci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .flags = 0,
        .stageCount = static_cast<std::uint32_t>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pTessellationState = nullptr,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = &ds,
        .pColorBlendState = &cb,
        .pDynamicState = &dsy,
        .layout = desc.layout,
        .renderPass = VK_NULL_HANDLE, // dynamic rendering only (VK1.3, #37)
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };

    VkPipeline pipe = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(device_->handle(), VK_NULL_HANDLE, 1, &pci, nullptr, &pipe);
    if (r != VK_SUCCESS) {
        return std::unexpected("vkCreateGraphicsPipelines failed: VkResult=" + std::to_string(r));
    }
    pipes_.emplace(k, pipe);
    return pipe;
}

} // namespace ve::gfx

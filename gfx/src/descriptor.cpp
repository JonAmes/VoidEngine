#include "ve/gfx/descriptor.h"

namespace ve::gfx {

std::expected<sampler_set_layout, std::string> sampler_set_layout::create(
    const device& dev, std::uint32_t binding, VkShaderStageFlags stage) {
    const VkDescriptorSetLayoutBinding b{
        .binding = binding,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = stage,
        .pImmutableSamplers = nullptr, // sampler rides the write, not the layout
    };
    const VkDescriptorSetLayoutCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .bindingCount = 1, .pBindings = &b,
    };
    sampler_set_layout out;
    if (const VkResult r = vkCreateDescriptorSetLayout(dev.handle(), &ci, nullptr, &out.layout_);
        r != VK_SUCCESS) {
        return std::unexpected("sampler_set_layout: vkCreateDescriptorSetLayout failed (" +
                               std::to_string(static_cast<int>(r)) + ")");
    }
    out.dev_ = &dev;
    return out;
}

sampler_set_layout::~sampler_set_layout() {
    if (dev_ && layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev_->handle(), layout_, nullptr);
}

sampler_set_layout::sampler_set_layout(sampler_set_layout&& o) noexcept
    : dev_(o.dev_), layout_(o.layout_) {
    o.dev_ = nullptr;
    o.layout_ = VK_NULL_HANDLE;
}

sampler_set_layout& sampler_set_layout::operator=(sampler_set_layout&& o) noexcept {
    if (this != &o) {
        if (dev_ && layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev_->handle(), layout_, nullptr);
        dev_ = o.dev_;
        layout_ = o.layout_;
        o.dev_ = nullptr;
        o.layout_ = VK_NULL_HANDLE;
    }
    return *this;
}

std::expected<descriptor_pool, std::string> descriptor_pool::create(
    const device& dev, std::uint32_t max_sampler_sets) {
    const VkDescriptorPoolSize size{
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = max_sampler_sets,
    };
    const VkDescriptorPoolCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        // Not FREE_DESCRIPTOR_SET: whole pool reset is the engine idiom —
        // sets are written right after allocate, never individually freed.
        .flags = 0,
        .maxSets = max_sampler_sets,
        .poolSizeCount = 1, .pPoolSizes = &size,
    };
    descriptor_pool out;
    if (const VkResult r = vkCreateDescriptorPool(dev.handle(), &ci, nullptr, &out.pool_);
        r != VK_SUCCESS) {
        return std::unexpected("descriptor_pool: vkCreateDescriptorPool failed (" +
                               std::to_string(static_cast<int>(r)) + ")");
    }
    out.dev_ = &dev;
    return out;
}

descriptor_pool::~descriptor_pool() {
    if (dev_ && pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev_->handle(), pool_, nullptr);
}

descriptor_pool::descriptor_pool(descriptor_pool&& o) noexcept
    : dev_(o.dev_), pool_(o.pool_) {
    o.dev_ = nullptr;
    o.pool_ = VK_NULL_HANDLE;
}

descriptor_pool& descriptor_pool::operator=(descriptor_pool&& o) noexcept {
    if (this != &o) {
        if (dev_ && pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev_->handle(), pool_, nullptr);
        dev_ = o.dev_;
        pool_ = o.pool_;
        o.dev_ = nullptr;
        o.pool_ = VK_NULL_HANDLE;
    }
    return *this;
}

std::expected<VkDescriptorSet, std::string> descriptor_pool::allocate(VkDescriptorSetLayout layout) {
    const VkDescriptorSetAllocateInfo ai{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = pool_,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
    };
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (const VkResult r = vkAllocateDescriptorSets(dev_->handle(), &ai, &set); r != VK_SUCCESS) {
        return std::unexpected("descriptor_pool: vkAllocateDescriptorSets failed (" +
                               std::to_string(static_cast<int>(r)) + ")");
    }
    return set;
}

void descriptor_pool::write_sampler(VkDescriptorSet set, std::uint32_t binding,
                                    VkSampler sampler, VkImageView view) {
    const VkDescriptorImageInfo info{
        .sampler = sampler,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    const VkWriteDescriptorSet w{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = set,
        .dstBinding = binding,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
    vkUpdateDescriptorSets(dev_->handle(), 1, &w, 0, nullptr);
}

} // namespace ve::gfx

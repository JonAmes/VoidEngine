#include "ve/gfx/descriptor.h"

#include <cstring>
#include <span>
#include <optional>
#include <vector>

namespace ve::gfx {

namespace {
// Same file-local helper mesh/presenter/texture each carry — memory-type
// selection stays a per-TU detail until a buffer class unifies it.
std::optional<std::uint32_t> find_memory_type(const device& dev, std::uint32_t type_bits,
                                              VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(dev.physical(), &mp);
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) != 0u &&
            (mp.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return std::nullopt;
}
} // namespace

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

std::expected<uniform_set_layout, std::string> uniform_set_layout::create(
    const device& dev, std::uint32_t binding, VkShaderStageFlags stage) {
    const ubo_binding b{binding, stage};
    return create(dev, std::span<const ubo_binding>{&b, 1});
}

std::expected<uniform_set_layout, std::string> uniform_set_layout::create(
    const device& dev, std::span<const ubo_binding> bindings) {
    std::vector<VkDescriptorSetLayoutBinding> bs;
    bs.reserve(bindings.size());
    for (const auto& b : bindings) {
        bs.push_back(VkDescriptorSetLayoutBinding{
            .binding = b.binding,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = b.stages,
            .pImmutableSamplers = nullptr,
        });
    }
    const VkDescriptorSetLayoutCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .bindingCount = static_cast<std::uint32_t>(bs.size()), .pBindings = bs.data(),
    };
    uniform_set_layout out;
    if (const VkResult r = vkCreateDescriptorSetLayout(dev.handle(), &ci, nullptr, &out.layout_);
        r != VK_SUCCESS) {
        return std::unexpected("uniform_set_layout: vkCreateDescriptorSetLayout failed (" +
                               std::to_string(static_cast<int>(r)) + ")");
    }
    out.dev_ = &dev;
    return out;
}

uniform_set_layout::~uniform_set_layout() {
    if (dev_ && layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev_->handle(), layout_, nullptr);
}

uniform_set_layout::uniform_set_layout(uniform_set_layout&& o) noexcept
    : dev_(o.dev_), layout_(o.layout_) {
    o.dev_ = nullptr;
    o.layout_ = VK_NULL_HANDLE;
}

uniform_set_layout& uniform_set_layout::operator=(uniform_set_layout&& o) noexcept {
    if (this != &o) {
        if (dev_ && layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev_->handle(), layout_, nullptr);
        dev_ = o.dev_;
        layout_ = o.layout_;
        o.dev_ = nullptr;
        o.layout_ = VK_NULL_HANDLE;
    }
    return *this;
}

std::expected<uniform_buffer, std::string> uniform_buffer::create(const device& dev,
                                                                  VkDeviceSize size) {
    const VkBufferCreateInfo bci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr, .flags = 0,
        .size = size,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0, .pQueueFamilyIndices = nullptr,
    };
    uniform_buffer out;
    if (const VkResult r = vkCreateBuffer(dev.handle(), &bci, nullptr, &out.buf_); r != VK_SUCCESS) {
        return std::unexpected("uniform_buffer: vkCreateBuffer failed (" +
                               std::to_string(static_cast<int>(r)) + ")");
    }
    const VkMemoryRequirements reqs = [&] {
        VkMemoryRequirements m{};
        vkGetBufferMemoryRequirements(dev.handle(), out.buf_, &m);
        return m;
    }();
    // Host-visible | host-coherent, persistent mapping: writes are CPU memcpys,
    // device-local staging would buy nothing for one-shot per-frame updates.
    const auto mt = find_memory_type(dev, reqs.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mt) {
        vkDestroyBuffer(dev.handle(), out.buf_, nullptr);
        out.buf_ = VK_NULL_HANDLE;
        return std::unexpected("uniform_buffer: no host-visible|coherent memory type");
    }
    const VkMemoryAllocateInfo mai{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .allocationSize = reqs.size,
        .memoryTypeIndex = *mt,
    };
    if (const VkResult r = vkAllocateMemory(dev.handle(), &mai, nullptr, &out.mem_); r != VK_SUCCESS) {
        vkDestroyBuffer(dev.handle(), out.buf_, nullptr);
        out.buf_ = VK_NULL_HANDLE;
        return std::unexpected("uniform_buffer: vkAllocateMemory failed (" +
                               std::to_string(static_cast<int>(r)) + ")");
    }
    if (const VkResult r = vkBindBufferMemory(dev.handle(), out.buf_, out.mem_, 0); r != VK_SUCCESS) {
        out.destroy();
        return std::unexpected("uniform_buffer: vkBindBufferMemory failed (" +
                               std::to_string(static_cast<int>(r)) + ")");
    }
    if (const VkResult r = vkMapMemory(dev.handle(), out.mem_, 0, VK_WHOLE_SIZE, 0, &out.mapped_);
        r != VK_SUCCESS) {
        out.destroy();
        return std::unexpected("uniform_buffer: vkMapMemory failed (" +
                               std::to_string(static_cast<int>(r)) + ")");
    }
    out.dev_ = &dev;
    out.size_ = size;
    return out;
}

void uniform_buffer::write(const void* data, VkDeviceSize bytes) {
    // Programming error, not runtime input: sized by the caller's struct.
    if (bytes > size_ || mapped_ == nullptr) return;
    std::memcpy(mapped_, data, static_cast<std::size_t>(bytes));
}

void uniform_buffer::destroy() {
    if (!dev_) return;
    if (mapped_ != nullptr) {
        vkUnmapMemory(dev_->handle(), mem_);
        mapped_ = nullptr;
    }
    if (mem_ != VK_NULL_HANDLE) {
        vkFreeMemory(dev_->handle(), mem_, nullptr);
        mem_ = VK_NULL_HANDLE;
    }
    if (buf_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev_->handle(), buf_, nullptr);
        buf_ = VK_NULL_HANDLE;
    }
}

uniform_buffer::~uniform_buffer() { destroy(); }

uniform_buffer::uniform_buffer(uniform_buffer&& o) noexcept
    : dev_(o.dev_), buf_(o.buf_), mem_(o.mem_), mapped_(o.mapped_), size_(o.size_) {
    o.dev_ = nullptr;
    o.buf_ = VK_NULL_HANDLE;
    o.mem_ = VK_NULL_HANDLE;
    o.mapped_ = nullptr;
}

uniform_buffer& uniform_buffer::operator=(uniform_buffer&& o) noexcept {
    if (this != &o) {
        destroy();
        dev_ = o.dev_;
        buf_ = o.buf_;
        mem_ = o.mem_;
        mapped_ = o.mapped_;
        size_ = o.size_;
        o.dev_ = nullptr;
        o.buf_ = VK_NULL_HANDLE;
        o.mem_ = VK_NULL_HANDLE;
        o.mapped_ = nullptr;
    }
    return *this;
}

std::expected<descriptor_pool, std::string> descriptor_pool::create(
    const device& dev, std::uint32_t max_sampler_sets, std::uint32_t max_uniform_sets) {
    // VUID-VkDescriptorPoolSize-descriptorCount-00302: a size entry with
    // count 0 is illegal — only declare the types actually requested.
    std::vector<VkDescriptorPoolSize> sizes;
    if (max_sampler_sets > 0)
        sizes.push_back({.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         .descriptorCount = max_sampler_sets});
    if (max_uniform_sets > 0)
        sizes.push_back({.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                         .descriptorCount = max_uniform_sets});
    const VkDescriptorPoolCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        // Not FREE_DESCRIPTOR_SET: whole pool reset is the engine idiom —
        // sets are written right after allocate, never individually freed.
        .flags = 0,
        .maxSets = max_sampler_sets + max_uniform_sets,
        .poolSizeCount = static_cast<std::uint32_t>(sizes.size()), .pPoolSizes = sizes.data(),
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
                                    VkSampler sampler, VkImageView view,
                                    VkImageLayout layout) {
    const VkDescriptorImageInfo info{
        .sampler = sampler,
        .imageView = view,
        .imageLayout = layout,
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

void descriptor_pool::write_uniform(VkDescriptorSet set, std::uint32_t binding,
                                    VkBuffer buffer, VkDeviceSize size) {
    const VkDescriptorBufferInfo info{
        .buffer = buffer,
        .offset = 0,
        .range = size,
    };
    const VkWriteDescriptorSet w{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = set,
        .dstBinding = binding,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pImageInfo = nullptr,
        .pBufferInfo = &info,
        .pTexelBufferView = nullptr,
    };
    vkUpdateDescriptorSets(dev_->handle(), 1, &w, 0, nullptr);
}

} // namespace ve::gfx

#pragma once
#include <ve/gfx/device.h>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

// 3.7 descriptor plumbing: the minimal glue that gets sampled images into
// dynamic-rendering pipelines. Thin deliberately — the engine's per-draw
// inputs are a small fixed set (sampler bindings now; UBO/storage writes are
// added when 3.9/3.11 need them). No bindless, no pooling heuristics:
// those are optimizations with a phase, not scaffolding for one.
namespace ve::gfx {

// A VkDescriptorSetLayout with exactly one COMBINED_IMAGE_SAMPLER binding —
// the shape every Phase 3 pass uses. Move-only; destroy before the device.
class sampler_set_layout {
public:
    static std::expected<sampler_set_layout, std::string> create(
        const device& dev, std::uint32_t binding, VkShaderStageFlags stage);
    ~sampler_set_layout();
    sampler_set_layout(sampler_set_layout&&) noexcept;
    sampler_set_layout& operator=(sampler_set_layout&&) noexcept;
    sampler_set_layout(const sampler_set_layout&) = delete;
    sampler_set_layout& operator=(const sampler_set_layout&) = delete;

    VkDescriptorSetLayout handle() const { return layout_; }

private:
    sampler_set_layout() = default;
    const device* dev_ = nullptr;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
};

// 3.9: the UBO twin — one UNIFORM_BUFFER binding (per-eye MVP + per-instance
// model matrices ride the eye-pass UBO). std140 alignment is the shader's
// contract; the buffer itself is just bytes here.
class uniform_set_layout {
public:
    static std::expected<uniform_set_layout, std::string> create(
        const device& dev, std::uint32_t binding, VkShaderStageFlags stage);
    ~uniform_set_layout();
    uniform_set_layout(uniform_set_layout&&) noexcept;
    uniform_set_layout& operator=(uniform_set_layout&&) noexcept;
    uniform_set_layout(const uniform_set_layout&) = delete;
    uniform_set_layout& operator=(const uniform_set_layout&) = delete;

    VkDescriptorSetLayout handle() const { return layout_; }

private:
    uniform_set_layout() = default;
    const device* dev_ = nullptr;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
};

// Host-visible, persistent-mapped UNIFORM_BUFFER. One-shot writes before the
// frame that consumes them (submit+wait load pattern, same as gpu_texture
// staging); coherent, so no explicit flush. Move-only.
class uniform_buffer {
public:
    static std::expected<uniform_buffer, std::string> create(const device& dev,
                                                             VkDeviceSize size);
    ~uniform_buffer();
    uniform_buffer(uniform_buffer&&) noexcept;
    uniform_buffer& operator=(uniform_buffer&&) noexcept;
    uniform_buffer(const uniform_buffer&) = delete;
    uniform_buffer& operator=(const uniform_buffer&) = delete;

    VkBuffer handle() const { return buf_; }
    VkDeviceSize size() const { return size_; }

    // memcpy into the mapping; caller must not race this against in-flight
    // reads (frame-sync machinery, 2.10, owns that ordering).
    void write(const void* data, VkDeviceSize bytes);

private:
    uniform_buffer() = default;
    void destroy();

    const device* dev_ = nullptr;
    VkBuffer buf_ = VK_NULL_HANDLE;
    VkDeviceMemory mem_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkDeviceSize size_ = 0;
};

// Descriptor storage for sampler and uniform sets. Sized at creation by set
// counts; freed wholesale (pool destroy) — per-set frees don't exist in
// Vulkan anyway, so there is no fake granularity to expose.
class descriptor_pool {
public:
    static std::expected<descriptor_pool, std::string> create(
        const device& dev, std::uint32_t max_sampler_sets, std::uint32_t max_uniform_sets = 0);
    ~descriptor_pool();
    descriptor_pool(descriptor_pool&&) noexcept;
    descriptor_pool& operator=(descriptor_pool&&) noexcept;
    descriptor_pool(const descriptor_pool&) = delete;
    descriptor_pool& operator=(const descriptor_pool&) = delete;

    VkDescriptorPool handle() const { return pool_; }

    std::expected<VkDescriptorSet, std::string> allocate(VkDescriptorSetLayout layout);

    // The one write shape for now: (texture view, static sampler) pair. The
    // image must already sit in the layout declared here — gpu_texture
    // guarantees SHADER_READ_ONLY_OPTIMAL after create (owner-transition
    // policy), so plumbing never touches barriers.
    void write_sampler(VkDescriptorSet set, std::uint32_t binding,
                       VkSampler sampler, VkImageView view);

    // 3.9: the UBO write — one uniform_buffer bound whole (offset 0).
    void write_uniform(VkDescriptorSet set, std::uint32_t binding,
                       VkBuffer buffer, VkDeviceSize size);

private:
    descriptor_pool() = default;
    const device* dev_ = nullptr;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
};

} // namespace ve::gfx

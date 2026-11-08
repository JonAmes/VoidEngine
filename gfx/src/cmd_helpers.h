#pragma once
#include <volk.h>

#include <cstdint>
#include <optional>

// One-shot command-buffer + memory-type helpers shared by every presenter
// (offscreen 2.4, WSI 2.8). Lives in exactly ONE place on purpose: the 2.7
// double-begin bug (VUID-vkBeginCommandBuffer-commandBuffer-00049) was born
// from copy-pasted command lifecycle code — presenters must not fork it
// again. Internal header: never included outside gfx/src/.
namespace ve::gfx {

// Memory-type lookup keyed on the PHYSICAL device, not ve::gfx::device:
// presenters store the handles (the device object is moved-from elsewhere),
// and recreate() paths have no device object at all.
inline std::optional<std::uint32_t> find_memory_type(VkPhysicalDevice physical,
                                                     std::uint32_t type_bits,
                                                     VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mem_props);
    for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) != 0u &&
            (mem_props.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

inline VkCommandBuffer alloc_cmd(VkDevice device, VkCommandPool pool) {
    const VkCommandBufferAllocateInfo ai{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &ai, &cmd);
    return cmd;
}

inline VkResult begin_one_shot(VkCommandBuffer cmd) {
    const VkCommandBufferBeginInfo bi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    return vkBeginCommandBuffer(cmd, &bi);
}

// Allocate AND begin: for internal one-op uses (clear, capture, present blit)
// whose caller never touches the buffer. Frame helpers that hand the buffer
// to a PASS must use alloc_cmd + begin_one_shot separately — doing both here
// and again in the frame helper begins twice (the 2.7 bug).
inline VkCommandBuffer alloc_one_shot(VkDevice device, VkCommandPool pool) {
    VkCommandBuffer cmd = alloc_cmd(device, pool);
    if (cmd != VK_NULL_HANDLE && begin_one_shot(cmd) != VK_SUCCESS) {
        vkFreeCommandBuffers(device, pool, 1, &cmd);
        return VK_NULL_HANDLE;
    }
    return cmd;
}

inline void submit_and_wait(VkQueue queue, VkCommandBuffer cmd) {
    // End + submit + wait only: whoever allocated the buffer frees it
    // (double-free is a validation failure, proven by the strict messenger).
    // QueueWaitIdle covers this queue's work.
    vkEndCommandBuffer(cmd);
    const VkSubmitInfo si{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
}

} // namespace ve::gfx

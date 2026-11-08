#include <ve/gfx/frame_sync.h>

#include <utility>

namespace ve::gfx {

std::expected<frame_sync, std::string> frame_sync::create(VkDevice device, std::uint32_t frame_count) {
    if (frame_count == 0) {
        return std::unexpected("frame_sync: frame_count must be >= 1");
    }
    frame_sync out;
    out.device_ = device;
    out.fences_.assign(frame_count, VK_NULL_HANDLE);
    // created SIGNALED: an untouched slot is "done", so the first frame of a
    // session never waits on nothing (see header for the why).
    const VkFenceCreateInfo fi{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    for (VkFence& fence : out.fences_) {
        if (const VkResult r = vkCreateFence(device, &fi, nullptr, &fence); r != VK_SUCCESS) {
            for (const VkFence created : out.fences_) {
                if (created != VK_NULL_HANDLE) {
                    vkDestroyFence(device, created, nullptr);
                }
            }
            return std::unexpected("vkCreateFence failed: VkResult(" + std::to_string(static_cast<int>(r)) + ")");
        }
    }
    return out;
}

void frame_sync::destroy_fences() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    for (const VkFence fence : fences_) {
        if (fence != VK_NULL_HANDLE) {
            vkDestroyFence(device_, fence, nullptr);
        }
    }
}

frame_sync::~frame_sync() {
    destroy_fences();
}

frame_sync::frame_sync(frame_sync&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      fences_(std::move(other.fences_)),
      current_(other.current_) {
    other.fences_.clear();
    other.current_ = 0;
}

frame_sync& frame_sync::operator=(frame_sync&& other) noexcept {
    if (this != &other) {
        // NOT this->~frame_sync(): running the destructor and then assigning
        // members moves-into a DEAD vector, whose already-freed buffer it
        // frees a second time (ASan caught exactly this at wsi recreate() ->
        // create_sync). Release the fences we hold and take on `other`'s
        // identity; the moved-from source lands in the empty no-op state.
        destroy_fences();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        fences_ = std::move(other.fences_);
        current_ = std::exchange(other.current_, 0);
    }
    return *this;
}

bool frame_sync::wait_current() {
    if (device_ == VK_NULL_HANDLE) {
        return true; // empty/moved-from: nothing was ever in flight
    }
    return vkWaitForFences(device_, 1, &fences_[current_], VK_TRUE, ~std::uint64_t{0}) == VK_SUCCESS;
}

bool frame_sync::reset_current() {
    if (device_ == VK_NULL_HANDLE) {
        return true;
    }
    return vkResetFences(device_, 1, &fences_[current_]) == VK_SUCCESS;
}

std::uint32_t frame_sync::advance() {
    current_ = (current_ + 1u) % frame_count();
    return current_;
}

bool frame_sync::wait_all() {
    if (device_ == VK_NULL_HANDLE) {
        return true;
    }
    // vkWaitForFences has a waitAll mode — one call, every slot.
    return vkWaitForFences(device_, static_cast<std::uint32_t>(fences_.size()),
                           fences_.data(), VK_TRUE, ~std::uint64_t{0}) == VK_SUCCESS;
}

} // namespace ve::gfx

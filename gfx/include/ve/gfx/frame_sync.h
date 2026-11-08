#pragma once
#include <volk.h>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace ve::gfx {

// Presenter-independent frame sync (2.10): the fence discipline for N frames
// in flight, as its own class so BOTH presenters run the same rules — the
// Tier-2 offscreen path here, the WSI presenter at its acquire/present call
// sites. Swapchain semaphores (image_available_, render_finished_) stay in
// the WSI presenter: they are the swapchain's protocol, not frame sync.
//
// Model (the textbook "one fence per frame slot"):
//   slot = current()          // which frame we are recording into
//   begin: wait_current()     // this slot's PREVIOUS frame must be done
//          reset_current()    // re-arm for the new submit
//   submit: carry fence()     // driver signals it when the frame completes
//   advance()                 // rotate to the next slot
// With frame_count 2, two frames are genuinely in flight: frame k+1 records
// while frame k's fence is still unsignaled.
//
// Fences are created SIGNALED (VkFenceCreateInfo::signaledState) so the first
// use of each slot — no previous frame, nothing submitted — returns from
// wait_current immediately. Waiting an unsignaled fence that no submit owns
// would block forever; creation-signaled is what makes "the slot is empty"
// and "the slot's work is done" the same well-defined state.
class frame_sync {
public:
    static std::expected<frame_sync, std::string> create(VkDevice device, std::uint32_t frame_count);

    // Empty (no fences, device VK_NULL_HANDLE): presenters declare the member
    // and then move-assign a create()d instance into it. Every method on an
    // empty instance is a no-op success — a moved-from presenter tears down
    // without touching anything.
    frame_sync() = default;

    frame_sync(const frame_sync&) = delete;
    frame_sync& operator=(const frame_sync&) = delete;
    frame_sync(frame_sync&& other) noexcept;
    frame_sync& operator=(frame_sync&& other) noexcept;
    ~frame_sync();

    std::uint32_t frame_count() const { return static_cast<std::uint32_t>(fences_.size()); }
    std::uint32_t current() const { return current_; }
    VkFence fence() const { return fences_[current_]; } // carry on this frame's submit

    bool wait_current(); // block until this slot's previous frame completed
    bool reset_current(); // re-arm after wait_current, before the next submit
    std::uint32_t advance(); // rotate; returns the new current slot
    bool wait_all(); // drain every slot (teardown / swapchain recreate)

private:
    // Destroy this instance's fences, leaving it allocatable/movable again.
    // Used by ~frame_sync AND move-assign — never call the destructor manually.
    void destroy_fences();

    VkDevice device_ = VK_NULL_HANDLE;
    std::vector<VkFence> fences_;
    std::uint32_t current_ = 0;
};

} // namespace ve::gfx

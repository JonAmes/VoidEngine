// Phase 2, task 2.7: the validation messenger IS the failure policy.
// A deliberately illegal call must surface in instance::validation_problems()
// with its VUID; ordinary valid traffic must leave the vector empty (asserted
// by every other gfx test). This is what makes "green" mean "validated".
#include <doctest/doctest.h>
#include <ve/gfx/device.h>
#include <ve/gfx/instance.h>

#include "gfx_test_env.h"

#include <string>
#include <vector>

TEST_CASE("gfx_validation_strict") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 validation test skipped");
        return;
    }

    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());
    auto dev = ve::gfx::device::create(*inst);
    REQUIRE(dev.has_value());

    // Fresh instance starts clean.
    CHECK(inst->validation_problems().empty());

    // Find the graphics family (not assumed 0) and record an illegal sequence:
    // vkCmdDraw with no pipeline bound and no rendering active. Validation
    // fires at record time; we never submit the buffer, so the driver is
    // never asked to execute nonsense.
    std::uint32_t fam_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev->physical(), &fam_count, nullptr);
    std::vector<VkQueueFamilyProperties> fams(fam_count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev->physical(), &fam_count, fams.data());
    std::uint32_t family = fam_count; // deliberately invalid if none found
    for (std::uint32_t i = 0; i < fam_count; ++i) {
        if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            family = i;
            break;
        }
    }
    REQUIRE(family < fam_count);

    VkCommandPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    REQUIRE(vkCreateCommandPool(dev->handle(), &pool_ci, nullptr, &pool) == VK_SUCCESS);

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    REQUIRE(vkAllocateCommandBuffers(dev->handle(), &alloc, &cmd) == VK_SUCCESS);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    REQUIRE(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);

    vkCmdDraw(cmd, 3, 1, 0, 0); // no pipeline bound, no render pass: illegal

    vkEndCommandBuffer(cmd); // returns success; the violation is the record

    // The watchdog must have caught it, named with a VUID, and the message
    // must name the offending command.
    const auto problems = inst->validation_problems();
    CHECK_FALSE(problems.empty());
    bool names_draw = false;
    for (const auto& p : problems) {
        if (p.find("vkCmdDraw") != std::string::npos) {
            names_draw = true;
        }
    }
    CHECK(names_draw);

    // Clearing gives a fresh slate (per-phase scoping in later tests).
    inst->clear_validation_problems();
    CHECK(inst->validation_problems().empty());

    vkDestroyCommandPool(dev->handle(), pool, nullptr);
}

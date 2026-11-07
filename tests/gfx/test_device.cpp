#include <doctest/doctest.h>
#include <ve/gfx/device.h>

#include "gfx_test_env.h"

#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

TEST_CASE("gfx_device_init") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 device test skipped");
        return;
    }

    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());

    auto dev = ve::gfx::device::create(*inst);
    REQUIRE(dev.has_value());

    CHECK(dev->handle() != VK_NULL_HANDLE);
    CHECK(dev->physical() != VK_NULL_HANDLE);
    CHECK(dev->graphics_queue() != VK_NULL_HANDLE);

    // Under the forced ICD the chosen device must be lavapipe; the graphics
    // family is whichever family lvp actually exposes (not assumed to be 0).
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(dev->physical(), &props);
    CHECK(std::string(props.deviceName).find("llvmpipe") != std::string::npos);

    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev->physical(), &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev->physical(), &family_count, families.data());
    REQUIRE(dev->graphics_queue_family() < family_count);
    CHECK((families[dev->graphics_queue_family()].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u);
}

TEST_CASE("gfx_device_vegpu_override") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 device test skipped");
        return;
    }

    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());

    // Missing match must be a CLEAN failure naming the needle (never a
    // silent fallback to some other GPU — that would poison goldens).
    setenv("VE_GPU", "definitely-not-a-gpu", 1);
    auto miss = ve::gfx::device::create(*inst);
    REQUIRE_FALSE(miss.has_value());
    CHECK(miss.error().find("definitely-not-a-gpu") != std::string::npos);

    // Matching needle (substring, case-insensitive) must succeed.
    setenv("VE_GPU", "LLVMpipe", 1);
    auto hit = ve::gfx::device::create(*inst);
    REQUIRE(hit.has_value());
    unsetenv("VE_GPU");
}

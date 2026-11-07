#include <doctest/doctest.h>
#include <ve/gfx/instance.h>

#include "gfx_test_env.h"

#include <string>

TEST_CASE("gfx_device_enumerate") {
    if (!gfx_test::force_lvp_icd()) {
        MESSAGE("no lavapipe ICD on this box — Tier-2 device test skipped");
        return;
    }

    auto inst = ve::gfx::instance::create("ve-tests");
    REQUIRE(inst.has_value());

    const auto devices = inst->enumerate_physical_devices();
    REQUIRE_FALSE(devices.empty());

    for (const VkPhysicalDevice phys : devices) {
        VkPhysicalDeviceProperties props{}; // v1 properties struct has no sType
        vkGetPhysicalDeviceProperties(phys, &props);
        CAPTURE(props.deviceName);
        CHECK(props.apiVersion >= VK_API_VERSION_1_3); // decision #37 floor
    }

    // Forced ICD must leave EXACTLY the software device — if a real GPU leaked
    // through the policy, every later golden would be non-deterministic.
    CHECK(devices.size() == 1u);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(devices[0], &props);
    CHECK(std::string(props.deviceName).find("llvmpipe") != std::string::npos);
    CHECK(props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU);
}

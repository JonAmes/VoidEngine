// Tier-1 smoke: the test harness boots headless and every module lib links.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string_view>

#include <ve/core/version.h>
#include <ve/gfx/version.h>
#include <ve/platform/version.h>
#include <ve/game/version.h>

TEST_CASE("engine links and reports its version headless") {
    // Version flows only from the VERSION file through the build system.
    CHECK(std::string_view(ve::core::version_string()) == "00.00.00");
    CHECK(std::string_view(ve::gfx::version_string()) == std::string_view(ve::core::version_string()));
    CHECK(std::string_view(ve::platform::version_string()) == std::string_view(ve::core::version_string()));
    CHECK(std::string_view(ve::game::version_string()) == std::string_view(ve::core::version_string()));
}

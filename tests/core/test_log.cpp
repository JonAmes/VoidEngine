// test: log_levels — level filter + file sink.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <ve/core/log.h>

namespace {
std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("log filter drops below-min levels and file sink captures the rest") {
    auto path = std::filesystem::temp_directory_path() / "ve_test_log_levels.log";
    std::filesystem::remove(path);
    // log_init takes a narrow path; path.c_str() is wchar_t* on Windows (#38),
    // so the narrow form is taken once and lives as long as the calls below.
    const std::string log_path = path.string();

    REQUIRE(ve::core::log_init(ve::core::LogLevel::info, log_path.c_str()));
    CHECK(ve::core::log_level() == ve::core::LogLevel::info);

    VE_LOG_TRACE("trace-line %d", 1);
    VE_LOG_DEBUG("debug-line");
    VE_LOG_INFO("info-line %s", "hello");
    VE_LOG_ERROR("error-line");
    ve::core::log_close();

    auto contents = read_file(path);
    CHECK(contents.find("trace-line") == std::string::npos); // filtered (info min)
    CHECK(contents.find("debug-line") == std::string::npos); // filtered
    CHECK(contents.find("[INFO ] info-line hello") != std::string::npos);
    CHECK(contents.find("[ERROR] error-line") != std::string::npos);

    // trace/debug pass when the floor is lowered
    std::filesystem::remove(path);
    REQUIRE(ve::core::log_init(ve::core::LogLevel::trace, log_path.c_str()));
    VE_LOG_TRACE("low-floor-trace");
    ve::core::log_close();
    CHECK(read_file(path).find("low-floor-trace") != std::string::npos);

    // a file that can't be opened is reported, not swallowed
    CHECK_FALSE(ve::core::log_init(ve::core::LogLevel::info,
                                   "/nonexistent-dir-ve/never.log"));
    std::filesystem::remove(path);
}

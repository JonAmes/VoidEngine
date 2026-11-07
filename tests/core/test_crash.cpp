// test: crash handler — the dump path writes to the log, and a real fatal
// signal in a forked child produces a dumped, faithfully re-raised death.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include <ve/core/crash.h>
#include <ve/core/log.h>

namespace {
std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("crash_dump writes a labelled stack dump to the log sink") {
    auto path = std::filesystem::temp_directory_path() / "ve_test_crash_dump.log";
    std::filesystem::remove(path);
    REQUIRE(ve::core::log_init(ve::core::LogLevel::info, path.c_str()));

    ve::core::crash_dump(SIGSEGV); // in-process: no raise
    ve::core::log_close();

    auto contents = read_file(path);
    CHECK(contents.find("crash: SIGSEGV") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("installed handler dumps and re-raises so the child dies by the signal") {
    auto path = std::filesystem::temp_directory_path() / "ve_test_crash_child.log";
    std::filesystem::remove(path);
    REQUIRE(ve::core::log_init(ve::core::LogLevel::info, path.c_str()));
    ve::core::install_crash_handler(); // our sigactions replace ASan's

    pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        // child inherits log state; produce a genuine fatal signal
        std::raise(SIGILL);
        ::_exit(99); // unreachable if the handler re-raised
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    ve::core::log_close();

    auto contents = read_file(path);
    std::filesystem::remove(path);

    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGILL); // death reported faithfully
    CHECK(contents.find("crash: SIGILL") != std::string::npos);
}

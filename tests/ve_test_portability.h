#pragma once
// The handful of OS-flavored calls the test suites need, in one place.
//
// Why not #ifdefs inline in test bodies: four suites build unique scratch-dir
// names from the process id, one drives the cook CLI through std::system, and
// one pins device selection by environment. Spelling POSIX vs MSVC differences
// at eight call sites invites drift; one header makes the portability surface
// countable (decision #38: the same suites must build on both platforms).
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ve_test {

#if defined(_WIN32)
inline int pid() { return ::_getpid(); }
// MSVC spells these with a leading underscore; _putenv_s with an empty value
// removes the variable, which is what unsetenv means for our purposes.
inline int set_env(const char* name, const char* value) { return ::_putenv_s(name, value); }
inline int clear_env(const char* name) { return ::_putenv_s(name, ""); }

// std::system on Windows returns the child's exit code directly — there is no
// wait-status encoding to decode, and no signal delivery through it either.
inline bool exited_normally(int status) { return status >= 0; }
inline int exit_status(int status) { return status; }
#else
inline int pid() { return static_cast<int>(::getpid()); }
inline int set_env(const char* name, const char* value) { return ::setenv(name, value, 1); }
inline int clear_env(const char* name) { return ::unsetenv(name); }

// POSIX hands back a wait status: distinguish "exited" from "killed by signal"
// before reading an exit code out of it (the 0.11 ASan suite depends on that
// distinction being real, not assumed).
inline bool exited_normally(int status) { return WIFEXITED(status); }
inline int exit_status(int status) { return WEXITSTATUS(status); }
#endif

// Leak-check opt-out for CLI children only; the parent stays fully gated. It is
// an env-assignment prefix, so it only exists where the sanitizer is enabled.
#if defined(_WIN32)
inline const char* asan_off_prefix() { return ""; } // ASan is not built on MSVC yet
#else
inline const char* asan_off_prefix() { return "ASAN_OPTIONS=detect_leaks=0 "; }
#endif

// Wraps a std::system() command line for the shell that will parse it.
//
// Why this exists on Windows: cmd.exe's /c parser strips the outer quote pair of
// a command line that begins with a quote, so a fully-quoted
//   "exe" "src" "out" 2> "log"
// arrives broken — the child never runs, the redirect file is never created, and
// cmd returns 1 by itself. A test that then asserts "nonzero exit" PASSES for the
// wrong reason (measured on MSVC: rc=1 and no log file; with this wrapper rc=0
// and the log file appears). One extra pair around the whole line survives the
// strip. POSIX sh has no such quirk and would take the quotes literally, so the
// helper is a no-op there.
#if defined(_WIN32)
inline std::string shell_cmd(const std::string& command) { return "\"" + command + "\""; }
#else
inline std::string shell_cmd(const std::string& command) { return command; }
#endif

// Is an external command reachable? The cooklet shells out for its GLSL compiler,
// so the shader suites need to tell "this box has no compiler" apart from "the
// cooklet is broken" — the same tier logic the gfx suites use for a missing
// lavapipe ICD (decision #36). Probing is the only honest test: the cooklet's own
// failure text for a missing tool is deliberately similar to its text for a GLSL
// error, so parsing it would let a real bug skip.
//
// The probe is done with std::filesystem, NOT by asking a shell. Why: the obvious
// Windows spelling (`where <tool>`) depends on a utility in System32, and a
// compiler-toolchain environment can leave that off PATH — measured on this box,
// inside a vcvars-imported environment, `where` itself was not found, so
// std::system returned 1 for a compiler that exists, is on PATH, and demonstrably
// works (2.9's demo cooked two units with that very tool). A probe that fails for
// reasons unrelated to the tool is worse than no probe: it silently drops the
// golden's coverage and prints a confident "install glslang-tools".
inline bool tool_on_path(const std::string& tool) {
    if (tool.empty()) {
        return false;
    }
    // A name carrying a separator is a file, not a PATH lookup (VE_GLSLANG may
    // point at an absolute path).
    if (tool.find('/') != std::string::npos || tool.find('\\') != std::string::npos) {
        std::error_code ec;
        return std::filesystem::exists(tool, ec);
    }
#if defined(_WIN32)
    // Own PATH scan. Extensions are probed because cmd resolves a bare name
    // through PATHEXT, and .bat/.cmd are how several SDK wrappers ship.
    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) {
        return false;
    }
    std::istringstream parts(path_env);
    std::string dir;
    while (std::getline(parts, dir, ';')) {
        if (dir.empty()) {
            continue;
        }
        for (const char* ext : {"", ".exe", ".bat", ".cmd"}) {
            std::error_code ec;
            if (std::filesystem::exists(std::filesystem::path(dir) / (tool + ext), ec)) {
                return true;
            }
        }
    }
    return false;
#else
    // POSIX `command -v` is a shell builtin — always present in whatever shell
    // std::system runs, so it carries none of the PATH fragility above.
    return std::system(("command -v " + tool + " >/dev/null 2>&1").c_str()) == 0;
#endif
}

} // namespace ve_test


#include "ve/core/log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace ve::core {
namespace {

struct LogState {
    LogLevel min_level = LogLevel::info;
    std::FILE* file = nullptr;
    std::mutex mutex;
};

LogState& state() {
    static LogState s; // function-local: no global mutable state at load time
    return s;
}

} // namespace

bool log_init(LogLevel min_level, const char* log_file) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.file) {
        std::fclose(s.file);
        s.file = nullptr;
    }
    s.min_level = min_level;
    if (log_file) {
        s.file = std::fopen(log_file, "a");
        if (!s.file) {
            return false;
        }
    }
    return true;
}

void log_close() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.file) {
        std::fclose(s.file);
        s.file = nullptr;
    }
}

LogLevel log_level() { return state().min_level; }

const char* log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::trace: return "TRACE";
    case LogLevel::debug: return "DEBUG";
    case LogLevel::info: return "INFO ";
    case LogLevel::warn: return "WARN ";
    case LogLevel::error: return "ERROR";
    case LogLevel::fatal: return "FATAL";
    case LogLevel::off: return "OFF  ";
    }
    return "?????";
}

void log_msg(LogLevel level, const char* fmt, ...) {
    auto& s = state();
    if (level < s.min_level) {
        return;
    }
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    std::lock_guard lock(s.mutex);
    std::fprintf(stderr, "[%s] %s\n", log_level_name(level), buf);
    if (s.file) {
        std::fprintf(s.file, "[%s] %s\n", log_level_name(level), buf);
        std::fflush(s.file);
    }
}

} // namespace ve::core

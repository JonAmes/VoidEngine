#pragma once

namespace ve::core {

// Structured logging for the whole engine. printf-style (no exceptions in
// core/). NOTE: deliberately NO timestamps here — core/ must not read the
// wall clock (AGENTS.md determinism rule); apps that want timestamps wrap
// their own sink or prefix lines at the presentation layer.
enum class LogLevel : int {
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    error = 4,
    fatal = 5,
    off = 6,
};

// min_level: messages below it are dropped. log_file (nullable): append sink
// in addition to stderr. Returns false if the file cannot be opened.
bool log_init(LogLevel min_level, const char* log_file);
void log_close();

LogLevel log_level();
const char* log_level_name(LogLevel level);

void log_msg(LogLevel level, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

} // namespace ve::core

#define VE_LOG_TRACE(...) ::ve::core::log_msg(::ve::core::LogLevel::trace, __VA_ARGS__)
#define VE_LOG_DEBUG(...) ::ve::core::log_msg(::ve::core::LogLevel::debug, __VA_ARGS__)
#define VE_LOG_INFO(...) ::ve::core::log_msg(::ve::core::LogLevel::info, __VA_ARGS__)
#define VE_LOG_WARN(...) ::ve::core::log_msg(::ve::core::LogLevel::warn, __VA_ARGS__)
#define VE_LOG_ERROR(...) ::ve::core::log_msg(::ve::core::LogLevel::error, __VA_ARGS__)
#define VE_LOG_FATAL(...) ::ve::core::log_msg(::ve::core::LogLevel::fatal, __VA_ARGS__)

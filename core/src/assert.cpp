#include "ve/core/assert.h"
#include "ve/core/log.h"

#include <cstdlib>

namespace ve::core {
namespace {
AssertHandler g_handler = nullptr; // set only via set_assert_handler
}

void set_assert_handler(AssertHandler handler) { g_handler = handler; }
AssertHandler assert_handler() { return g_handler; }

void assert_fail_default(const char* expr, const char* msg,
                         const std::source_location& where) {
    VE_LOG_FATAL("assert failed: %s%s%s at %s:%d in %s", expr,
                 msg && *msg ? " — " : "", msg ? msg : "", where.file_name(),
                 where.line(), where.function_name());
    std::abort();
}

} // namespace ve::core

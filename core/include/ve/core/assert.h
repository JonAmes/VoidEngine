#pragma once

// Engine asserts: release-strippable, exception-free (core/ rule). Active in
// every build that isn't -DNDEBUG (ci_local builds Debug). On failure the
// handler logs, then the default handler aborts (crash handler turns that into
// a stack dump). Tests inject a non-aborting handler via set_assert_handler.
#include <source_location>

namespace ve::core {

using AssertHandler = void (*)(const char* expr, const char* msg,
                               const std::source_location& where);

// nullptr restores the default (log + abort).
void set_assert_handler(AssertHandler handler);
AssertHandler assert_handler();

[[noreturn]] void assert_fail_default(const char* expr, const char* msg,
                                      const std::source_location& where);

inline void assert_fire(const char* expr, const char* msg,
                        const std::source_location& where) {
    if (auto h = assert_handler()) {
        h(expr, msg, where);
    } else {
        assert_fail_default(expr, msg, where);
    }
}

} // namespace ve::core

#if !defined(NDEBUG)
#define VE_ASSERT(cond)                                                        \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ::ve::core::assert_fire(#cond, "",                                   \
                                    std::source_location::current());          \
        }                                                                      \
    } while (0)
#define VE_ASSERT_MSG(cond, msg)                                               \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ::ve::core::assert_fire(#cond, (msg),                              \
                                    std::source_location::current());          \
        }                                                                      \
    } while (0)
#else
#define VE_ASSERT(cond) \
    do {                \
    } while (0)
#define VE_ASSERT_MSG(cond, msg) \
    do {                         \
    } while (0)
#endif

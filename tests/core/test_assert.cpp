// test: assert_macro — VE_ASSERT fires with expr+msg+location, non-firing
// path is clean, and a custom handler replaces the abort default.
#include <doctest/doctest.h>

#include <string>

#include <ve/core/assert.h>

namespace {
struct Capture {
    std::string expr, msg, file;
    unsigned line = 0;
    int fires = 0;
};
Capture* g_capture = nullptr;
void capture_handler(const char* expr, const char* msg,
                     const std::source_location& where) {
    g_capture->expr = expr;
    g_capture->msg = msg ? msg : "";
    g_capture->file = where.file_name();
    g_capture->line = where.line();
    ++g_capture->fires;
}
} // namespace

TEST_CASE("VE_ASSERT reports condition, message, and source location") {
    Capture cap;
    g_capture = &cap;
    ve::core::set_assert_handler(&capture_handler);

    VE_ASSERT(true);
    CHECK(cap.fires == 0);

    int x = 0;
    VE_ASSERT(x > 5);
    CHECK(cap.fires == 1);
    CHECK(cap.expr == "x > 5");
    CHECK(cap.file.find("test_assert.cpp") != std::string::npos);
    CHECK(cap.line > 0);

    VE_ASSERT_MSG(x > 5, "x must exceed five");
    CHECK(cap.fires == 2);
    CHECK(cap.msg == "x must exceed five");

    ve::core::set_assert_handler(nullptr); // default (abort) restored
    g_capture = nullptr;
    CHECK(ve::core::assert_handler() == nullptr);
}

#include <doctest/doctest.h>

#include "ve/core/prof.h"

// Checklist 0.11 automated half: the hook must compile, link, and run under
// ASan. A zone + frame marks exercise the client's lazy init. The capture-side
// check (marks visible in tracy-gui) stays manual by design.
TEST_CASE("tracy_hook_live") {
    bool body_ran = false;
    {
        VE_PROFILE_ZONE("profile_smoke");
        body_ran = true;
    }
    CHECK(body_ran);
    for (int i = 0; i < 3; ++i) {
        VE_PROFILE_FRAME();
    }

#ifdef VE_HAVE_TRACY
    // Contract: the VE_TRACY hook must put TRACY_ENABLE on consuming TUs —
    // without it ZoneScoped/FrameMark above would have compiled to nothing.
#  ifdef TRACY_ENABLE
    CHECK_MESSAGE(true, "VE_TRACY propagates TRACY_ENABLE to consuming TUs");
#  else
    CHECK_MESSAGE(false, "VE_HAVE_TRACY without TRACY_ENABLE: hook broken");
#  endif
    // Touches the client singleton (proves it links). IsConnected is
    // environment-dependent — a dev machine with tracy-gui open flips it true —
    // so its value is deliberately NOT asserted.
    (void)tracy::GetProfiler().IsConnected();
#endif
}

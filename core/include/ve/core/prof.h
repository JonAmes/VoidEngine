#pragma once

// Tracy day-one hook (checklist 0.11; user approved Tracy 2026-11-07).
// VE_TRACY=ON puts VE_HAVE_TRACY + TRACY_ENABLE on every engine TU, so
// instrumentation is one macro away wherever the frame loop lands; VE_TRACY=OFF
// keeps these zero-cost no-ops and links no profiler code.
//
// Integration check that stays MANUAL (checklist 0.11): open tracy-gui (port
// 8086) while any instrumented binary runs — FrameMark rows must appear.
// The automated test (tests/core/test_profile.cpp) only proves the client is
// instrumented, linked, and runs clean under ASan.

#ifdef VE_HAVE_TRACY
#include <tracy/Tracy.hpp>
#define VE_PROFILE_FRAME() FrameMark
#define VE_PROFILE_ZONE(name) ZoneScopedN(name)
#else
#define VE_PROFILE_FRAME() ((void)0)
#define VE_PROFILE_ZONE(name) ((void)0)
#endif

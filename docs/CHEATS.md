# CHEATS.md — shortcuts with reasons

Anything that bends a doctrine rule gets a line here. No line = not allowed.

| What | Where | Why |
|---|---|---|
| stb warning suppressions (`-Wno-*` list) | `tests/CMakeLists.txt` (gfx/test_texture.cpp), `tools/cook/CMakeLists.txt` (src/png.cpp), `tools/demo/CMakeLists.txt` (src/png_out.cpp) | Third-party single-header C code compiled as C++; the `-Wall -Wextra -Wpedantic -Werror` wall governs our code, never vendored headers. Suppressions are scoped to the TUs that instantiate stb — every engine TU still compiles clean under the full wall. |
| Tracy `-w` on its own TUs | `CMakeLists.txt` (Tracy::TracyClient) | Third-party C++ client; the wall governs our code only. Scoped to the imported target; every engine TU compiles under the full wall. Tracy itself is a user-approved dep (PLAN.md dep list), not a cheat. |
| stb linked only into `ve_cooklet` + tests | `tools/CMakeLists.txt` | Decision #13: runtime has zero decode deps. `ve_gfx`/`ve_core` never see stb; `texture_roundtrip` proves the loader is byte-copy only. |
| MSVC wall spelling `/W4 /utf-8 /permissive- /Zc:preprocessor /WX` | `CMakeLists.txt` (compiler-gated) | The wall is re-spelled, never lowered (decision #38). GCC/Clang keep `-Wall -Wextra -Wpedantic -Werror` untouched; MSVC has no `-Wextra`/`-Wpedantic`, so it gets the closest real wall, and `/WX` keeps "warnings are errors" true there too. |
| GCC/Clang-only flags behind `if(NOT MSVC)` (`-Wno-*` stb list, `-fsanitize`, `-rdynamic`) | `tests/CMakeLists.txt`, `tools/cook/CMakeLists.txt`, `core/CMakeLists.txt` | On MSVC an unrecognized option is a *warning* (D9002), and `/WX` makes warnings fatal — so a Linux spelling left in place is a build failure, not a harmless no-op. |
| `doctest` gets `/wd5285` | `tests/CMakeLists.txt` | doctest 2.4.11 specializes `std::tuple`, which VS 18's STL flags (C5285). Third-party header: same rule as stb and Tracy — the wall governs our code, not vendored ones. |
| Sanitizers reported unavailable on MSVC instead of applied | `CMakeLists.txt` | No UBSan on MSVC, and ASan needs a debug build. The "sanitizers on in test builds" contract stays literally true on Linux and is stated out loud elsewhere, rather than silently dropped by an `if`. |
| `core/src/crash.cpp` + `tests/core/test_crash.cpp` excluded on MSVC | `core/CMakeLists.txt`, `tests/CMakeLists.txt` | `sigaction`, `backtrace_symbols_fd` and `execinfo.h` are POSIX by construction, and the handler re-raises so the kernel reports the death. Stubbing it would put a handler in the log that dumps nothing — a lie, not a gap. The Windows handler is a #38 item. |
| `VE_PRINTF_FORMAT(fmt,args)` expands to nothing on MSVC | `core/include/ve/core/log.h` | No `__attribute__` on MSVC; the bare attribute broke every `gfx/` TU on Windows. The printf format check stays exactly where a compiler can perform it. |
| GLFW `/W0` (`-w` on GCC/Clang) | `platform/CMakeLists.txt` | Third-party C TU (fetched when no system package exists); the wall governs engine code only. |


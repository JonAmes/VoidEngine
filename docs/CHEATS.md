# CHEATS.md — shortcuts with reasons

Anything that bends a doctrine rule gets a line here. No line = not allowed.

| What | Where | Why |
|---|---|---|
| stb warning suppressions (`-Wno-*` list) | `tests/CMakeLists.txt` (gfx/test_texture.cpp), `tools/cook/CMakeLists.txt` (src/png.cpp) | Third-party single-header C code compiled as C++; the `-Wall -Wextra -Wpedantic -Werror` wall governs our code, never vendored headers. Suppressions are scoped to the two TUs that instantiate stb — every engine TU still compiles clean under the full wall. |
| stb linked only into `ve_cooklet` + tests | `tools/CMakeLists.txt` | Decision #13: runtime has zero decode deps. `ve_gfx`/`ve_core` never see stb; `texture_roundtrip` proves the loader is byte-copy only. |

# stb — approved dependency, COOKLET-SIDE ONLY (PNG decode). Never linked into
# runtime code (decision #13: zero third-party decode deps at runtime).
# Pinned to an exact commit for reproducible builds; stb has no release tags.
include(FetchContent)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb
    GIT_TAG 8e2e85e5574aacda369c9faa558e9ebde73d8c98
)
FetchContent_MakeAvailable(stb) # header-only repo; no CMake targets upstream
add_library(stb_headers INTERFACE)
target_include_directories(stb_headers INTERFACE ${stb_SOURCE_DIR})

add_subdirectory(cook)

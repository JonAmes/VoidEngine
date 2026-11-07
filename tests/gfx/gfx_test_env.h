#pragma once
#include <cstdlib>
#include <filesystem>

namespace gfx_test {

// Decision #35: Tier-2 gfx tests run on lavapipe — deterministic software
// Vulkan, no display, and it never touches the inference GPUs (3090/P5000).
// MUST be called before volkInitialize/instance creation: the loader reads
// VK_ICD_FILENAMES when it loads ICD manifests, not per call.
// Returns false when no lavapipe ICD exists on this box (test then skips
// with a message — ctest stays green on boxes without mesa-vulkan-drivers).
// #38 note: Windows runs the same suite on a real GPU (no lavapipe there);
// this helper is Linux-only on purpose.
inline bool force_lvp_icd() {
#ifdef __linux__
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/usr/share/vulkan/icd.d", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("lvp_icd", 0) == 0) {
            return setenv("VK_ICD_FILENAMES", entry.path().c_str(), 1) == 0;
        }
    }
#endif
    return false;
}

} // namespace gfx_test

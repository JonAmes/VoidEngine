#pragma once
#include <doctest/doctest.h>
#include <ve/gfx/instance.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace gfx_test {

// The 2.7 failure policy, made visible: when the strict messenger recorded
// anything, the failure prints the actual VUID messages at the call site —
// never a bare "false".
inline void check_clean_validation(const ve::gfx::instance& inst) {
    const auto problems = inst.validation_problems();
    std::string dump;
    for (const auto& p : problems) {
        dump += "\n  " + p;
    }
    CHECK_MESSAGE(problems.empty(), "validation problems:\n" << dump);
}

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

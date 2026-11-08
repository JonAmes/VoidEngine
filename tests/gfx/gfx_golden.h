#pragma once
// 2.11 golden-PNG harness (test-side only — stb is a cooklet/test dep, never
// reachable from ve_gfx). One comparator for every golden image we commit:
//   * strict (policy defaults) on the deterministic lavapipe gate (#35) —
//     byte-exact is the contract there;
//   * fuzzy (channel_tolerance + max_mismatch_pct) on real GPUs (#38), where
//     cross-vendor rasterization is NOT bit-identical and only edges drift —
//     goldens themselves live deep inside/outside primitives (2.6 policy).
// Goldens live in tests/gfx/goldens/; VE_GOLDEN_UPDATE=1 regenerates them
// (and the failure message says so, with the actual image dumped for diff).
#include <string>
#include <vector>

namespace gfx_golden {

struct policy {
    // A pixel MISMATCHES when any of its 4 channels differs from the golden
    // by more than channel_tolerance (0 = byte-exact).
    int channel_tolerance = 0;
    // Fraction (percent) of mismatched pixels still tolerated.
    double max_mismatch_pct = 0.0;
    std::string label = "golden"; // names the image in the failure message
};

struct verdict {
    bool ok = false;
    int golden_w = 0, golden_h = 0;
    int mismatch_pixels = 0;
    double mismatch_pct = 0.0;
    int max_channel_delta = 0;            // reported even when inside tolerance
    int first_bad_x = -1, first_bad_y = -1;
    std::string message;                  // human-readable verdict for CHECK_MESSAGE
};

// Decode a PNG to RGBA8 (stb force-converts any channel count). Empty + err on
// unreadable/missing file.
std::vector<unsigned char> read_png_rgba8(const std::string& path, std::string& err);
// Encode RGBA8 to a PNG. err set on failure.
bool write_png_rgba8(const std::string& path, const unsigned char* rgba, int w, int h,
                     std::string& err);

// The comparator. Handles: missing/unreadable golden (fails, names
// VE_GOLDEN_UPDATE), size mismatch (fails with dimensions), then the
// tolerance/pct gates.
verdict compare_png_rgba8(const unsigned char* rgba, int w, int h,
                          const std::string& golden_png, const policy& pol);

// Repo goldens directory (<repo>/tests/gfx/goldens, overridable with
// VE_GOLDEN_DIR) + an image name inside it.
std::string golden_path(const std::string& name);
// VE_GOLDEN_UPDATE=1 in the environment.
bool golden_update_requested();

// Full workflow used by pixel tests: compare against <name> in the goldens
// dir; on VE_GOLDEN_UPDATE write it instead; on failure dump the actual
// image next to the test's output and put every detail in the message.
verdict check_golden(const std::string& name, const unsigned char* rgba, int w, int h,
                     const policy& pol);

} // namespace gfx_golden

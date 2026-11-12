#pragma once
#include <cstdint>
#include <filesystem>

namespace ve::gfx {
class device;
class wsi_presenter;
} // namespace ve::gfx
namespace ve::platform {
class window;
} // namespace ve::platform

namespace ve::demo {

// 8.6 terrain art-direction spike (#17d, THROWAWAY — no ctest test, per the
// checklist box): renders the 7.2 analytic sampler (terrain_height /
// terrain_normal + authored stamps) through meshes generated CPU-side from
// the sampler, in two looks at the SAME fixed camera and the SAME sampler
// params — stylized low-poly cel with palette cards vs realistic-leaning.
// Writes terrain_stylized.png, terrain_realistic.png and terrain_summary.txt
// into out_dir and self-attests like every ve_demo mode: exit 0 = both
// painted and the looks differ; 2 = setup refusal; 4 = capture/paint/diff
// gate failure. The decision which look wins is the USER's (#17 gate).
int run_terrain_mode(ve::platform::window& win, ve::gfx::device& dev,
                     ve::gfx::wsi_presenter& pres,
                     const std::filesystem::path& units_dir,
                     const std::filesystem::path& out_dir,
                     std::uint32_t frames_per_look);

} // namespace ve::demo

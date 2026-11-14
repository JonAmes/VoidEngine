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

// 8.18 — descent segment for HUMAN EYES (checklist 8.18: "tools/demo ...
// descent segment for human eyes (Xvfb evidence)"). Streams the SAME
// real-scale world the terrain_descent_golden ctest test pins (planet
// r=6 Mm, 8.17 quadtree chunks + morph skirts, 8.10 log-zoom reversed-Z
// spine) down a scripted approach: camera on the +X nadir axis, altitude
// from 20 km to ~1.5 km. Every frame re-selects chunks by the golden
// test's screen-size rule and rebuilds the mesh from the shared sampler
// (sim::gen_chunk) — the streaming path itself, played, not replayed.
//
// Deviation from the checklist wording ("flight mode extended"): the
// 7.14 flight scenario is test-pinned core fixture data, so the descent
// rides as its own --mode descent segment (same tool, same WSI path).
//
// Writes descent_frame<N>_alt<A>m.png per frame + descent_frames.csv +
// descent_summary.txt into out_dir. Self-attests like every ve_demo mode:
// exit 0 = every frame painted (>=0.995 non-sky, the golden test's
// blue-coverage contract at window size) and all PNGs written; 2 = setup
// refusal; 4 = paint/PNG gate failure.
int run_descent_mode(ve::platform::window& win, ve::gfx::device& dev,
                     ve::gfx::wsi_presenter& pres,
                     const std::filesystem::path& units_dir,
                     const std::filesystem::path& out_dir,
                     std::uint32_t frames);

} // namespace ve::demo

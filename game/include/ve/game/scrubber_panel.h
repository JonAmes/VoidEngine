#pragma once
// Phase 6 task 6.16 (decision #16) — the replay scrubber PANEL: the debug-UI
// wrapper around the 6.15 headless scrubber API, built on the Phase 3
// pure-CPU immediate UI core (3.13). Timeline transport (-100/-1/+1/+100,
// jump-to-tick field), an entity-state inspector at the cursor, and a
// state-diff view between ticks. Panel logic is headless-testable: input
// arrives as neutral ui_io, output is the ui_context draw list — no display,
// no presenter, no Vulkan.

#include <cstdint>
#include <optional>

#include "ve/core/sim/scrubber.h"
#include "ve/gfx/ui.h"

namespace ve::game {

// What the panel did this frame (interaction state, observable without
// poking at the scrubber; the scrubber's own cursor/hash are pinned too).
struct scrubber_panel_actions {
    std::int64_t stepped = 0;  // frames step_frame advanced this frame
    bool jumped = false;       // a jump_to landed this frame (button or Enter)
    std::optional<core::sim::tick_t> jumped_to;
    bool diff_toggled = false; // the diff checkbox flipped this frame
};

class scrubber_panel {
public:
    // Fixed panel geometry (tests pin widget rects against these + the
    // ui_context layout metrics).
    static constexpr std::int32_t kWinW = 220, kWinH = 360;
    // Diff view span: diff_between(cursor - kDiffSpan, cursor) while shown.
    static constexpr core::sim::tick_t kDiffSpan = 100;

    // Draw one panel frame at (x, y) and drive the scrubber from clicks.
    void draw(gfx::ui_context& ui, core::sim::ReplayScrubber& sc, std::int32_t x,
              std::int32_t y, scrubber_panel_actions& out);

private:
    // Jump to the tick parsed from the text field; no-op on garbage.
    void commit_tick(core::sim::ReplayScrubber& sc, scrubber_panel_actions& out);

    char m_tick_buf[16] = "";
    bool m_show_diff = false;
};

} // namespace ve::game

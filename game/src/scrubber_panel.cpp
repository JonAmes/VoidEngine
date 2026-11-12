// 6.16 — scrubber panel implementation. Every state mutation goes through
// the 6.15 scrubber API (which itself only ever calls replay_step): the
// panel is pure glue from ui clicks to scrubber calls, plus inspector text
// derived from the cursor state. Layout is the ui_context column, so widget
// rects are a pure function of the widget order below + the fixed metrics.
#include "ve/game/scrubber_panel.h"

#include <cstdio>
#include <cstring>

namespace ve::game {

namespace {

// One inspector row into a fixed buffer (deterministic formatting: same
// state -> same bytes -> same glyph quads).
void row(char (&buf)[96], const char* fmt, double a, double b, double c) {
    std::snprintf(buf, sizeof buf, fmt, a, b, c);
}

} // namespace

void scrubber_panel::commit_tick(core::sim::ReplayScrubber& sc,
                                 scrubber_panel_actions& out) {
    const char* s = m_tick_buf;
    const bool neg = *s == '-';
    if (neg) ++s;
    bool digits = *s != '\0';
    std::int64_t v = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9') {
            digits = false;
            break;
        }
        v = v * 10 + (*p - '0');
    }
    if (!digits)
        return;
    if (sc.jump_to(neg ? static_cast<core::sim::tick_t>(-v)
                       : static_cast<core::sim::tick_t>(v))) {
        out.jumped = true;
        out.jumped_to = sc.cursor();
    }
}

void scrubber_panel::draw(gfx::ui_context& ui, core::sim::ReplayScrubber& sc,
                          std::int32_t x, std::int32_t y,
                          scrubber_panel_actions& out) {
    if (!ui.begin_window("Scrubber", x, y, kWinW, kWinH)) {
        ui.end_window();
        return;
    }

    // --- timeline transport (fixed order; tests pin these rects) ---------
    if (ui.button("-100") && sc.has_cursor() && sc.jump_to(sc.cursor() - 100)) {
        out.jumped = true;
        out.jumped_to = sc.cursor();
    }
    if (ui.button("-1") && sc.has_cursor() && sc.jump_to(sc.cursor() - 1)) {
        out.jumped = true;
        out.jumped_to = sc.cursor();
    }
    if (ui.button("+1")) {
        const std::uint64_t ops0 = sc.replay_ops();
        if (sc.step_frame(1))
            out.stepped = static_cast<std::int64_t>(sc.replay_ops() - ops0);
    }
    if (ui.button("+100")) {
        const std::uint64_t ops0 = sc.replay_ops();
        if (sc.step_frame(100))
            out.stepped = static_cast<std::int64_t>(sc.replay_ops() - ops0);
    }

    // --- jump-to-tick field + Go ------------------------------------------
    if (ui.text_field("Tick", m_tick_buf, sizeof m_tick_buf))
        commit_tick(sc, out);
    if (ui.button("Go"))
        commit_tick(sc, out);

    if (ui.checkbox("Diff t-100", &m_show_diff))
        out.diff_toggled = true;

    // --- cursor + entity inspector ----------------------------------------
    char buf[96];
    if (sc.has_cursor()) {
        std::snprintf(buf, sizeof buf, "t %lld  ops %llu",
                      static_cast<long long>(sc.cursor()),
                      static_cast<unsigned long long>(sc.replay_ops()));
        ui.label(buf);
        std::snprintf(buf, sizeof buf, "cps %zu  log %zu", sc.checkpoint_count(),
                      sc.log_size());
        ui.label(buf);
        const auto h = sc.hash();
        if (h)
            std::snprintf(buf, sizeof buf, "hash %016llx",
                          static_cast<unsigned long long>(*h));
        else
            std::snprintf(buf, sizeof buf, "hash ----");
        ui.label(buf);
        const auto& s = sc.state();
        row(buf, "pos %.9g %.9g %.9g", s.pos.x, s.pos.y, s.pos.z);
        ui.label(buf);
        row(buf, "vel %.9g %.9g %.9g", s.vel.x, s.vel.y, s.vel.z);
        ui.label(buf);
        row(buf, "om  %.9g %.9g %.9g", s.omega_body.x, s.omega_body.y,
            s.omega_body.z);
        ui.label(buf);
        if (m_show_diff) {
            const auto d = sc.diff_between(sc.cursor() - kDiffSpan, sc.cursor());
            if (d) {
                std::snprintf(buf, sizeof buf, "diff %lld->%lld",
                              static_cast<long long>(d->from),
                              static_cast<long long>(d->to));
                ui.label(buf);
                row(buf, "dp %.9g %.9g %.9g", d->dpos.x, d->dpos.y, d->dpos.z);
                ui.label(buf);
                row(buf, "dv %.9g %.9g %.9g", d->dvel.x, d->dvel.y, d->dvel.z);
                ui.label(buf);
                row(buf, "dw %.9g %.9g %.9g", d->domega.x, d->domega.y,
                    d->domega.z);
                ui.label(buf);
            } else {
                ui.label("diff n/a");
            }
        }
    } else {
        ui.label("no cursor");
    }

    ui.end_window();
}

} // namespace ve::game

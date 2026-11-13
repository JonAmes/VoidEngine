#include "ve/core/sim/ftl.h"

#include <cmath>

namespace ve::core::sim {

bool ftl_drive_ok(const ftl_drive& d) {
    return std::isfinite(d.speed_mult) && std::isfinite(d.spool_s) &&
           d.speed_mult > 1.0 && d.spool_s > 0.0;
}

bool ftl_engage(const ftl_drive& d, ftl_state& st, tick_t now) {
    if (st.phase != ftl_phase::idle || !ftl_drive_ok(d))
        return false;  // guard: double-engage and invalid aggregates (mutation-checked)
    st.phase = ftl_phase::spooling_up;
    st.mult = 1.0;
    st.ramp_rate = (d.speed_mult - 1.0) / d.spool_s;  // mult-units per second
    st.anchor_tick = now;
    st.anchor_mult = 1.0;
    return true;
}

bool ftl_disengage(ftl_state& st, tick_t now) {
    if (st.phase == ftl_phase::idle)
        return false;
    st.phase = ftl_phase::spooling_down;
    st.anchor_tick = now;
    st.anchor_mult = st.mult;  // ramp continues from the live value — no jump
    return true;
}

void ftl_step(const ftl_drive& d, ftl_state& st, tick_t now) {
    // Live-aggregate survival check FIRST (every engaged phase): a
    // function-zeroed or collapsed drive goes idle INSTANTLY, so the flip
    // branches below never compare against (or copy) a dead target
    // mid-ramp. dark-not-dead: no ramp invented past the stack.
    if (st.phase != ftl_phase::idle && (!ftl_drive_ok(d) || d.speed_mult <= 1.0)) {
        st.phase = ftl_phase::idle;
        st.mult = 1.0;
        st.anchor_tick = now;
        st.anchor_mult = 1.0;
        return;
    }
    switch (st.phase) {
    case ftl_phase::idle:
        st.mult = 1.0;
        return;
    case ftl_phase::spooling_up: {
        // Analytic from the anchor (header: why not an accumulator).
        const double m =
            st.anchor_mult + st.ramp_rate * tick_to_seconds(now - st.anchor_tick);
        if (m >= d.speed_mult) {
            st.mult = d.speed_mult;  // clamp lands BIT-exactly on the flip tick
            st.phase = ftl_phase::warped;
            st.anchor_tick = now;
        } else {
            st.mult = m;
        }
        return;
    }
    case ftl_phase::warped:
        st.mult = d.speed_mult;  // live-aggregate tracking (survival checked above)
        return;
    case ftl_phase::spooling_down: {
        const double m =
            st.anchor_mult - st.ramp_rate * tick_to_seconds(now - st.anchor_tick);
        if (m <= 1.0) {
            st.mult = 1.0;
            st.phase = ftl_phase::idle;
        } else {
            st.mult = m;
        }
        return;
    }
    }
}

bool ftl_burning(const ftl_state& st) { return st.phase != ftl_phase::idle; }

}  // namespace ve::core::sim

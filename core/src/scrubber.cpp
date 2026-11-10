#include "ve/core/sim/scrubber.h"

#include <utility>

namespace ve::core::sim {

bool ReplayScrubber::load(ReplayLog log, std::vector<sim_checkpoint> cps) {
    if (log.size() == 0 || cps.empty())
        return false;  // nothing to scrub; keep the old record
    const tick_t hi = static_cast<tick_t>(log.size());  // exclusive
    tick_t prev = -2;
    for (const auto& cp : cps) {
        if (cp.tick <= prev || cp.tick >= hi)
            return false;  // not strictly ascending, or outside the record
        prev = cp.tick;
    }
    m_log = std::move(log);
    m_cps = std::move(cps);
    m_have = false;
    m_cursor = -1;
    m_s = rigid_state{};
    m_ops = 0;
    return true;
}

std::size_t ReplayScrubber::covering_cp(tick_t t) const {
    // m_cps ascending; linear scan (checkpoint counts are tiny — ordered
    // container rule #5, and insertion order is the authored order).
    std::size_t best = m_cps.size();
    for (std::size_t i = 0; i < m_cps.size(); ++i) {
        if (m_cps[i].tick <= t)
            best = i;
        else
            break;
    }
    return best;
}

std::optional<rigid_state> ReplayScrubber::materialize(tick_t t) const {
    if (!in_range(t))
        return std::nullopt;
    const std::size_t ci = covering_cp(t);
    if (ci == m_cps.size())
        return std::nullopt;  // no checkpoint covers t
    rigid_state s = m_cps[ci].state;
    for (tick_t i = m_cps[ci].tick + 1; i <= t; ++i) {
        // THE replay step (4.7) — scrubber physics == live physics, by
        // construction there is no other path.
        replay_step(m_w, m_log, static_cast<std::size_t>(i), i, m_p, s);
        ++m_ops;
    }
    return s;
}

bool ReplayScrubber::step_frame(std::int64_t frames) {
    if (!m_have || frames <= 0)
        return false;
    const tick_t target = m_cursor + frames;
    if (target >= static_cast<tick_t>(m_log.size()))
        return false;  // fail closed at the end; cursor unchanged
    for (tick_t i = m_cursor + 1; i <= target; ++i) {
        replay_step(m_w, m_log, static_cast<std::size_t>(i), i, m_p, m_s);
        ++m_ops;
    }
    m_cursor = target;
    return true;
}

bool ReplayScrubber::jump_to(tick_t tick) {
    if (m_have && tick == m_cursor)
        return true;  // already there: zero ops
    const auto s = materialize(tick);
    if (!s)
        return false;
    m_s = *s;
    m_cursor = tick;
    m_have = true;
    return true;
}

std::optional<std::uint64_t> ReplayScrubber::hash() const {
    if (!m_have)
        return std::nullopt;
    return state_hash(m_cursor, m_s);
}

std::optional<sim_state_diff> ReplayScrubber::diff_between(tick_t a,
                                                           tick_t b) const {
    const auto sa = materialize(a);
    const auto sb = materialize(b);
    if (!sa || !sb)
        return std::nullopt;
    sim_state_diff d;
    d.from = a;
    d.to = b;
    d.dpos = sb->pos - sa->pos;
    d.dvel = sb->vel - sa->vel;
    d.domega = sb->omega_body - sa->omega_body;
    // Unit quaternions: to = att_delta * from  =>  delta = to * conj(from).
    d.att_delta = sb->att * math::conjugate(sa->att);
    d.hash_from = state_hash(a, *sa);
    d.hash_to = state_hash(b, *sb);
    return d;
}

}  // namespace ve::core::sim

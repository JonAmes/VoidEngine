#include "ve/core/sim/scrubber.h"

#include <utility>

#include "ve/core/sim/lod.h"

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
    m_compressed = false;
    m_sched.clear();
    m_have = false;
    m_cursor = -1;
    m_s = rigid_state{};
    m_body = lod_body{};
    m_sched_i = 0;
    m_ops = 0;
    return true;
}

bool ReplayScrubber::load_compressed(ReplayLog log, std::vector<sim_checkpoint> cps,
                                     std::vector<lod_transition> sched) {
    // Validate the schedule FIRST (ascending ticks in [0, size), divisors
    // in range) so the all-or-nothing contract matches load(); then defer
    // to load() for the cp/log checks and the member reset.
    const tick_t hi = static_cast<tick_t>(log.size());
    tick_t prev = -1;
    for (const auto& tr : sched) {
        if (tr.tick <= prev || tr.tick >= hi)
            return false;
        if (tr.divisor < 1 || tr.divisor > kLod_max_divisor)
            return false;
        prev = tr.tick;
    }
    if (!load(std::move(log), std::move(cps)))
        return false;
    m_compressed = true;
    m_sched = std::move(sched);
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

// Advance a compressed cursor from (state s, lattice phase valid just
// after tick `from`) through ticks (from, to]. Divisor transitions fire at
// their recorded tick before the due-test at that tick (the live loop's
// ordering), so the walk reproduces it exactly. dt = the elapsed tick gap
// (lod_replay_step derives it), landing every step on the same tick the
// live run stepped.
static void replay_compressed_window(const rail_world& w, const ReplayLog& log,
                                     const std::vector<lod_transition>& sched,
                                     std::size_t& sched_i, const rigid_params& p,
                                     tick_t from, tick_t to, lod_body& b,
                                     rigid_state& s, std::uint64_t& ops) {
    for (tick_t i = from + 1; i <= to; ++i) {
        while (sched_i < sched.size() && sched[sched_i].tick <= i) {
            b.divisor = sched[sched_i].divisor;
            ++sched_i;
        }
        if (lod_replay_step(w, log, i, p, s, b))
            ++ops;
    }
}

std::optional<rigid_state> ReplayScrubber::materialize(tick_t t) const {
    if (!in_range(t))
        return std::nullopt;
    const std::size_t ci = covering_cp(t);
    if (ci == m_cps.size())
        return std::nullopt;  // no checkpoint covers t
    rigid_state s = m_cps[ci].state;
    if (m_compressed) {
        // Rebuild the lattice phase at the covering cp from the recorded
        // schedule, then walk the window — SAME lod_replay_step the live
        // compressed run used, so the scrubbed state is bit-identical.
        const lod_lattice lat = lod_lattice_at(m_sched, m_cps[ci].tick);
        lod_body b{lat.divisor, lat.last_step_tick};
        std::size_t si = 0;
        while (si < m_sched.size() && m_sched[si].tick <= m_cps[ci].tick)
            ++si;  // transitions at/below the cp already folded into lat
        replay_compressed_window(m_w, m_log, m_sched, si, m_p, m_cps[ci].tick, t, b,
                                 s, m_ops);
        return s;
    }
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
    if (m_compressed) {
        replay_compressed_window(m_w, m_log, m_sched, m_sched_i, m_p, m_cursor,
                                 target, m_body, m_s, m_ops);
        m_cursor = target;
        return true;
    }
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
    // Re-seed the incremental lattice cursor for a following step_frame run
    // (jump_to is the entry point; step_frame continues from here).
    if (m_compressed) {
        const lod_lattice lat = lod_lattice_at(m_sched, tick);
        m_body = lod_body{lat.divisor, lat.last_step_tick};
        m_sched_i = 0;
        while (m_sched_i < m_sched.size() && m_sched[m_sched_i].tick <= tick)
            ++m_sched_i;
    }
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

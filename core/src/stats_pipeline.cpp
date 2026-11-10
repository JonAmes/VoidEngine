// Phase 6 task 6.4 — see stats_pipeline.h for the contract.
#include "ve/core/sim/stats_pipeline.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ve::core {

bool StatsPipeline::set_clamp(std::string_view attr, float lo, float hi) {
    if (attr.empty() || !std::isfinite(lo) || !std::isfinite(hi) || lo > hi)
        return false;
    for (auto& c : m_clamps) {
        if (c.first == attr) {
            c.second = clamp_range{lo, hi};
            return true;
        }
    }
    m_clamps.emplace_back(std::string(attr), clamp_range{lo, hi});
    return true;
}

bool StatsPipeline::clear_clamp(std::string_view attr) {
    const std::size_t before = m_clamps.size();
    std::erase_if(m_clamps, [&](const auto& c) { return c.first == attr; });
    return m_clamps.size() != before;
}

const clamp_range* StatsPipeline::find_clamp(std::string_view attr) const {
    for (const auto& c : m_clamps)
        if (c.first == attr)
            return &c.second;
    return nullptr;
}

std::optional<clamp_range> StatsPipeline::clamp_for(std::string_view attr) const {
    if (const auto* r = find_clamp(attr))
        return *r;
    return std::nullopt;
}

std::uint64_t StatsPipeline::current_clock() const {
    // One clock per side (6.1/6.3 revisions); staleness is against the
    // later of the two — any stats-base or stack mutation re-dirties.
    return std::max(m_stats.revision(), m_mods.revision());
}

float StatsPipeline::resolve(std::string_view attr, float base) const {
    float flat = 0.0f, pct = 0.0f, mult = 1.0f;
    // Walk the TOTAL ORDERED VIEW (source order, then modifier order). The
    // walk order is not cosmetic: fp32 add/mul accumulate in this exact
    // sequence, so two runs that see the same stack resolve bit-identically
    // — the replay hash depends on it.
    for (const auto& src : m_mods.sources())
        for (const auto& m : src.mods) {
            if (m.target_attr != attr)
                continue;
            switch (m.op) {
            case mod_op::flat_add:
                flat += m.value;
                break;
            case mod_op::percent:
                pct += m.value;  // percent-POINTS: 10 means +10%
                break;
            case mod_op::multiplier:
                mult *= m.value;
                break;
            }
        }

    float v = base + flat;               // stage 1
    v *= 1.0f + pct / 100.0f;            // stage 2
    v *= mult;                           // stage 3
    if (const auto* r = find_clamp(attr))
        v = std::clamp(v, r->lo, r->hi);  // stage 4 — always last
    // Inputs are finite (6.1/6.3 contracts); only fp32 overflow can leave
    // this range. Saturate rather than let NaN/inf poison clamps, consumers,
    // or the replay hash.
    if (!std::isfinite(v))
        v = std::signbit(v) ? -std::numeric_limits<float>::max()
                            : std::numeric_limits<float>::max();
    return v;
}

const std::uint64_t* StatsPipeline::find_stamp(std::string_view attr) const {
    for (const auto& s : m_stamps)
        if (s.first == attr)
            return &s.second;
    return nullptr;
}

std::uint64_t* StatsPipeline::find_stamp(std::string_view attr) {
    for (auto& s : m_stamps)
        if (s.first == attr)
            return &s.second;
    return nullptr;
}

void StatsPipeline::set_stamp(std::string_view attr, std::uint64_t stamp) {
    if (auto* p = find_stamp(attr))
        *p = stamp;
    else
        m_stamps.emplace_back(std::string(attr), stamp);
}

void StatsPipeline::recompute(std::string_view attr) {
    for (const auto& e : m_stats.entries())
        if (e.key == attr) {
            m_stats.set_derived(attr, resolve(attr, e.value.base));
            set_stamp(attr, current_clock());
            ++m_count;
            return;
        }
}

void StatsPipeline::recompute_all() {
    // Snapshot keys first: set_derived does not change entries, but walking
    // entries() while writing through StatsSet stays obviously safe this way.
    for (const auto& e : m_stats.entries())
        recompute(e.key);
}

std::optional<float> StatsPipeline::derived(std::string_view attr) {
    for (const auto& e : m_stats.entries()) {
        if (e.key != attr)
            continue;
        const std::uint64_t clock = current_clock();
        const std::uint64_t* stamp = find_stamp(attr);
        if (!stamp || *stamp != clock)
            recompute(attr);  // one re-walk; the stamp now equals clock
        return m_stats.derived(attr);
    }
    return std::nullopt;
}

}  // namespace ve::core

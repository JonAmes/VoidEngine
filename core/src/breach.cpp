// 9.12 section breach (#19c) — see breach.h for the law.
#include "ve/core/sim/breach.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <utility>

namespace ve::core::sim {

namespace {
int find_section(const section_air& field, std::string_view id) {
    for (std::size_t i = 0; i < field.sections.size(); ++i)
        if (field.sections[i]->id == id)
            return static_cast<int>(i);
    return -1;
}
}  // namespace

bool air_configure(section_air& field,
                   std::span<const section_air_setup> setups,
                   std::span<const air_edge> edges,
                   StatsPipeline& pipe) {
    section_air fresh;  // build off-tree: a bad authoring leaves `field` empty
    for (const auto& s : setups) {
        if (s.section_id.empty() || !std::isfinite(s.capacity) || s.capacity <= 0.0)
            return false;
        if (std::any_of(fresh.sections.begin(), fresh.sections.end(),
                        [&](const auto& e) { return e->id == s.section_id; }))
            return false;  // duplicate section id
        auto entry = std::make_unique<air_entry>(s.section_id, s.capacity, s.bands);
        entry->air.set_ceiling(s.capacity);  // ctor sets VALUE only (9.11 trap)
        // Fail-open flows (6.9 law): empty refill attr = no pump.
        entry->air.bind(&pipe, reservoir_attrs{.supply_attr = s.refill_attr,
                                               .load_attr = "",
                                               .leak_attr = ""});
        fresh.sections.push_back(std::move(entry));
    }
    for (const auto& e : edges) {
        if (e.a >= fresh.sections.size() || e.b >= fresh.sections.size() || e.a == e.b)
            return false;  // edge outside the graph
        fresh.edges.push_back(e);
    }
    field = std::move(fresh);
    return true;
}

bool section_air::breach_open(std::string_view section_id, double rate) {
    const int idx = find_section(*this, section_id);
    if (idx < 0) return false;                              // unknown section
    if (!std::isfinite(rate) || rate <= 0.0) return false;  // fail-SAFE
    if (is_breached(section_id)) return false;              // idempotent
    breaches.push_back(breach_entry{static_cast<std::uint32_t>(idx), rate});
    return true;
}

bool section_air::breach_patch(std::string_view section_id) {
    const int idx = find_section(*this, section_id);
    if (idx < 0) return false;
    const auto before = breaches.size();
    std::erase_if(breaches, [&](const breach_entry& b) {
        return b.section == static_cast<std::uint32_t>(idx);
    });
    return breaches.size() != before;  // no breach there = false
}

bool section_air::is_breached(std::string_view section_id) const {
    const int idx = find_section(*this, section_id);
    if (idx < 0) return false;
    return std::any_of(breaches.begin(), breaches.end(),
                       [&](const breach_entry& b) {
                           return b.section == static_cast<std::uint32_t>(idx);
                       });
}

Reservoir* section_air::air(std::string_view section_id) {
    const int idx = find_section(*this, section_id);
    return idx < 0 ? nullptr : &sections[idx]->air;
}

TagSet* section_air::section_tags(std::string_view section_id) {
    const int idx = find_section(*this, section_id);
    return idx < 0 ? nullptr : &sections[idx]->tags;
}

void section_air::step(const DoorField& doors) {
    // 1) shipped #19e flows (refill pumps) run first, every tick.
    for (auto& s : sections) s->air.step();
    if (breaches.empty()) return;

    // 2) per breach: BFS reachable region through OPEN (or doorless)
    // edges. Determinism: sections expand in edge-registration order;
    // the queue is FIFO; visited is an index-ordered bitset.
    std::vector<char> visited(sections.size(), 0);
    for (const auto& b : breaches) {
        std::fill(visited.begin(), visited.end(), 0);
        std::deque<std::uint32_t> queue{b.section};
        visited[b.section] = 1;
        while (!queue.empty()) {
            const std::uint32_t cur = queue.front();
            queue.pop_front();
            for (const auto& e : edges) {
                const std::uint32_t other =
                    e.a == cur ? e.b : (e.b == cur ? e.a : 0xFFFFFFFFu);
                if (other == 0xFFFFFFFFu || visited[other]) continue;
                if (e.door_mount && !doors.open(*e.door_mount)) continue;  // shut = airtight
                visited[other] = 1;
                queue.push_back(other);
            }
        }
        // 3) every section in the region loses this breach's rate through
        // the set_value door (floor clamps at exactly 0).
        for (std::size_t i = 0; i < sections.size(); ++i)
            if (visited[i])
                sections[i]->air.set_value(sections[i]->air.value() - b.rate);
    }
}

bool section_air::integrity_hit(double damage) {
    if (!integrity) return false;                              // tier unconfigured
    if (!std::isfinite(damage) || damage <= 0.0) return false;  // fail-SAFE
    if (destroyed) return false;  // terminal event: zero state change after
    // Overflow landing on the structure drains through the set_value door
    // (floor clamps at exactly 0.0; ceiling never rises).
    integrity->integrity.set_value(integrity->integrity.value() - damage);
    // Zero-crossing = the dramatic hull breach: terminal sim event. The
    // authored band throws INSIDE set_value's re-eval; the flag marks the
    // event exactly once (first landing on 0.0).
    if (integrity->integrity.value() == 0.0) destroyed = true;
    return true;
}

Reservoir* section_air::integrity_value() {
    return integrity ? &integrity->integrity : nullptr;
}

TagSet* section_air::hull_tags() {
    return integrity ? &integrity->tags : nullptr;
}

bool integrity_configure(section_air& field, const hull_integrity_setup& setup) {
    if (!std::isfinite(setup.capacity) || setup.capacity <= 0.0) return false;
    if (field.integrity) return false;  // reconfigure while configured = refused
    // unique_ptr keeps the TagSet address fixed for the Reservoir's self-ref.
    field.integrity = std::make_unique<section_air::integrity_entry>(
        setup.capacity, setup.bands);
    field.integrity->integrity.set_ceiling(setup.capacity);  // ctor sets VALUE only
    return true;
}

}  // namespace ve::core::sim

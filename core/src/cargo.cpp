#include "ve/core/sim/cargo.h"

#include <algorithm>
#include <cmath>

namespace ve::core::sim {

double stack_desc::total() const {
    double t = 0.0;
    for (const auto& p : placements) t += p.quantity;
    return t;
}

bool hold_desc::accepts(std::string_view commodity) const {
    // No gates = open bulk hold (#20a capacity gate default: ungated).
    if (gates.empty()) return true;
    return std::find(gates.begin(), gates.end(), commodity) != gates.end();
}

CargoManifest::CargoManifest(const Loadout& loadout, std::string_view hold_tag,
                             std::string_view capacity_key,
                             std::string_view gate_prefix) {
    // Hold order = mount order = deterministic sim iteration (#5).
    for (const auto& mc : loadout.mounts()) {
        if (std::find(mc.tags.begin(), mc.tags.end(), hold_tag) == mc.tags.end())
            continue;
        hold_desc h;
        h.mount_id = mc.instance_id;
        h.capacity = 0.0;  // attr absent -> inert hold
        for (const auto& a : mc.attrs)
            if (a.key == capacity_key) h.capacity = a.value;
        if (!std::isfinite(h.capacity) || h.capacity < 0.0)
            h.capacity = 0.0;  // defensive; cooklet gates numeric attrs
        for (const auto& t : mc.tags)
            if (t.size() > gate_prefix.size() &&
                t.compare(0, gate_prefix.size(), gate_prefix) == 0)
                h.gates.push_back(t.substr(gate_prefix.size()));
        m_holds.push_back(std::move(h));
    }
}

stack_desc* CargoManifest::find_stack(std::string_view commodity) {
    const auto it = std::find_if(m_stacks.begin(), m_stacks.end(),
                                 [&](const stack_desc& s) { return s.commodity_id == commodity; });
    return it == m_stacks.end() ? nullptr : &*it;
}

const stack_desc* CargoManifest::find_stack(std::string_view commodity) const {
    return const_cast<CargoManifest*>(this)->find_stack(commodity);
}

std::expected<void, cargo_error> CargoManifest::add_commodity(
    std::string_view commodity, double qty) {
    if (!std::isfinite(qty) || qty <= 0.0)
        return std::unexpected(cargo_error::invalid_quantity);
    if (commodity.empty())
        return std::unexpected(cargo_error::invalid_id);

    // Which holds can take it, and their free space, in mount order.
    std::vector<double> free_space;  // parallel to m_holds; -1 = cannot take
    std::size_t accepting = 0;
    double room = 0.0;
    for (std::size_t i = 0; i < m_holds.size(); ++i) {
        const auto& h = m_holds[i];
        if (!h.accepts(commodity)) { free_space.push_back(-1.0); continue; }
        ++accepting;
        double used = 0.0;
        for (const auto& st : m_stacks)
            for (const auto& p : st.placements)
                if (p.hold_index == i) used += p.quantity;  // i == free_space.size()
        const double f = h.capacity - used;
        free_space.push_back(f > 0.0 ? f : 0.0);
        room += free_space.back();
    }
    if (accepting == 0)
        return std::unexpected(cargo_error::no_gate);  // no hold gates this id
    if (room + 1e-9 < qty)
        return std::unexpected(cargo_error::overflow);  // atomic: store nothing

    stack_desc* st = find_stack(commodity);
    if (!st) {
        m_stacks.push_back(stack_desc{std::string(commodity), {}});
        st = &m_stacks.back();
    }
    // First-fit split in hold (mount) order: exact, replay-identical.
    double left = qty;
    for (std::size_t i = 0; i < m_holds.size() && left > 0.0; ++i) {
        if (free_space[i] <= 0.0) continue;
        const double take = std::min(free_space[i], left);
        auto pit = std::find_if(st->placements.begin(), st->placements.end(),
                                [&](const placement_desc& p) { return p.hold_index == i; });
        if (pit == st->placements.end())
            st->placements.push_back(placement_desc{i, take});
        else
            pit->quantity += take;
        left -= take;
    }
    return {};
}

std::expected<void, cargo_error> CargoManifest::remove_commodity(
    std::string_view commodity, double qty) {
    if (!std::isfinite(qty) || qty <= 0.0)
        return std::unexpected(cargo_error::invalid_quantity);
    if (commodity.empty())
        return std::unexpected(cargo_error::invalid_id);
    stack_desc* st = find_stack(commodity);
    if (!st) return std::unexpected(cargo_error::unknown_commodity);
    if (st->total() + 1e-9 < qty)
        return std::unexpected(cargo_error::insufficient);

    // Drain placements in REVERSE hold order: removal is the mirror image of
    // the first-fit fill, so add/remove pairs cancel exactly at any tick.
    double left = qty;
    for (auto pit = st->placements.end(); pit != st->placements.begin() && left > 0.0;) {
        --pit;
        const double take = std::min(pit->quantity, left);
        pit->quantity -= take;
        left -= take;
    }
    std::erase_if(st->placements, [](const placement_desc& p) { return p.quantity <= 0.0; });
    if (st->placements.empty())
        m_stacks.erase(m_stacks.begin() + (st - m_stacks.data()));  // drained stacks leave
    return {};
}

std::expected<void, cargo_error> CargoManifest::add_spare(
    std::string_view instance_id, std::string_view type_id) {
    if (instance_id.empty() || type_id.empty())
        return std::unexpected(cargo_error::invalid_id);
    if (std::any_of(m_spares.begin(), m_spares.end(),
                    [&](const spare_desc& s) { return s.instance_id == instance_id; }))
        return std::unexpected(cargo_error::duplicate_spare);
    m_spares.push_back(spare_desc{std::string(instance_id), std::string(type_id)});
    return {};
}

std::expected<void, cargo_error> CargoManifest::remove_spare(std::string_view instance_id) {
    const auto it = std::find_if(m_spares.begin(), m_spares.end(),
                                 [&](const spare_desc& s) { return s.instance_id == instance_id; });
    if (it == m_spares.end()) return std::unexpected(cargo_error::unknown_spare);
    m_spares.erase(it);
    return {};
}

double CargoManifest::quantity_of(std::string_view commodity) const {
    const stack_desc* st = find_stack(commodity);
    return st ? st->total() : 0.0;
}

}  // namespace ve::core::sim

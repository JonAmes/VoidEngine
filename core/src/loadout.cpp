// 5.1 — loadout assembly + total validation. Check order is FIXED so the
// reported error is deterministic for a broken input (replay/authoring tools
// may branch on it).
#include "ve/core/sim/loadout.h"

namespace ve::core::sim {

std::expected<Loadout, loadout_error> Loadout::assemble(
    hull_info hull,
    std::vector<socket_desc> sockets,
    std::vector<component_type> catalog,
    std::vector<mount_request> mounts) {
    Loadout out;

    // Hull-level sanity before anything references it.
    if (!(hull.hull_mass >= 0.0))
        return std::unexpected(loadout_error::invalid_hull_mass);
    if (!(hull.grid.cell > 0.0) || hull.grid.subcell_ratio < 1 ||
        !(hull.grid.cell_height > 0.0) || !(hull.grid.tolerance >= 0.0))
        return std::unexpected(loadout_error::invalid_grid);

    // Socket table: unique ids.
    for (std::size_t i = 0; i < sockets.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j)
            if (sockets[j].id == sockets[i].id)
                return std::unexpected(loadout_error::duplicate_socket_id);
    }
    // Publish the socket table up front: the mount loop queries it.
    out.m_hull = std::move(hull);
    out.m_sockets = std::move(sockets);

    for (const auto& mr : mounts) {
        // Type must exist in the catalog (its category drives the match).
        const component_type* type = nullptr;
        for (const auto& ct : catalog)
            if (ct.type_id == mr.type_id) { type = &ct; break; }
        if (!type)
            return std::unexpected(loadout_error::unknown_component_type);

        // Socket must exist, be free, and accept the category.
        const int si = out.socket_index(mr.socket_id);
        if (si < 0)
            return std::unexpected(loadout_error::unknown_socket);
        if (out.mount_at_socket(mr.socket_id) != nullptr)
            return std::unexpected(loadout_error::socket_occupied);
        if (type->category != out.m_sockets[si].category)
            return std::unexpected(loadout_error::category_mismatch);

        // Unique instance id; unique attr keys within this mount.
        if (out.mount(mr.instance_id) != nullptr)
            return std::unexpected(loadout_error::duplicate_instance_id);
        for (std::size_t i = 0; i < mr.attrs.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (mr.attrs[j].key == mr.attrs[i].key)
                    return std::unexpected(loadout_error::duplicate_attr_key);

        mounted_component mc;
        mc.instance_id = mr.instance_id;
        mc.type_id = mr.type_id;
        mc.socket_id = mr.socket_id;
        mc.category = out.m_sockets[si].category;  // == type category (checked)
        mc.attrs = mr.attrs;                       // insertion order kept
        out.m_mounts.push_back(std::move(mc));
    }

    return out;
}

int Loadout::socket_index(std::string_view id) const {
    for (std::size_t i = 0; i < m_sockets.size(); ++i)
        if (m_sockets[i].id == id)
            return static_cast<int>(i);
    return -1;
}

const mounted_component* Loadout::mount(std::string_view instance_id) const {
    for (const auto& mc : m_mounts)
        if (mc.instance_id == instance_id)
            return &mc;
    return nullptr;
}

const mounted_component* Loadout::mount_at_socket(std::string_view socket_id) const {
    for (const auto& mc : m_mounts)
        if (mc.socket_id == socket_id)
            return &mc;
    return nullptr;
}

std::optional<double> Loadout::attr(const mounted_component& mc, std::string_view key) const {
    for (const auto& ap : mc.attrs)
        if (ap.key == key)
            return ap.value;
    return std::nullopt;
}

}  // namespace ve::core::sim

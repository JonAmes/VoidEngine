// Phase 5 task 5.11 — see ve/core/sim/occupancy.h.

#include "ve/core/sim/occupancy.h"

#include <algorithm>
#include <utility>

namespace ve::core::sim {

Occupancy::Occupancy(const Loadout& loadout,
                     std::span<const std::string> role_tag_registry) {
    // Mount order -> seat order (deterministic, #5). A mount is a seat iff
    // any of its tags is in the registry; role_tags keeps MOUNT tag order
    // so a multi-role seat iterates roles as the def authored them.
    for (const auto& mc : loadout.mounts()) {
        std::vector<std::string> roles;
        for (const auto& t : mc.tags)
            if (std::find(role_tag_registry.begin(), role_tag_registry.end(), t) !=
                role_tag_registry.end())
                roles.push_back(t);
        if (roles.empty()) continue;
        m_seats.push_back(seat_desc{mc.instance_id, std::move(roles), false, std::nullopt});
    }
}

int Occupancy::seat_index(std::string_view id) const {
    for (std::size_t i = 0; i < m_seats.size(); ++i)
        if (m_seats[i].instance_id == id) return static_cast<int>(i);
    return -1;
}

std::expected<void, seat_error> Occupancy::sit(std::uint64_t occupant,
                                               std::string_view seat) {
    const int dst = seat_index(seat);
    if (dst < 0) return std::unexpected(seat_error::unknown_seat);
    if (m_seats[dst].occupant && *m_seats[dst].occupant != occupant)
        return std::unexpected(seat_error::seat_busy);
    // Atomic move: detach from any other seat first, then attach. Same
    // occupant already in `seat` is a no-op that still succeeds.
    for (auto& s : m_seats)
        if (s.occupant && *s.occupant == occupant) s.occupant = std::nullopt;
    m_seats[dst].occupant = occupant;
    return {};
}

std::expected<void, seat_error> Occupancy::stand(std::string_view seat) {
    const int i = seat_index(seat);
    if (i < 0) return std::unexpected(seat_error::unknown_seat);
    if (!m_seats[i].occupant) return std::unexpected(seat_error::not_seated);
    m_seats[i].occupant = std::nullopt;
    return {};
}

std::expected<void, seat_error> Occupancy::set_dark(std::string_view seat, bool dark) {
    const int i = seat_index(seat);
    if (i < 0) return std::unexpected(seat_error::unknown_seat);
    m_seats[i].dark = dark;  // occupancy untouched: dark drops availability
                             // (the Phase 6 stack), the sitter stays put
    return {};
}

std::optional<std::uint64_t> Occupancy::occupant_at(std::string_view seat) const {
    const int i = seat_index(seat);
    if (i < 0 || !m_seats[i].occupant) return std::nullopt;
    return m_seats[i].occupant;
}

std::optional<std::string_view> Occupancy::seat_of(std::uint64_t occupant) const {
    for (const auto& s : m_seats)
        if (s.occupant && *s.occupant == occupant) return s.instance_id;
    return std::nullopt;
}

bool Occupancy::function_available(std::string_view seat) const {
    const int i = seat_index(seat);
    return i >= 0 && m_seats[i].occupant && !m_seats[i].dark;
}

std::optional<std::uint64_t> Occupancy::occupant_for_role(std::string_view role) const {
    for (const auto& s : m_seats) {
        if (!s.occupant || s.dark) continue;
        if (std::find(s.role_tags.begin(), s.role_tags.end(), role) != s.role_tags.end())
            return s.occupant;
    }
    return std::nullopt;
}

}  // namespace ve::core::sim

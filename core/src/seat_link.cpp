// Phase 6 task 6.7 — see seat_link.h for the contract.
#include "ve/core/sim/seat_link.h"

namespace ve::core::sim {

bool SeatLink::add_affordance(const seat_affordance& a) {
    if (a.role_tag.empty())
        return false;
    for (const auto& e : m_affordances)
        if (e.role_tag == a.role_tag)
            return false;  // one affordance per role; duplicates are a data bug
    m_affordances.push_back(a);
    return true;
}

std::optional<std::size_t> SeatLink::find_seat(std::string_view seat) const {
    const auto& seats = m_occ.seats();
    for (std::size_t i = 0; i < seats.size(); ++i)
        if (seats[i].instance_id == seat)
            return i;
    return std::nullopt;
}

std::optional<std::size_t> SeatLink::seated_at(std::uint64_t occupant) const {
    const auto sv = m_occ.seat_of(occupant);
    if (!sv)
        return std::nullopt;
    return find_seat(*sv);
}

const seat_affordance* SeatLink::affordance_for(std::size_t seat_i) const {
    // First registered affordance whose role tag the seat carries wins
    // (registration order is the deterministic tie-break).
    for (const auto& a : m_affordances)
        for (const auto& rt : m_occ.seats()[seat_i].role_tags)
            if (rt == a.role_tag)
                return &a;
    return nullptr;
}

void SeatLink::apply_seat(std::size_t i, std::uint64_t /*occupant*/,
                          ModifierStack& ostack) {
    const auto* aff = affordance_for(i);
    if (!aff)
        return;
    if (!aff->occupant_grant.empty()) {
        effect_desc fx;
        fx.effect_id = aff->role_tag + ".grant";  // opaque audit string
        fx.bundle = aff->occupant_grant;
        // Invariant: grant_source(i) was pre-checked free on this stack.
        apply_effect(ostack, grant_source(i), fx);
    }
    if (!aff->when_unmanned.empty())
        m_ship.remove_source(unmanned_source(i));  // station is manned now
}

void SeatLink::reverse_seat(std::size_t i, ModifierStack& ostack) {
    const auto* aff = affordance_for(i);
    if (!aff)
        return;
    if (!aff->occupant_grant.empty())
        ostack.remove_source(grant_source(i));  // the unit drops as one
    if (!aff->when_unmanned.empty()) {
        effect_desc fx;
        fx.effect_id = aff->role_tag + ".unmanned";
        fx.bundle = aff->when_unmanned;
        apply_effect(m_ship, unmanned_source(i), fx);
    }
}

std::expected<void, link_error> SeatLink::sit(std::uint64_t occupant,
                                              std::string_view seat,
                                              ModifierStack& ostack) {
    const auto i = find_seat(seat);
    if (!i)
        return std::unexpected(link_error::seat_not_found);
    const auto old = seated_at(occupant);
    if (old && *old == *i)
        return {};  // already in that seat: no-op, stacks untouched

    if (const auto cur = m_occ.occupant_at(seat))
        if (*cur != occupant)
            return std::unexpected(link_error::seat_busy);

    // Conflict pre-check: reserved grant id must be free on this stack
    // (a caller source squatting in the reserved range is a wiring bug).
    if (const auto* aff = affordance_for(*i);
        aff && !aff->occupant_grant.empty() && ostack.has_source(grant_source(*i)))
        return std::unexpected(link_error::stack_conflict);

    if (const auto r = m_occ.sit(occupant, seat); !r)
        return std::unexpected(link_error::seat_busy);

    // Stats as a unit: old seat's grant drops (and its station re-arms
    // when_unmanned), new seat's grant lands (and its zeroing lifts).
    if (old) {
        reverse_seat(*old, ostack);
        if (m_bus)
            m_bus->emit(rail_kind::seat_drop, occupant,
                        static_cast<std::uint64_t>(*old) + 1);
    }
    apply_seat(*i, occupant, ostack);
    if (m_bus)
        m_bus->emit(rail_kind::seat_attach, occupant,
                    static_cast<std::uint64_t>(*i) + 1);
    return {};
}

std::expected<void, link_error> SeatLink::stand(std::uint64_t occupant,
                                                std::string_view seat,
                                                ModifierStack& ostack) {
    const auto i = find_seat(seat);
    if (!i)
        return std::unexpected(link_error::seat_not_found);
    const auto cur = m_occ.occupant_at(seat);
    if (!cur || *cur != occupant)
        return std::unexpected(link_error::not_seated);

    if (const auto r = m_occ.stand(seat); !r)
        return std::unexpected(link_error::not_seated);

    reverse_seat(*i, ostack);  // grant drops as one unit; station re-arms
    if (m_bus)
        m_bus->emit(rail_kind::seat_drop, occupant,
                    static_cast<std::uint64_t>(*i) + 1);
    return {};
}

std::expected<void, link_error> SeatLink::drop(std::uint64_t /*claimant*/,
                                               std::string_view seat,
                                               ModifierStack& ostack) {
    // KO / forced ejection (#18d): whoever physically holds the seat goes,
    // and THEIR stack is the one passed (the sim knows the victim).
    const auto i = find_seat(seat);
    if (!i)
        return std::unexpected(link_error::seat_not_found);
    const auto cur = m_occ.occupant_at(seat);
    if (!cur)
        return std::unexpected(link_error::not_seated);

    if (const auto r = m_occ.stand(seat); !r)
        return std::unexpected(link_error::not_seated);

    reverse_seat(*i, ostack);
    if (m_bus)
        m_bus->emit(rail_kind::seat_drop, *cur,
                    static_cast<std::uint64_t>(*i) + 1);
    return {};
}

}  // namespace ve::core::sim

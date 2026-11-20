// Phase 6 task 6.7 — see seat_link.h for the contract.
#include "ve/core/sim/seat_link.h"

#include <bit>
#include <cmath>

#include "ve/core/fnv.h"
#include "ve/core/rng.h"

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


// --- 9.19 (#18d): hit on a manned station rolls onto the occupant. ---------
// Keyed draw (splitmix64 chain, gun/contact convention): the seat id is
// hashed with the SHARED Fnv1a64 (same primitive as the replay/stats hashes,
// "compatible with replay" = bitwise); tick and the per-hit input's RAW fp64
// bits ride the key so two hits at different ticks, or hits of different
// strength at the same tick, land on different stream positions.
station_hit_outcome SeatLink::station_hit(std::string_view seat, std::uint64_t world_seed,
                                          tick_t tick, double per_hit_input,
                                          double ko_severity, double injury_severity,
                                          const effect_desc& ko_effect,
                                          const effect_desc& injury_effect,
                                          std::uint64_t effect_source_id,
                                          ModifierStack& occupant_stack) {
    const auto i = find_seat(seat);
    if (!i)
        return station_hit_outcome::none;  // not a station component here
    const auto who = m_occ.occupant_at(seat);
    if (!who)
        return station_hit_outcome::none;  // empty seat: the 9.11 health path owns it
    if (!std::isfinite(per_hit_input) || !(per_hit_input > 0.0))
        return station_hit_outcome::none;  // fail-safe: garbage input, no roll, no drop

    Fnv1a64 seat_hash;
    for (const char c : seat)
        seat_hash.mix_byte(static_cast<std::uint8_t>(c));
    std::uint64_t h = splitmix64(world_seed ^ (seat_hash.value() * 0x9E3779B97F4A7C15ULL));
    h = splitmix64(h ^ (static_cast<std::uint64_t>(tick) * 0xBF58476D1CE4E5B9ULL
                        + 0x94D049BB133111EBULL));
    h = splitmix64(h ^ (std::bit_cast<std::uint64_t>(per_hit_input) * 0x9E3779B97F4A7C15ULL));
    const double s = unit01(h) * per_hit_input;  // authored severity scaling

    if (s >= ko_severity) {
        // Status effect lands on the OCCUPANT's stack first (recovery reads
        // it there); then the seat drops as a unit — occupancy + grant +
        // rail event through the shipped drop path. Drop is unconditional:
        // a stats-side apply conflict must not keep a KO'd body in the seat.
        if (!ko_effect.bundle.empty())
            apply_effect(occupant_stack, effect_source_id, ko_effect);
        // std::expected is [[nodiscard]]; the drop MUST happen even if it
        // reports a link_error (see comment above) — discard is intentional,
        // so silence the wall explicitly instead of altering behaviour.
        static_cast<void>(drop(*who, seat, occupant_stack));
        return station_hit_outcome::ko;
    }
    if (s >= injury_severity) {
        if (!injury_effect.bundle.empty())
            apply_effect(occupant_stack, effect_source_id, injury_effect);
        return station_hit_outcome::injury;
    }
    return station_hit_outcome::none;
}

}  // namespace ve::core::sim

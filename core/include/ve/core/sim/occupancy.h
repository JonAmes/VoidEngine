#pragma once
// Phase 5 task 5.11 — seat occupancy machinery (decision #18). `core/`
// knows only "occupancy": a seat is a mount whose tags intersect the
// caller-supplied role-tag registry (game/ names the roles; core never
// does — AGENTS rule 1). This tracks WHO is in WHICH seat, seat darkness
// (#18d health, "dark-not-dead"), and answers the dead-until-manned check
// (#18a) as a plain occupancy/tag query.
//
// THE PHASE 6 SEAM: sitting/standing/KO/dark changing availability is the
// only signal here. Attaching/dropping the occupant's StatsSet modifier
// stack "as a unit" (#11/#18) subscribes to exactly these transitions; no
// stats machinery lives in this file.
//
// Determinism (#5): seats iterate in loadout mount order; every mutation
// is atomic — a call either fully applies or changes nothing, and sit()
// MOVES an already-seated occupant in one step (swap recomputes cleanly).

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ve/core/sim/loadout.h"

namespace ve::core::sim {

// Why a sit/stand/set_dark request was refused. First-cause ordering is
// fixed: unknown_seat is checked before seat_busy/not_seated.
enum class seat_error : std::uint8_t {
    unknown_seat,
    seat_busy,   // someone else is already in the target seat
    not_seated,  // stand requested at an empty seat
};

// One seat view: the mount + its registry roles + live occupancy.
struct seat_desc {
    std::string instance_id;               // the seat component's mount id
    std::vector<std::string> role_tags;    // mount tags ∩ registry (mount-tag order)
    bool dark = false;                     // #18d: dark-not-dead (health event)
    std::optional<std::uint64_t> occupant; // opaque occupant id
};

class Occupancy {
public:
    // Seeds seats from loadout mounts carrying ANY registry tag, in mount
    // order. The registry is data (game/kPhase5_role_tags or cooked sets).
    Occupancy(const Loadout& loadout, std::span<const std::string> role_tag_registry);

    const std::vector<seat_desc>& seats() const { return m_seats; }

    // Sit: unknown_seat if the seat is not a seat. If `occupant` already
    // sits elsewhere, they MOVE (detach + attach atomically, one call).
    // If the target seat holds a DIFFERENT occupant: seat_busy, no change.
    // Dark seats accept sitters (someone can walk in and patch a dark
    // section, #19c); availability is a separate query.
    std::expected<void, seat_error> sit(std::uint64_t occupant, std::string_view seat);

    // Stand at `seat`: not_seated if empty.
    std::expected<void, seat_error> stand(std::string_view seat);

    // #18d health transition (hit took the station dark / repair brought
    // it back). Never changes occupancy — availability only.
    std::expected<void, seat_error> set_dark(std::string_view seat, bool dark);

    // Queries ------------------------------------------------------------
    std::optional<std::uint64_t> occupant_at(std::string_view seat) const;
    // Seat instance currently holding `occupant`, if any.
    std::optional<std::string_view> seat_of(std::uint64_t occupant) const;
    // #18a dead-until-manned: the seat's function runs only when seated
    // AND not dark. False for unknown ids.
    bool function_available(std::string_view seat) const;
    // First (mount-order) manned, non-dark seat carrying `role`; the
    // gunner-KO'd-goes-silent check (#18d) is `manned(role) == false`.
    std::optional<std::uint64_t> occupant_for_role(std::string_view role) const;
    bool manned(std::string_view role) const { return occupant_for_role(role).has_value(); }

private:
    int seat_index(std::string_view id) const;  // -1 when not a seat

    std::vector<seat_desc> m_seats;
};

}  // namespace ve::core::sim

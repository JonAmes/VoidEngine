#pragma once
// Phase 6 task 6.7 — seat <-> stats linkage (done-when (c); decision #18:
// the modifier stack an occupied seat affords attaches to the occupant
// when seated and drops when NOT, AS A UNIT). This file is that seam:
// it subscribes to exactly the sit/stand/drop transitions of Occupancy
// (which stayed stats-free in 5.11) and applies/reverses 6.6 EFFECTS.
//
// What a role affords is COOKED/GAME DATA — an affordance is two modifier
// bundles keyed by role tag:
//   occupant_grant   applied to the OCCUPANT's stack while seated
//                    (pilot reflex boost, gunner aim assist...)
//   when_unmanned    applied to the SHIP's stack while the seat is empty
//                    (#18b: the pilot seat zeroes engine-command function
//                    of unmanned stations — a zeroing modifier, exactly
//                    the #26 "function-zeroed until repaired" pattern,
//                    but for manning).
// core/ never names roles: role tags are opaque strings; the game's
// registry (game/kPhase5_role_tags) built the seats already.
//
// Source-id layout (deterministic, per-link): seat index i (Occupancy
// order) owns source kSeatGrantBase+i in the occupant's stack and
// kSeatUnmannedBase+i in the ship stack. Occupant stacks are per-entity,
// so grants never collide with other occupants; ship-stack ids are unique
// by index. Callers must keep their own source ids out of these ranges
// (bases are near the top of the u64 space by convention).
//
// Every mutation also emits the #28b rail event (seat_attach / seat_drop)
// on the optional EffectEventBus, so replays and scrubbing see the same
// transitions the stats did.
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ve/core/sim/effect_bus.h"
#include "ve/core/sim/effects.h"
#include "ve/core/sim/modifiers.h"
#include "ve/core/sim/occupancy.h"
#include "ve/core/sim/time.h"

namespace ve::core::sim {

inline constexpr std::uint64_t kSeatGrantBase    = 0xFFFF'FFFF'0000'0000ull;
inline constexpr std::uint64_t kSeatUnmannedBase = 0xFFFF'FFFF'8000'0000ull;

struct seat_affordance {
    std::string role_tag;                       // matches seat_desc.role_tags entries
    std::vector<modifier_desc> occupant_grant;  // -> occupant stack while seated
    std::vector<modifier_desc> when_unmanned;   // -> ship stack while seat empty
};

// Refusal causes for link operations (its own checks run BEFORE Occupancy's).
enum class station_hit_outcome : std::uint8_t { none, injury, ko };

enum class link_error : std::uint8_t {
    seat_not_found,   // not a seat at all (unknown or not seat-tagged)
    stack_conflict,   // a reserved source id already in use on the target stack
    seat_busy,        // occupant collision, mirrored from occupancy
    not_seated,
};

class SeatLink {
public:
    SeatLink(Occupancy& occ, ModifierStack& ship_stack, EffectEventBus* bus = nullptr)
        : m_occ(occ), m_ship(ship_stack), m_bus(bus) {}

    // Cooked-data registration (ordered; first match on a role tag wins).
    bool add_affordance(const seat_affordance& a);

    // Sit / stand / forced-drop through the link so stats + rail events
    // stay atomic with occupancy. `occupant_stack` is that entity's stack.
    // sit() on an already-seated occupant MOVES (occupancy semantics):
    // old seat's grant drops, new seat's grant lands, old seat re-gains
    // its when_unmanned bundle — all in one call.
    std::expected<void, link_error> sit(std::uint64_t occupant, std::string_view seat,
                                        ModifierStack& occupant_stack);
    std::expected<void, link_error> stand(std::uint64_t occupant, std::string_view seat,
                                          ModifierStack& occupant_stack);
    // #18d KO / forced ejection: same reversal, rail event is seat_drop.
    std::expected<void, link_error> drop(std::uint64_t occupant, std::string_view seat,
                                         ModifierStack& occupant_stack);

    // 9.19 (#18d): a weapon hit that lands on a MANSED station component
    // rolls ONTO the occupant. Deterministic severity draw keyed on
    // (world seed, tick, FNV(seat id), per-hit input) — the #18d/#20d/#31b
    // stream pattern; `per_hit_input` (the caller's hit strength, e.g. the
    // 9.11 overflow) rides the key AND scales severity: s = u * input.
    //   s >= ko_severity      KO: ko_effect lands on the OCCUPANT's stack,
    //                         then the seat DROPS (grant reverses as a unit,
    //                         seat unmanned -> crew-gated functions silent).
    //   s >= injury_severity  injury: bundle lands, occupant stays seated.
    //   else                  none (grazed).
    // Empty seat, unknown seat, or non-finite/<=0 input: NO roll (fail-safe;
    // the hit just damaged the component through the 9.11 path).
    // `effect_source_id` is caller-owned (remove_source = recovery at a
    // medbay). A source conflict fails the bundle apply but STILL drops the
    // seat: the KO's occupancy effect is not conditional on the stats side.
    station_hit_outcome station_hit(std::string_view seat, std::uint64_t world_seed,
                                    tick_t tick, double per_hit_input,
                                    double ko_severity, double injury_severity,
                                    const effect_desc& ko_effect,
                                    const effect_desc& injury_effect,
                                    std::uint64_t effect_source_id,
                                    ModifierStack& occupant_stack);

    // The source ids a seat uses (exposed for audit/tests).
    static constexpr std::uint64_t grant_source(std::size_t seat_i) {
        return kSeatGrantBase + seat_i;
    }
    static constexpr std::uint64_t unmanned_source(std::size_t seat_i) {
        return kSeatUnmannedBase + seat_i;
    }

private:
    std::optional<std::size_t> find_seat(std::string_view seat) const;
    std::optional<std::size_t> seated_at(std::uint64_t occupant) const;
    void apply_seat(std::size_t i, std::uint64_t occupant, ModifierStack& ostack);
    void reverse_seat(std::size_t i, ModifierStack& ostack);
    const seat_affordance* affordance_for(std::size_t seat_i) const;

    Occupancy& m_occ;
    ModifierStack& m_ship;
    EffectEventBus* m_bus;  // not owned
    std::vector<seat_affordance> m_affordances;  // registration order
};

}  // namespace ve::core::sim

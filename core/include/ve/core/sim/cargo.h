#pragma once
// Cargo manifest (#20a): commodity stacks + owned spare gear, part of the
// loadout data model. CONCEPT-FREE like the rest of core/sim: a mount is a
// cargo hold exactly when it carries a caller-supplied hold tag; its capacity
// is the value of a caller-supplied attribute key; its commodity gates are
// the mount's tags that start with a caller-supplied gate prefix (the suffix
// is the opaque commodity id). Commodity ids, type ids and all key/tag names
// are cooked/game-side data -- core never names "fuel" or "cargo" itself and
// interprets nothing (AGENTS rule 1). The manifest owns deterministic
// ordered containers and atomic add/remove with explicit overflow errors.
//
// Out of scope here (later seams, same discipline as occupancy):
//   - market transactions (#20b) and price: Phase 8/9 -- this is the
//     possession container the market moves through;
//   - bolting a spare onto a socket (hangar #15/#24c): the mount path in
//     loadout already validates it; here spares are roster entries only;
//   - mass accounting: cargo mass aggregates into the ship like every other
//     component's mass in Phase 6, not here.
//
// Capacity model (ratified in tests): capacity lives PER HOLD; adding a
// quantity fills the accepting holds in mount order (first-fit split across
// holds), removing drains those placements in reverse mount order. A hold
// with NO gate tags accepts every commodity (open bulk hold); a gated hold
// accepts only its listed commodity ids. Spares are not bulk commodities and
// do not occupy commodity capacity (#20a lists them side by side).

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ve/core/sim/loadout.h"

namespace ve::core::sim {

// First-cause order for add_commodity: invalid_quantity, no_gate, overflow.
enum class cargo_error {
    invalid_quantity,   // <= 0, NaN, or inf
    no_gate,            // no hold accepts this commodity id
    overflow,           // accepting holds lack the room (nothing stored)
    insufficient,       // removing more than carried
    unknown_commodity,  // no such stack
    duplicate_spare,    // spare instance id already on the roster
    unknown_spare,      // no such spare to remove
    invalid_id,         // empty commodity/spare id
};

// Where a commodity actually sits: one placement per holding hold, ordered by
// hold (mount) order. Sum over placements == stack total.
struct placement_desc {
    std::size_t hold_index;  // index into holds()
    double quantity;
    friend bool operator==(const placement_desc&, const placement_desc&) = default;
};

struct stack_desc {
    std::string commodity_id;  // opaque (cooked data)
    std::vector<placement_desc> placements;
    double total() const;
};

struct spare_desc {
    std::string instance_id;  // opaque; unique across the roster
    std::string type_id;      // same id space as loadout component types
};

struct hold_desc {
    std::string mount_id;                     // the mount carrying the tag
    double capacity;                          // from the supplied attr key
    std::vector<std::string> gates;           // accepted commodity ids
    bool accepts(std::string_view commodity) const;  // empty gates = all
};

class CargoManifest {
public:
    // Scans loadout.mounts() in mount order. A mount is a hold iff it
    // carries hold_tag; capacity = its attr under capacity_key (absent ->
    // 0.0, an uncapacitated hold is inert but legal); gates = its tags that
    // start with gate_prefix, with the prefix stripped.
    CargoManifest(const Loadout& loadout, std::string_view hold_tag,
                  std::string_view capacity_key, std::string_view gate_prefix);

    std::expected<void, cargo_error> add_commodity(std::string_view commodity,
                                                   double qty);
    std::expected<void, cargo_error> remove_commodity(std::string_view commodity,
                                                      double qty);
    std::expected<void, cargo_error> add_spare(std::string_view instance_id,
                                               std::string_view type_id);
    std::expected<void, cargo_error> remove_spare(std::string_view instance_id);

    double quantity_of(std::string_view commodity) const;  // 0 when absent
    std::span<const hold_desc> holds() const { return m_holds; }
    std::span<const stack_desc> stacks() const { return m_stacks; }  // insertion order
    std::span<const spare_desc> spares() const { return m_spares; }

private:
    std::vector<hold_desc> m_holds;
    std::vector<stack_desc> m_stacks;  // one per commodity, insertion order
    std::vector<spare_desc> m_spares;  // roster order

    stack_desc* find_stack(std::string_view commodity);
    const stack_desc* find_stack(std::string_view commodity) const;
};

}  // namespace ve::core::sim

#pragma once
// Phase 6 task 6.6 — effects + conditions (PLAN: "Effects & conditions").
//
// An EFFECT is a bundle of modifiers applied as ONE atomic source: the
// whole bundle lands on its own source id or nothing lands. Damage /
// heal / shield-drain are all expressible as pure data here (negative /
// positive flat_add on some attribute) — core/ never names a combat word;
// the vocabularies are cooked/game strings like every other key.
//   - timed effect: expire_tick stamped on every bundle member (the whole
//     bundle drops together via ModifierStack::expire_due).
//   - instant effect: expire_tick nullopt; members live until the effect
//     source is removed (repair/dispel) — the #26 "function-zeroing
//     modifier until repaired" pattern is exactly this.
// Atomicity covers policy rejects too: if any bundle member would be
// rejected (cap/exclusive), the partially-added source is dropped and
// NOTHING of the effect survives. One effect = one source = one remove.
//
// A CONDITION is a pure-data gate: required/forbidden gameplay tags
// (ancestor semantics, 6.2) plus attribute tests against DERIVED values
// (6.4). "Gate on a function" is composition outside this struct — the
// sim ANDs its own predicates with condition_met; keeping the struct
// data-only keeps it hashable, loggable, and cooked-data-representable.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ve/core/sim/modifiers.h"
#include "ve/core/sim/stats.h"
#include "ve/core/sim/tags.h"

namespace ve::core {

struct effect_desc {
    std::string effect_id;                       // opaque (effect name, item id...)
    std::optional<std::uint64_t> expire_tick;    // stamped over every member's expiry
    std::vector<modifier_desc> bundle;           // the modifiers, applied in order
};

enum class effect_result : std::uint8_t {
    applied,
    invalid,             // empty bundle, bad member, or source id already taken
    rejected_policy,     // a member hit cap/exclusive; fully rolled back
};

// Apply `fx` as source `effect_source_id` (owned by the effect for its
// whole life; removal is ModifierStack::remove_source(effect_source_id)).
// All-or-nothing. Tags on each member carry through for the dispel path
// (ModifierStack::remove_modifiers_tagged).
effect_result apply_effect(ModifierStack& stack, std::uint64_t effect_source_id,
                           const effect_desc& fx);

// --- conditions -------------------------------------------------------------

enum class cmp_op : std::uint8_t { less, less_equal, greater, greater_equal, equal };

struct attr_test {
    std::string attr;      // tested against the DERIVED value (pipeline view)
    cmp_op op = cmp_op::greater_equal;
    float threshold = 0.0f;
};

struct condition {
    std::vector<std::string> require_tags;  // ALL must be answered (ancestor match)
    std::vector<std::string> forbid_tags;   // NONE may be answered
    std::vector<attr_test> attr_tests;      // ALL must pass; missing attr -> fail
};

// Null section pointer = that section is vacuously satisfied (compose
// partial conditions freely). fp32 == tests are exact-bit comparisons;
// authors who want ranges use the ordered ops.
bool condition_met(const condition& c, const TagSet* tags, const StatsSet* stats);

}  // namespace ve::core

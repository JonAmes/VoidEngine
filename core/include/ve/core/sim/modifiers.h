#pragma once
// Phase 6 task 6.3 — modifier sources + the stack container (PLAN #16
// "Modifier stack"). A modifier is one rule over one attribute: op
// (flat_add | percent | multiplier), value, and the identity/policy data
// that decides how it combines with previously-applied modifiers.
//
// Stacking identity: (target_attr, stack_id) — stack_id is an opaque
// caller-chosen string naming "the same effect" (an effect name in 6.6, a
// component instance in 6.7/6.10...). Two modifiers sharing an identity
// interact under the INCOMING modifier's policy:
//   replace   — drop every existing same-identity modifier, insert this one
//   refresh   — keep existing values; overwrite their expiry with the new
//               one's (nullopt = become permanent)
//   stack_n   — insert iff current same-identity count < stack_cap
//   exclusive — insert iff NO same-identity modifier exists at all
// A modifier with a fresh identity (no collision) simply appends.
//
// Cycles rejected BY CONSTRUCTION (PLAN: "rejected by design, not
// debugged"): modifier_desc::target_attr names a BASE attribute and the
// data model has no edge from a modifier to another modifier — a
// self-modifying cycle or dependent chain is not a thing you can build
// here, so there is nothing to detect. The 6.4 pipeline only ever reads
// base values.
//
// Determinism: sources and each source's modifiers are insertion-ordered
// vectors; "the ordered view" = source order then modifier order, total
// and stable (AGENTS rule 5). Sources are opaque u64 ids (entity ids,
// mount indices, effect ids, seat ids — caller's namespace; core never
// interprets them).
//
// Expiry is tick-absolute (fixed sim ticks, #45): a modifier is live while
// current_tick < expire_tick; expire_due(tick) drops due ones deterministically.
// `revision()` bumps on every mutation — that is the dirty-flag clock the
// 6.4 recompute-on-read pipeline rides.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ve/core/sim/tags.h"

namespace ve::core {

enum class mod_op : std::uint8_t { flat_add, percent, multiplier };
enum class stack_policy : std::uint8_t { replace, refresh, stack_n, exclusive };

struct modifier_desc {
    std::string target_attr;                  // base attribute key (opaque)
    std::string stack_id;                     // stacking identity partner key
    mod_op op = mod_op::flat_add;
    float value = 0.0f;
    stack_policy policy = stack_policy::exclusive;  // incoming-collision rule
    std::uint32_t stack_cap = 1;              // stack_n only: max live instances incl. this
    std::optional<std::uint64_t> expire_tick; // live while tick < this; nullopt = permanent
    std::vector<std::string> tags;            // opaque descriptors (gates, dispel, UI)
};

enum class mod_add_result : std::uint8_t {
    added,
    replaced,        // colliding identities dropped, this one inserted
    refreshed,       // existing kept, expiry overwritten
    rejected_cap,    // stack_n at capacity
    rejected_exclusive,
    source_unknown,
    invalid,         // empty attr/stack_id, non-finite value, stack_cap 0
};

class ModifierStack {
public:
    struct modifier {
        std::string target_attr;
        std::string stack_id;
        mod_op op;
        float value;
        stack_policy policy;
        std::uint32_t stack_cap;
        std::optional<std::uint64_t> expire_tick;
        TagSet tags;
    };

    struct source {
        std::uint64_t id;
        TagSet tags;                    // describes the SOURCE (seat, component...)
        std::vector<modifier> mods;     // insertion order
    };

    // --- sources -------------------------------------------------------
    bool add_source(std::uint64_t id, std::vector<std::string> tags = {});  // dup id -> false
    bool remove_source(std::uint64_t id);   // drops its modifiers with it
    [[nodiscard]] bool has_source(std::uint64_t id) const;
    [[nodiscard]] std::size_t source_count() const { return m_sources.size(); }

    // Ordered view: source order (insertion), then modifier order. Total,
    // stable, never re-sorted — 6.4 aggregation and 6.5 bus order ride it.
    [[nodiscard]] const std::vector<source>& sources() const { return m_sources; }

    // --- modifiers -----------------------------------------------------
    mod_add_result add_modifier(std::uint64_t source_id, const modifier_desc& md);
    std::size_t expire_due(std::uint64_t tick);  // drops live-at-tick-arrives set; returns count

    // Tag queries across MODIFIER tags (ancestor semantics, 6.2):
    [[nodiscard]] std::size_t count_modifiers_tagged(std::string_view query) const;
    std::size_t remove_modifiers_tagged(std::string_view query);  // dispel path (6.6)
    // And across SOURCE tags:
    [[nodiscard]] bool source_tagged(std::uint64_t id, std::string_view query) const;

    [[nodiscard]] std::size_t modifier_count() const;

    // Bumped on every mutation (source add/remove, modifier add/refresh/
    // expire/remove). 6.4's recompute-on-read compares per-attribute stamps
    // against this; never a per-tick full pass.
    [[nodiscard]] std::uint64_t revision() const { return m_revision; }

private:
    [[nodiscard]] source* find_source(std::uint64_t id);
    [[nodiscard]] const source* find_source(std::uint64_t id) const;
    std::size_t count_identity(std::string_view attr, std::string_view stack_id) const;
    std::size_t drop_identity(std::string_view attr, std::string_view stack_id);
    void touch() { ++m_revision; }

    std::vector<source> m_sources;  // insertion order = the only iteration order
    std::uint64_t m_revision = 0;
};

}  // namespace ve::core

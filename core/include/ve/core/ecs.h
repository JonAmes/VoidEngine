#pragma once
// Phase 8 task 8.2.5 — the minimal ECS registry (PLAN #8: "entity +
// component arrays", archetype-lite). No earlier phase ever needed it;
// the 8.3 inspector is the first consumer, so it lands here, small and
// honest, NOT a Phase 12 hot-system rewrite.
//
// Hard-boundary notes:
//  - AGENTS rule: no RTTI in core/ — component dispatch is by REGISTRATION
//    INDEX (register_component_type returns it; game/ owns the spellings).
//    Type-erased storage is shared_ptr<void>: the deleter is captured at
//    creation, so deleting through void is defined WITHOUT RTTI.
//  - 6.1 opaque-names rule: names are DATA the game side supplies; core
//    stores and echoes them, never interprets one (no component is ever
//    named "Piloting"/"Laser" in this file).
//  - Determinism (rule 5): slots iterate in creation order, component type
//    lists ascend by registration index, the free list is LIFO but ids stay
//    unique via generation bumps. No unordered containers anywhere.
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ve/core/assert.h"  // VE_ASSERT in the emplace() template

namespace ve::core {

struct entity_id {
    std::int32_t slot = -1;
    std::uint32_t generation = 0;  // free-list reuse bumps it: stale ids die
    friend bool operator==(const entity_id&, const entity_id&) = default;
};

class ecs_world {
public:
    // Register a component-type name (opaque). Returns its type index =
    // registration order. Re-registering the SAME name returns the same
    // index (panels and game code ask repeatedly). Index 0 is a valid type.
    std::uint32_t register_component_type(std::string_view name);
    [[nodiscard]] std::span<const std::string> component_type_names() const {
        return m_names;
    }

    // Create appends a slot (or pops the free list, bumping generation).
    entity_id create();
    [[nodiscard]] bool alive(entity_id e) const;
    bool destroy(entity_id e);
    [[nodiscard]] std::size_t entity_count() const { return m_live; }

    // --- typed component access (door over the void* storage) -----------
    // Replacing an existing component swaps the blob (old value destructs).
    template <class T>
    T& emplace(entity_id e, std::uint32_t type, T v) {
        VE_ASSERT(alive(e) && type < m_names.size());
        auto& blob = m_slots[static_cast<std::size_t>(e.slot)].comps[type];
        blob = std::shared_ptr<void>(new T(std::move(v)));
        return *static_cast<T*>(blob.get());
    }
    template <class T>
    [[nodiscard]] T* get(entity_id e, std::uint32_t type) const {
        if (!alive(e) || type >= m_names.size()) return nullptr;
        return static_cast<T*>(
            m_slots[static_cast<std::size_t>(e.slot)].comps[type].get());
    }
    bool remove(entity_id e, std::uint32_t type);
    [[nodiscard]] bool has(entity_id e, std::uint32_t type) const;

    // Tree view for inspector panels: live entities in SLOT order, each
    // with its held component types ASCENDING by registration index.
    struct entity_view {
        entity_id id;
        std::vector<std::uint32_t> types;
        friend bool operator==(const entity_view&, const entity_view&) = default;
    };
    [[nodiscard]] std::vector<entity_view> entities() const;

private:
    struct ent_slot {
        std::uint32_t generation = 0;
        bool alive = false;
        std::vector<std::shared_ptr<void>> comps;  // indexed by type index
    };

    std::vector<std::string> m_names;
    std::vector<ent_slot> m_slots;
    std::vector<std::int32_t> m_free;
    std::size_t m_live = 0;
};

}  // namespace ve::core

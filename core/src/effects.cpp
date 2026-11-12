// Phase 6 task 6.6 — see effects.h for the contract.
#include "ve/core/sim/effects.h"

#include <cmath>

namespace ve::core {

effect_result apply_effect(ModifierStack& stack, std::uint64_t effect_source_id,
                           const effect_desc& fx) {
    // Validate EVERYTHING before touching the stack (atomicity part 1).
    if (fx.bundle.empty() || stack.has_source(effect_source_id))
        return effect_result::invalid;
    for (const auto& md : fx.bundle)
        if (md.target_attr.empty() || md.stack_id.empty() || !std::isfinite(md.value) ||
            (md.policy == stack_policy::stack_n && md.stack_cap == 0))
            return effect_result::invalid;

    if (!stack.add_source(effect_source_id))
        return effect_result::invalid;  // raced/taken; nothing added

    // Atomicity part 2: any policy reject rolls the whole effect back.
    for (const auto& md_in : fx.bundle) {
        modifier_desc md = md_in;
        if (fx.expire_tick)
            md.expire_tick = fx.expire_tick;  // timed effects expire as a unit
        const mod_add_result r = stack.add_modifier(effect_source_id, md);
        if (r != mod_add_result::added && r != mod_add_result::replaced &&
            r != mod_add_result::refreshed) {
            stack.remove_source(effect_source_id);  // full rollback
            return effect_result::rejected_policy;
        }
    }
    return effect_result::applied;
}

bool condition_met(const condition& c, const TagSet* tags, const StatsSet* stats) {
    if (tags) {
        for (const auto& t : c.require_tags)
            if (!tags->contains(t))
                return false;
        for (const auto& t : c.forbid_tags)
            if (tags->contains(t))
                return false;
    }
    if (stats) {
        for (const auto& a : c.attr_tests) {
            const auto v = stats->derived(a.attr);
            if (!v.has_value())
                return false;  // untestable attribute = gate closed (fail-safe)
            switch (a.op) {
            case cmp_op::less:         if (!(*v <  a.threshold)) return false; break;
            case cmp_op::less_equal:   if (!(*v <= a.threshold)) return false; break;
            case cmp_op::greater:      if (!(*v > a.threshold)) return false; break;
            case cmp_op::greater_equal:if (!(*v >= a.threshold)) return false; break;
            case cmp_op::equal:        if (*v != a.threshold) return false; break;
            }
        }
    }
    return true;
}

}  // namespace ve::core

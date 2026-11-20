// Phase 9 task 9.21 — FSM transition law. Pure thresholds, fixed order.
#include "ve/game/ai_pilot.h"

namespace ve::game {

const char* ai_state_name(const ai_state s) {
    switch (s) {
    case ai_state::patrol:     return "patrol";
    case ai_state::engage:     return "engage";
    case ai_state::disengage:  return "disengage";
    case ai_state::withdraw:   return "withdraw";
    }
    return "?";
}

ai_state ai_next_state(const ai_state cur,
                       const std::optional<double> contact_slant,
                       const bool faction_hostile,
                       const double own_integrity,
                       const ai_pilot_attrs& attrs) {
    // "have a live hostile contact" — hostility is a resolved tag (#25a),
    // the slant is the shared contact-board measurement (9.16).
    const bool have = faction_hostile && contact_slant.has_value();
    switch (cur) {
    case ai_state::patrol:
        // acquire: at-or-inside engage range (<=: boundary acquires).
        if (have && *contact_slant <= attrs.engage_range) return ai_state::engage;
        return ai_state::patrol;
    case ai_state::engage:
        // retreat threshold FIRST: a hit landing on the same tick the
        // target crosses the range boundary withdraws, not re-holds.
        if (own_integrity < attrs.withdraw_integrity) return ai_state::withdraw;
        // hysteresis: losing the contact needs disengage_range (> boundary).
        if (!have || *contact_slant > attrs.disengage_range) return ai_state::disengage;
        return ai_state::engage;
    case ai_state::disengage:
        // aggression is the re-engagement reach multiplier; 0 never bites.
        if (have && attrs.aggression > 0.0 &&
            *contact_slant <= attrs.engage_range * attrs.aggression)
            return ai_state::engage;
        return ai_state::patrol;
    case ai_state::withdraw:
        // disengaged enough (contact gone or out of reach) -> resume patrol.
        if (!have || *contact_slant > attrs.disengage_range) return ai_state::patrol;
        return ai_state::withdraw;
    }
    return cur;  // unreachable; fail-safe keeps enum domain total
}

ai_intent ai_intent_of(const ai_state s) {
    return {s, s == ai_state::engage};
}

}  // namespace ve::game

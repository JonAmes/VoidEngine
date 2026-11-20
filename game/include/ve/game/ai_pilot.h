#pragma once
// Phase 9 task 9.21 — combat AI FSM pilot, game-side (#21 "Slice AI").
//
// Four states (patrol / engage / disengage / withdraw). The FSM is a PURE
// transition function of (current state, contact snapshot, own integrity,
// authored attributes, faction hostility tag) — no stored randomness, no
// wall-clock: transitions land at hand-computable ticks inside the replay
// hash (determinism rule #5). The pilot flies with the SAME assist
// controllers and stats the player uses (#21): the FSM only emits intent
// (state + pursue point + weapons-free); the host turns intent into
// tick_command through core assist controllers, and the 9.19 gun station
// gate decides whether "weapons free" can actually fire.
//
// Aggression, engagement range and retreat thresholds are ATTRIBUTES
// (aggression 0..1, ranges m, integrity fraction 0..1) — the pilot's
// StatsSet feeds these; no magic constants inside the FSM.
//
// Hostility comes from tags, never numbers (#25a/#20c): the caller
// resolves owner x stance x reputation into `hostile` (select_targets,
// 9.20), and reputation threshold flips run through core apply_bands —
// the FSM only sees the resulting faction.hostile tag.
#include <cstdint>
#include <optional>

namespace ve::game {

enum class ai_state : std::uint8_t { patrol, engage, disengage, withdraw };

[[nodiscard]] const char* ai_state_name(ai_state s);

// Authored pilot attrs (SI; from the pilot/seat StatsSet).
struct ai_pilot_attrs {
    double engage_range = 0.0;      // acquire/hold radius, m
    double disengage_range = 0.0;   // beyond this the contact is lost, m
    double withdraw_integrity = 0.0; // own integrity fraction below -> withdraw
    double aggression = 0.0;        // [0,1]: re-engagement reach from
                                    // disengage = engage_range * aggression
};

// One tick of the FSM. `contact_slant` is the CURRENT measured range to
// the single hostile contact the board offers (9.16 shared picture —
// nullopt = no contact). `faction_hostile` = faction.hostile tag live.
// Evaluation order is FIXED (withdraw check first inside engage, etc.)
// so ties resolve identically on replay.
[[nodiscard]] ai_state ai_next_state(ai_state cur,
                                     std::optional<double> contact_slant,
                                     bool faction_hostile,
                                     double own_integrity,
                                     const ai_pilot_attrs& attrs);

// Intent emitted with the resolved state: pursue point (contact position
// when engaging, waypoint when patrolling — caller supplies) and
// weapons-free (true only while engaged).
struct ai_intent {
    ai_state state = ai_state::patrol;
    bool weapons_free = false;
};
[[nodiscard]] ai_intent ai_intent_of(ai_state s);

}  // namespace ve::game

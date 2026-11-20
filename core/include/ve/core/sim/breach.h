#pragma once
// Phase 9 task 9.12 — section breach (#19c, two-tier breach law).
//
// A hit on a sectioned hull wall opens a SMALL breach: that section's air
// reservoir vents (the `atmo.venting` band tag is authored, opaque here)
// until doors close or a patch repair runs. This module adds ONLY the
// breach leak law + door-isolation walk — every other piece is shipped
// machinery: per-section air = Phase 6.9 Reservoirs (bands throw the
// venting/empty tags hysteresis-free), doors = 7.13 DoorField bits, the
// section graph = 7.11 authored nodes, and downstream consumers already
// read section pressure as section_state facts (7.11 junction rule) or
// through effect condition gates (#18d KO path = a bundle gated on the
// section's empty tag — expressible as data, zero new machinery).
//
// The vent law (deterministic, ordered):
//   1. each section's reservoir steps its pipeline flows FIRST (refill
//      attrs run under the shipped #19e integrator),
//   2. for each OPEN breach, BFS from the breached section along edges
//      whose door is open or absent (edges listed in registration order,
//      BFS expands neighbors in that order — ordered containers, rule 5),
//   3. every section in that reachable region loses `rate` per tick
//      through the set_value door (floor clamps at 0: a blown-out section
//      stays EXACTLY 0.0, never negative).
// Closing the doors at tick T isolates neighbors from tick T onward —
// the breached section itself keeps venting until `breach_patch`.
// Simplification (documented, deterministic): the breach leaks `rate`
// from EVERY section in the connected region rather than redistributing
// one total outflow; multiple breaches in one region each contribute
// their own rate. The whole-ship hull-breach threshold (9.14) is
// untouched here (#19c: pile-up of small breaches never destroys alone).
//
// Section identity = authored section id; sections live in unique_ptr so
// a Reservoir's TagSet pointer stays pinned (9.11 precedent). All
// tag/attr strings are caller-supplied cooked data (AGENTS rule 1: core
// never names "atmo" itself — the tests do).
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ve/core/sim/bands.h"
#include "ve/core/sim/doors.h"     // DoorField: edge open/closed facts
#include "ve/core/sim/reservoir.h"
#include "ve/core/sim/stats_pipeline.h"
#include "ve/core/sim/tags.h"

namespace ve::core::sim {

// One authored section's air: capacity + bands authored at configure;
// the reservoir starts FULL (authored pressure). `refill_attr` optional:
// a pipeline supply attr that pumps air back in after a patch (fail-open
// flow: empty string = no pump, like the shield regen door).
struct section_air_setup {
    std::string section_id;
    double capacity = 0.0;
    std::vector<reservoir_band> bands;
    std::string refill_attr;
};

// One graph edge between section INDICES (order as configured). A door
// component (mount index into DoorField) closes the edge to air when
// shut; nullopt = an open passage with no door (air always flows).
struct air_edge {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::optional<std::uint32_t> door_mount;
};

// One section's live air state. Member ORDER is load-bearing: the
// Reservoir self-refers the entry's OWN TagSet for its band tags, and the
// entry lives behind unique_ptr so that address never moves.
struct air_entry {
    std::string id;
    TagSet tags;
    Reservoir air;
    air_entry(std::string id_, double capacity,
              std::vector<reservoir_band> bands)
        : id(std::move(id_)), air(capacity, std::move(bands), &tags) {}
};

struct section_air {
    std::vector<std::unique_ptr<air_entry>> sections;  // config order
    std::vector<air_edge> edges;
    // One breach per section (a second hit on an already-breached
    // section re-opens nothing; add its own breach entry instead by
    // calling with a fresh... no: breaches are idempotent per section).
    struct breach_entry {
        std::uint32_t section = 0;   // index into `sections`
        double rate = 0.0;           // m3-ish air units LOST per tick
    };
    std::vector<breach_entry> breaches;  // breach-open order

    // Per-tick: step reservoirs (flows), then apply every breach's drain
    // to its reachable region (BFS over open edges, deterministic order).
    void step(const DoorField& doors);

    // Breach verbs. breach_open: unknown section id, or rate <= 0 /
    // non-finite, = refusal (false, zero state change) — fail-SAFE. An
    // already-breached section stays as-is (idempotent, false).
    // breach_patch: no breach on that section = false. Patching removes
    // the leak; the refill attr (if authored) recovers pressure through
    // the shipped integrator, bands un-throw above their thresholds.
    bool breach_open(std::string_view section_id, double rate);
    bool breach_patch(std::string_view section_id);

    [[nodiscard]] bool is_breached(std::string_view section_id) const;
    // Live reservoir handle (null = unknown section).
    [[nodiscard]] Reservoir* air(std::string_view section_id);
    // Live band tags thrown by a section's air reservoir.
    [[nodiscard]] TagSet* section_tags(std::string_view section_id);

    // ---- 9.14 whole-ship destruction (PLAN #19c dramatic tier; mechanism
    // ratified as user decision #56, 2026-11-19: ship-wide hull-integrity
    // Reservoir — the 9.11 single shield pool pattern) ----
    // Hull-wall hit overflow that lands on the STRUCTURE (not absorbed by
    // shield/component health) drains the pool through the set_value door;
    // the authored zero-crossing band throws on the hull's own TagSet.
    // Small-breach AIR venting never touches it, and component OFFLINE
    // pile-up can never drain it (separate domains; the pile-up test is
    // the guard). When the value first lands on 0.0 the hull is
    // DESTROYED — a terminal sim event: further hits refuse with zero
    // state change. Configure AFTER air_configure (it rebuilds the field).
    bool integrity_hit(double damage);  // refused (false, zero change) when
                                        // unconfigured, damage <= 0 / NaN /
                                        // non-finite, or already destroyed
    [[nodiscard]] bool is_destroyed() const { return destroyed; }
    Reservoir* integrity_value();       // null = dramatic tier unconfigured
    TagSet* hull_tags();                // null when unconfigured

    struct integrity_entry {  // unique_ptr: TagSet address fixed for Reservoir
        TagSet tags;
        Reservoir integrity;
        integrity_entry(double capacity, std::vector<reservoir_band> bands)
            : integrity(capacity, std::move(bands), &tags) {}
    };
    std::unique_ptr<integrity_entry> integrity;  // null = tier unconfigured
    bool destroyed = false;
};

// Build the field: configure order is the section INDEX order (deterministic
// identity everywhere else). Every setup must have a unique id, finite
// capacity > 0, and a finite refill-free initial = capacity. Returns false
// on any authoring error (no partial field left behind).
bool air_configure(section_air& field,
                   std::span<const section_air_setup> setups,
                   std::span<const air_edge> edges,
                   StatsPipeline& pipe);

// Dramatic-tier authoring (9.14): finite capacity > 0; bands validated by
// the shipped band machinery (nonempty tags). Reconfiguring while already
// configured = refused (false). The pool starts FULL (capacity) with the
// ceiling clamped at configure (9.11 trap: ctor sets VALUE only).
struct hull_integrity_setup {
    double capacity = 0.0;
    std::vector<reservoir_band> bands;  // e.g. {0.0, "hull.breached", at_or_below}
};
bool integrity_configure(section_air& field, const hull_integrity_setup& setup);

}  // namespace ve::core::sim

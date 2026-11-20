#pragma once
// Phase 9 task 9.16 — the contact map (PLAN #31b–d): sensor measurements as
// SHARED SIM STATE. The player scope and the Combat AI FSMs read the SAME
// contact_board records — no separate AI truth (#31b). Records are fixed-tick
// sim state: every measurement is a pure function of
// (world seed, tick, sensor id, target id) + positions, keyed exactly like
// the gun spread stream (9.10) and the KO roll (#18d), so the whole picture
// replays bit-identically with the trajectory it was drawn from.
//
// Laws (each pinned by tests contact_measurement_seeded / contact_class_bands
// / contact_ghosts_never_targetable):
//   MEASUREMENT (#31b): a sensor sees a target when slant <= range. The
//     authored angular resolution (arcminutes) becomes an ERROR CONE: the
//     measured direction is jittered inside a cone of half-angle
//     res_rad * assist_gain around the true line of sight (same cap-uniform
//     draw + smallest-axis frame as the 9.10 spread law), the measured
//     position sits at the true slant along that jittered direction, and the
//     uncertainty RADIUS = slant * cone — so uncertainty grows linearly with
//     distance (hand-computable). assist_gain in [0,1] is the targeting
//     computer's buy-down (#31b: "the targeting computer is an ordinary
//     assist controller that buys uncertainty down" — the controller itself
//     lands in 9.17; this layer just takes the live gain the caller feeds).
//     gain == 0 -> EXACT passthrough (measured == true position, unc 0).
//   FAIL-SAFE (#26b dark-not-dead rides free): the caller reads sensor attrs
//     LIVE (a dark sensor wire zeroes range through WiringReach) — here,
//     range <= 0 / non-finite = senses NOTHING; res non-finite = NO
//     measurement (a corrupt sensor yields no contact, never a fake clean
//     one); res <= 0 = perfect cone (missing-spread precedent, gun.h).
//   BANDS (#31c), evaluated at the winner sensor's measurement:
//     class silhouette resolves when hull_size >= slant * cone_eff
//     (hand-computable boundary slant* = hull_size / cone_eff); owner/IFF
//     resolves when slant <= iff_range; below both, an unknown ring (both
//     flags false). class_tag / owner token are opaque def data copied on
//     resolve — core never names a class (#AGENTS rule 1).
//   MULTI-SENSOR: one board per observer; when several sensors see the same
//     target this tick, the NEAREST wins (ties -> first in sensor
//     registration order; ordered containers, determinism rule 5).
//   GHOSTS (#31d): a record whose last measurement is older than `now` is a
//     GHOST — frozen last measurement + tick stamp, never updates by itself.
//     Ghosts are NEVER targetable: contact_targetable() is true only for a
//     record measured at exactly this tick (the sole gate 9.17/9.21 will
//     aim through). Records age out and are pruned when
//     now - seen > ghost_ticks (authored lifetime; 0 = feed is truth, a
//     missed tick drops the record immediately).
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ve/core/math/vec.h"
#include "ve/core/sim/time.h"

namespace ve::core::sim {

// One sensor's live state for this tick. id = the sensor component id
// (rng stream key). range/res_arcmin/iff_range are caller-resolved attrs
// (sensor.range / sensor.res_arcmin, #31b) read LIVE so any modifier —
// including dark-not-dead zeroing — rides with zero new machinery.
struct sensor_view {
    std::int32_t id = -1;
    math::dvec3 at{};
    double range = 0.0;       // m; <=0 or non-finite = senses nothing (fail-safe)
    double res_arcmin = 0.0;  // arcminutes; non-finite = NO measurement;
                              // <=0 = perfect (zero) cone
    double iff_range = 0.0;   // m; <=0 = owner/IFF band never opens
};

// One candidate entity as the world says it REALLY is (the board converts
// these into bounded-error records; the true state never leaks into a
// record). assist_gain scales this target's cone (1.0 = no assist lock).
struct contact_target {
    std::int64_t id = -1;
    math::dvec3 pos{};
    double hull_size = 0.0;   // m silhouette size (def data); <=0 never classes
    std::string class_tag;    // opaque def class.* tag (class_stanza precedent)
    std::uint64_t owner = 0;  // opaque owner token (presentation maps it to
                              // owner x stance x reputation colour, #25)
    double assist_gain = 1.0; // [0,1]; non-finite = 1.0; <0 = 0 (full buy-down)
};

// The ship's picture of one entity (#31b record shape).
struct contact_record {
    std::int64_t target = -1;
    math::dvec3 measured{};     // last measured position (bounded error)
    double uncertainty = 0.0;   // radius m at the measurement (slant * cone)
    tick_t seen = -1;           // tick of the last measurement
    std::int32_t seen_by = -1;  // winning sensor's component id
    bool class_resolved = false;
    std::string class_tag;      // non-empty only when class_resolved
    bool owner_resolved = false;
    std::uint64_t owner = 0;    // non-zero only when owner_resolved (token 0
                                // stays 0: "no owner to show")
};

// One observer's contact records, in first-seen registration order (ordered
// container, rule 5). ghost_ticks = authored ghost lifetime (#31d).
struct contact_board {
    std::int64_t ghost_ticks = 0;
    std::vector<contact_record> records;

    const contact_record* find(std::int64_t target) const {
        for (const auto& r : records)
            if (r.target == target) return &r;
        return nullptr;
    }
};

// One fixed-tick pass: prune aged ghosts, then feed the pass's live sensors
// (in registration order) over the targets. Every target in ANY sensor's
// range gets its record refreshed (nearest sensor wins); everything else
// just ages toward its ghost expiry. `now` must never go backwards.
void contact_measure_tick(contact_board& board,
                          std::span<const sensor_view> sensors,
                          std::span<const contact_target> targets,
                          tick_t now, std::uint64_t world_seed);

// The ONLY targetability gate (#31d): a contact is targetable exactly while
// it was measured at THIS tick. A ghost — or an absent record — is never
// targetable, never interactable.
bool contact_targetable(const contact_board& board, std::int64_t target,
                        tick_t now);

}  // namespace ve::core::sim

#pragma once
// Phase 5 task 5.1 — the loadout data model (decision #24a law: ONE build
// model at both scales; a station is just a big hull with sockets). The
// structural nouns (hull, socket, mount, attribute key) are engine
// vocabulary; EVERYTHING game-flavored is opaque data here: component
// type ids and attribute-key strings arrive from cooked defs, `core/`
// never interprets them (AGENTS rule 1). Category match is enforced as
// pure equality; what "gun" affords is game-side policy.
//
// Determinism (#5): all tables are insertion-ordered vectors; lookups are
// linear scans — hulls carry tens of sockets, so a hash map buys nothing
// and an unordered container is banned in sim logic anyway.
//
// Validation is assemble-time and total: a loadout that exists has no
// duplicate socket ids, no dangling socket refs, no double-occupied
// sockets, no category mismatches, no duplicate instance ids, no duplicate
// attribute keys or duplicate tags inside one mount, and a sane #44 grid
// block (tags are set-like capability lists; a doubled tag is an authoring
// bug, never a doubled capability). The cooklet
// (5.6-5.9) runs the same checks earlier on authored content; runtime
// refuses to build a broken loadout even from hand-assembled structs.

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ve/core/math/quat.h"
#include "ve/core/math/vec.h"

namespace ve::core::sim {

// Socket categories, decision #21: authored in hull defs / glb node extras;
// core knows only that categories must match, never what one affords.
enum class socket_category : std::uint8_t { main_, rcs_, gun_, utility_ };

// Interior lattice block (decision #44). Human units at def level; stored
// SI (cooked path coerces via #42 suffixes — 5.7). Structural validation
// here: positive sizes, integer subcell ratio, non-negative tolerance.
struct grid_block {
    double cell = 0.0;          // m, primary cell size
    int subcell_ratio = 1;      // >= 1, keeps lattices coincident at corners
    double cell_height = 0.0;   // m, true 3D cells
    double tolerance = 0.0;     // m, anchor snap limit (cook FAILS beyond)
};

// One hardpoint from the hull unit (5.6 cooks these from hp_* empties,
// decision #14). orientation_local is the MOUNT frame: forces/axes are
// expressed in it; which axis "thrust along" means is game-side policy.
struct socket_desc {
    std::string id;             // hp_* node name, unique in the hull
    socket_category category = socket_category::utility_;
    math::dvec3 position_local{};
    math::dquat orientation_local{};  // identity if the art authored none
};

// A mountable component TYPE: type_id is opaque (e.g. "thruster.main" from
// cooked game data); its category gates which sockets accept it.
struct component_type {
    std::string type_id;
    socket_category category = socket_category::utility_;
};

// Attribute key -> base value, SI (#42 suffixes already applied upstream).
// Values are double, not fp32: mount numbers feed fp64 inertia/thrust math
// (5.2/5.5); stats display may narrow to fp32 later (Phase 6).
struct attr_pair {
    std::string key;
    double value = 0.0;
};

// One intended mount (from cooked def rows, hangar actions, or tests).
// tags are opaque gameplay tags (#28a facet actions, #18 role tags) — the
// cooklet already rejects unknown facet-action tags; core only stores them.
struct mount_request {
    std::string instance_id;    // unique across the loadout
    std::string type_id;        // must name a catalog entry
    std::string socket_id;      // must name a hull socket
    std::vector<attr_pair> attrs;  // base values; insertion order kept
    std::vector<std::string> tags; // capability/role tags; insertion order kept
};

// A validated, mounted component.
struct mounted_component {
    std::string instance_id;
    std::string type_id;
    std::string socket_id;
    socket_category category = socket_category::utility_;
    std::vector<attr_pair> attrs;
    std::vector<std::string> tags;
};

// Everything that failed to assemble (single value: the FIRST violation in
// check order — deterministic, and the cooklet reports per-entry anyway).
enum class loadout_error : std::uint8_t {
    duplicate_socket_id,
    unknown_component_type,
    unknown_socket,
    socket_occupied,
    category_mismatch,
    duplicate_instance_id,
    duplicate_attr_key,
    duplicate_tag,
    invalid_grid,
    invalid_hull_mass,
};

// Hull-level info: identity + the hull's OWN structural mass contribution
// (5.2 derives ship mass/CoM/tensor from this plus the mounts). The hull
// acts as a point mass at hull_com_local unless inertia_diag carries a real
// authored tensor diagonal (component mounts add via parallel axis).
struct hull_info {
    std::uint64_t mesh_content_id = 0;  // MESH unit in the manifest
    std::string class_tag;              // opaque ("class.shuttle"); core never parses
    grid_block grid{};
    double hull_mass = 0.0;             // kg, authored (#14 root extras)
    math::dvec3 hull_com_local{};
    math::dvec3 hull_inertia_diag{0.0, 0.0, 0.0};  // kg*m^2 about hull_com_local
};

// The assembled, validated loadout. Immutable after assemble() (mount/
// unmount mid-play builds a NEW loadout — cheap copies at this size, and
// it keeps the sim state hash story simple: loadout identity, not deltas).
class Loadout {
public:
    // Assemble + validate. sockets/catalog/mounts may be any ordered range.
    static std::expected<Loadout, loadout_error> assemble(
        hull_info hull,
        std::vector<socket_desc> sockets,
        std::vector<component_type> catalog,
        std::vector<mount_request> mounts);

    const hull_info& hull() const { return m_hull; }
    const std::vector<socket_desc>& sockets() const { return m_sockets; }
    // Mounts in request order (deterministic iteration, #5).
    const std::vector<mounted_component>& mounts() const { return m_mounts; }

    // -1 when absent; linear scan on purpose (tens of entries).
    int socket_index(std::string_view id) const;
    const mounted_component* mount(std::string_view instance_id) const;
    const mounted_component* mount_at_socket(std::string_view socket_id) const;
    std::size_t free_socket_count() const { return m_sockets.size() - m_mounts.size(); }

    std::optional<double> attr(const mounted_component& mc, std::string_view key) const;

private:
    hull_info m_hull;
    std::vector<socket_desc> m_sockets;
    std::vector<mounted_component> m_mounts;
};

}  // namespace ve::core::sim

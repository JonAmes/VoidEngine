#pragma once
// Phase 5 task 5.9b — hull cross-validation gates. The cooklet runs these
// after DEF0 + SOCK units exist (PLAN #44 gate list, #46 conventions):
//   * component socket pairing + #46a named-empty 1:1 pairing,
//   * #46b lattice snap (rewrite INTO the SOCK payload; beyond-tolerance =
//     cook fail),
//   * #44 every anchor in exactly one grid section,
//   * #46c/#46c1 door/airlock facing rules (explicit face axis, strict),
//   * #26a wire endpoint existence, #28a facet-target existence,
//   * #46d seam rules (partner cooked, socket both sides, exterior cell,
//     lattice compat, symmetric back-declaration).
// Gates run in fixed order over payload order — identical inputs produce
// identical first-error text (cook determinism, like the manifest graph).
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ve/core/cooked.h"  // Bytes, Unit

namespace ve::cook {

using ve::core::cooked::Bytes;  // cook.h does the full using-directive

// ---- decoded payload views (byte-faithful to cooked.h layouts) ----------
struct SockAnchor {
    std::string name;
    std::uint8_t kind = 0;      // 0 hp_*, 1 door_, 2 seat_, 3 panel_, 4 counter_, 5 berth_
    std::uint8_t category = 0;  // core socket_category encoding
    std::array<double, 3> t{{0, 0, 0}};
    std::array<double, 4> q{{0, 0, 0, 1}};
};
struct SockView {
    std::string mesh_id;
    std::vector<SockAnchor> anchors;
};

struct DefView {
    std::string class_tag;
    double cell_m = 0.0;
    std::uint32_t subcell_ratio = 0;
    double cell_height_m = 0.0;
    double tolerance_m = 0.0;
    struct Section { std::string name; std::vector<std::array<std::int32_t, 6>> boxes; };
    std::vector<Section> sections;
    struct Facet { std::string action, target; std::uint8_t has_target = 0; };
    struct Comp {
        std::string id, type, socket;
        std::uint8_t category = 0;
        std::uint8_t face = 0;  // 0 none, 1 +x, 2 -x, 3 +y, 4 -y, 5 +z, 6 -z (#46c1)
        std::vector<std::pair<std::string, double>> attrs;  // #47b: G10 reads dock attrs
        std::vector<Facet> facets;
    };
    std::vector<Comp> comps;
    struct Wire {
        std::string id;
        std::uint8_t type = 0;
        std::uint8_t from_kind = 0, to_kind = 0;  // 0 socket, 1 component
        std::string from_ref, to_ref;
    };
    std::vector<Wire> wires;
    struct Seam { std::string partner_ref, socket; };
    std::vector<Seam> seams;
};

bool decode_def_payload(const Bytes& payload, DefView& out, std::string* err);
bool decode_sock_payload(const Bytes& payload, SockView& out, std::string* err);
// Canonical SOCK v2 bytes (byte-identical to the .glb cook emission) so
// #46b snapped coords can be rewritten into the unit file.
Bytes encode_sock_payload(const SockView& in);

// One hull's validation inputs. seam_partner_ids is parallel to def.seams:
// caller-resolved source content IDs of each authored partner ref
// ("" = unresolved/missing -> gate fails).
struct HullInputs {
    std::string def_id;
    DefView def;
    std::vector<SockView> socks;  // one per mesh dep
    std::vector<std::string> seam_partner_ids;
};

// All gates; on success snapped_out (parallel to hull.socks) carries the
// #46b lattice-snapped anchor coords — callers compare bytes to decide
// whether to rewrite SOCK unit files. partners maps partner def source id
// -> its decoded inputs (needed for #46d seam checks).
bool cook_hull_validate(const HullInputs& hull,
                        const std::map<std::string, HullInputs>& partners,
                        std::vector<SockView>& snapped_out,
                        std::string* err);

} // namespace ve::cook

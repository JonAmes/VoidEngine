// Phase 5 task 5.9b — hull cross-validation gates (cook_hull_validate).
// Payload-level: Hand-built DefView/SockView inputs pin each gate's
// first-error exactly once, plus a full-pass hull (door + airlock + seam +
// lattice snap rewrite) and SOCK encode/decode byte stability. The e2e
// fixture pass (real assets -> cook -> validate) rides cook_incremental /
// cook_hull_def's manifest checks.
#include <array>
#include <map>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/cook/hullvalidate.h"

namespace {

using namespace ve::cook;

DefView mk_def() {
    DefView d;
    d.class_tag = "class.test";
    d.cell_m = 2.0;
    d.subcell_ratio = 4;
    d.cell_height_m = 2.2;
    d.tolerance_m = 0.1;
    d.sections.push_back({"A", {std::array<std::int32_t, 6>{-1, 0, -1, 1, 1, -1}}});
    d.sections.push_back({"B", {std::array<std::int32_t, 6>{-1, -1, 0, 1, 1, 1}}});
    return d;
}

SockAnchor mk_anchor(const std::string& name, int kind, double x, double y, double z) {
    SockAnchor a;
    a.name = name;
    a.kind = static_cast<std::uint8_t>(kind);
    a.category = 3;
    a.t = {x, y, z};
    return a;
}

DefView::Comp mk_comp(const std::string& id, const std::string& type, const std::string& socket,
                      int face = 0) {
    DefView::Comp c;
    c.id = id;
    c.type = type;
    c.socket = socket;
    c.category = 3;
    c.face = static_cast<std::uint8_t>(face);
    return c;
}

// Full-pass hull: A is the lower cell slab (z=-1), B the upper (z=0).
// hp_engine (cell A), door joins A->B (face +z), airlock faces space (+x),
// seat_1 on the subcell lattice. Wire + repair.target facet cross-ref real.
HullInputs mk_hull() {
    HullInputs h;
    h.def_id = "defs/a.def.toml";
    h.def = mk_def();
    SockView s;
    s.mesh_id = "m.glb";
    s.anchors = {mk_anchor("hp_engine", 0, 2.0, 0.0, -2.0),       // cell (1,0,-1) in A
                 mk_anchor("door_1", 1, 0.0, 0.0, -2.0),          // cell (0,0,-1) in A
                 mk_anchor("airlock_1", 1, 2.0, 0.0, -2.0),       // cell (1,0,-1) in A
                 mk_anchor("seat_1", 2, 0.5, 0.55, -2.0)};        // subcell lattice
    h.socks = {s};
    h.def.comps = {
        mk_comp("engine", "engine.fusion", "hp_engine"),
        mk_comp("door_1", "door.hatch", "door_1", 5),             // +z -> cell B
        mk_comp("airlock_1", "airlock.outer", "airlock_1", 1),    // +x -> no section
        mk_comp("seat_1", "seat.pilot", "seat_1"),
    };
    h.def.wires.push_back({"w1", 0, 1, 0, "engine", "hp_engine"});  // comp -> hardpoint
    h.def.comps[0].facets.push_back({"repair.target", "w1", 1});
    return h;
}

void run_pass(const HullInputs& h, const std::map<std::string, HullInputs>& partners = {}) {
    std::vector<SockView> snapped;
    std::string err;
    REQUIRE_MESSAGE(cook_hull_validate(h, partners, snapped, &err), err);
}

std::string run_fail(const HullInputs& h, const std::map<std::string, HullInputs>& partners = {}) {
    std::vector<SockView> snapped;
    std::string err;
    CHECK_FALSE(cook_hull_validate(h, partners, snapped, &err));
    return err;
}

} // namespace

TEST_CASE("cook_hull_validate") {
    const HullInputs h = mk_hull();
    run_pass(h);

    // #46b: on-lattice fixture -> snap is a byte-exact no-op.
    std::vector<SockView> snapped;
    std::string err;
    REQUIRE(cook_hull_validate(h, {}, snapped, &err));
    REQUIRE(snapped.size() == h.socks.size());
    CHECK(encode_sock_payload(snapped[0]) == encode_sock_payload(h.socks[0]));

    // #46b: within-tolerance drift snaps AND rewrites the payload coords.
    HullInputs hh = mk_hull();
    hh.socks[0].anchors[1].t[0] = 0.05;                 // door within tol 0.1
    hh.socks[0].anchors[3].t[0] = 0.54;                 // seat on subcell lattice
    std::vector<SockView> sn2;
    REQUIRE_MESSAGE(cook_hull_validate(hh, {}, sn2, &err), err);
    CHECK(sn2[0].anchors[1].t[0] == 0.0);
    CHECK(sn2[0].anchors[3].t[0] == 0.5);

    // decode(encode(view)) round-trips byte-stably (rewrite path safety).
    const Bytes b = encode_sock_payload(sn2[0]);
    SockView back;
    REQUIRE(decode_sock_payload(b, back, &err));
    CHECK(encode_sock_payload(back) == b);
    CHECK(back.anchors.size() == 4);
    CHECK(back.anchors[2].name == "airlock_1");
}

TEST_CASE("cook_hull_validate_seams") {
    // Seaming hull A at its extreme cell (1,0,-1) to partner P sharing socket
    // "hp_dock"; both declare the join; grids compatible (#46d pass path).
    HullInputs h = mk_hull();
    h.socks[0].anchors.push_back(mk_anchor("hp_dock", 0, 2.0, 0.0, -2.0));  // cell A, exterior +x
    h.def.comps.push_back(mk_comp("port_1", "port.dock", "hp_dock"));
    h.def.seams.push_back({"../p.def.toml", "hp_dock"});
    h.seam_partner_ids = {"defs/p.def.toml"};

    HullInputs p = mk_hull();
    p.def_id = "defs/p.def.toml";
    p.socks[0].anchors.push_back(mk_anchor("hp_dock", 0, -2.0, 0.0, -2.0));  // cell (-1,0,-1)
    p.def.comps.push_back(mk_comp("port_2", "port.dock", "hp_dock"));
    p.def.seams.push_back({"../a.def.toml", "hp_dock"});
    p.seam_partner_ids = {"defs/a.def.toml"};

    std::map<std::string, HullInputs> partners{{p.def_id, p}};
    run_pass(h, partners);

    // Grid incompatible: partner cell 3.0 vs 2.0 -> not integer-multiple.
    HullInputs p2 = p;
    p2.def.cell_m = 3.0;
    CHECK(run_fail(h, {{p2.def_id, p2}}).find("grid incompatible") != std::string::npos);

    // Partner declares no seam back.
    HullInputs p3 = p;
    p3.def.seams.clear();
    p3.seam_partner_ids.clear();
    CHECK(run_fail(h, {{p3.def_id, p3}}).find("no seam back") != std::string::npos);

    // Partner lacks the shared hardpoint anchor.
    HullInputs p4 = p;
    for (auto& a : p4.socks[0].anchors)
        if (a.name == "hp_dock") a.name = "hp_other";
    CHECK(run_fail(h, {{p4.def_id, p4}}).find("not found in partner") != std::string::npos);

    // Unresolved partner ref (caller could not resolve it).
    HullInputs h2 = h;
    h2.seam_partner_ids = {""};
    CHECK(run_fail(h2, partners).find("partner def not cooked") != std::string::npos);

    // Seam socket must be a HARDPOINT anchor here.
    HullInputs h3 = h;
    h3.def.seams[0].socket = "door_1";
    h3.seam_partner_ids = {"defs/p.def.toml"};
    CHECK(run_fail(h3, partners).find("not a hardpoint anchor here") != std::string::npos);

    // Seam socket cell must be on the hull EXTREME (enclosed by sections).
    HullInputs h4 = mk_hull();
    h4.socks[0].anchors.push_back(mk_anchor("hp_dock", 0, 0.0, 0.0, 0.0));  // cell (0,0,0): all 6 neighbors exist
    h4.def.comps.push_back(mk_comp("port_1", "port.dock", "hp_dock"));
    h4.def.seams.push_back({"../p.def.toml", "hp_dock"});
    h4.seam_partner_ids = {"defs/p.def.toml"};
    CHECK(run_fail(h4, partners).find("enclosed by sections") != std::string::npos);
}

TEST_CASE("cook_hull_validate_gates") {
    std::string err;

    // G1 (#46c1): door/airlock need an explicit face.
    {
        HullInputs h = mk_hull();
        h.def.comps[1].face = 0;
        CHECK(run_fail(h).find("missing required 'face'") != std::string::npos);
    }
    // G2: component socket must name a real anchor.
    {
        HullInputs h = mk_hull();
        h.def.comps[0].socket = "hp_nonexistent";
        CHECK(run_fail(h).find("names no anchor") != std::string::npos);
    }
    // G3a (#46a): interior mount pairs BY id.
    {
        HullInputs h = mk_hull();
        h.def.comps[3].id = "seat_other";
        // socket==seat_1 (kind 2) but id differs; anchor pairing then fails too.
        CHECK(run_fail(h).find("pairing ids differ") != std::string::npos);
    }
    // G3b (#46a): unpaired interior anchor (art empty with no def component).
    {
        HullInputs h = mk_hull();
        h.socks[0].anchors.push_back(mk_anchor("panel_stray", 3, 0.5, 0.0, -2.0));
        CHECK(run_fail(h).find("need exactly 1") != std::string::npos);
    }
    // G4 (#46b): primary anchor beyond tolerance off the cell lattice.
    {
        HullInputs h = mk_hull();
        h.socks[0].anchors[1].t[0] = 0.3;  // tol 0.1
        CHECK(run_fail(h).find("off the cell lattice") != std::string::npos);
    }
    // G4 (#46b): furnishings ride the SUBCELL lattice.
    {
        HullInputs h = mk_hull();
        h.socks[0].anchors[3].t[0] = 0.75;  // nearest subcell 0.5/1.0 both > tol
        CHECK(run_fail(h).find("off the subcell lattice") != std::string::npos);
    }
    // G5 (#44): anchor in no section.
    {
        HullInputs h = mk_hull();
        h.socks[0].anchors[0].t[0] = 10.0;  // cell (5,0,-1): outside all boxes
        CHECK(run_fail(h).find("in 0 sections") != std::string::npos);
    }
    // G5 (#44): anchor in two sections (overlapping boxes).
    {
        HullInputs h = mk_hull();
        h.def.sections[1].boxes[0] = {-1, 0, -2, 1, 1, 1};  // B now swallows A
        CHECK(run_fail(h).find("in 2 sections") != std::string::npos);
    }
    // G6 (#46c): door facing its own section's cell.
    {
        HullInputs h = mk_hull();
        h.def.comps[1].face = 2;  // -x: neighbor still inside A
        CHECK(run_fail(h).find("its own section") != std::string::npos);
    }
    // G6 (#46c strict): door into space must be airlock.*.
    {
        HullInputs h = mk_hull();
        h.def.comps[1].face = 1;  // +x from cell (0,0,-1) -> (1,0,-1) which IS A... 
        // (1,0,-1) is in A -> own section, not space. Use -y for space instead:
        h.def.comps[1].face = 4;  // -y: neighbor (0,-1,-1) in no section
        CHECK(run_fail(h).find("declare it airlock") != std::string::npos);
    }
    // G6 (#46c): airlock must NOT face an existing section.
    {
        HullInputs h = mk_hull();
        h.def.comps[2].face = 6;  // -z from (1,0,-1) -> (1,0,-2): none... 
        h.def.comps[2].face = 5;  // +z -> (1,0,0) is B -> fail
        CHECK(run_fail(h).find("use door") != std::string::npos);
    }
    // G7 (#26a): wire endpoints must exist.
    {
        HullInputs h = mk_hull();
        h.def.wires[0].from_ref = "ghost_comp";
        CHECK(run_fail(h).find("names no component") != std::string::npos);
    }
    {
        HullInputs h = mk_hull();
        h.def.wires[0].to_ref = "ghost_socket";
        CHECK(run_fail(h).find("names no hardpoint") != std::string::npos);
    }
    // G8 (#28a): facet target names a real wire.
    {
        HullInputs h = mk_hull();
        h.def.comps[0].facets[0].target = "w_ghost";
        CHECK(run_fail(h).find("names no wire") != std::string::npos);
    }
    // Decoders reject wrong payload versions / truncation (module hygiene).
    {
        DefView d;
        Bytes b;
        b.push_back(1); b.push_back(0); b.push_back(0); b.push_back(0);  // ver 1
        CHECK_FALSE(decode_def_payload(b, d, &err));
        CHECK(err.find("version") != std::string::npos);
        SockView s;
        Bytes sb;  // empty -> truncated
        CHECK_FALSE(decode_sock_payload(sb, s, &err));
    }
    // Gate ORDER is fixed: missing face fails before a dangling socket in the
    // same hull (cook determinism: identical inputs -> identical first error).
    {
        HullInputs h = mk_hull();
        h.def.comps[1].face = 0;
        h.def.comps[0].socket = "hp_ghost";
        err = run_fail(h);
        CHECK(err.find("missing required 'face'") != std::string::npos);
        CHECK(err.find("names no anchor") == std::string::npos);
    }
}

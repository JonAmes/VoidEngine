// Phase 5 task 5.10 — runtime load path (loadout_load). Hand-encoded
// DEF0-v2 + SOCK-v2 payloads (byte layout pinned in ve/core/cooked.h) go
// through load_hull_loadout; the assembled Loadout must equal the cooked
// tables value-for-value (the "loaded tables byte-equal cooked payload"
// bullet), and corrupt/massaged units must be refused with the first
// violation named deterministically.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ve/core/cooked.h"
#include "ve/core/sim/loadout_load.h"

namespace {

using ve::core::cooked::Bytes;
using ve::core::cooked::Unit;
using namespace ve::core::sim;

// ---- local little-endian encoder (cooklet writers are private) ----
void pu8(Bytes& b, std::uint8_t v) { b.push_back(v); }
void pu32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void pi32(Bytes& b, std::int32_t v) { pu32(b, static_cast<std::uint32_t>(v)); }
void pf64(Bytes& b, double d) {
    std::uint64_t v = 0;
    static_assert(sizeof d == 8);
    std::memcpy(&v, &d, 8);
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void pstr(Bytes& b, const std::string& s) {
    pu32(b, static_cast<std::uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

// SOCK v2: str mesh_unit_id, u32 ver, u32 count,
// { str name, u8 kind, u8 cat, 3*f64 t, 4*f64 q }
Bytes sock_payload() {
    Bytes b;
    pstr(b, "test_ship.glb");
    pu32(b, 2);
    pu32(b, 3);
    pstr(b, "hp_engine"); pu8(b, 0); pu8(b, 0);
    pf64(b, 2.0); pf64(b, 0.0); pf64(b, -2.0);
    pf64(b, 0); pf64(b, 0); pf64(b, 0); pf64(b, 1);
    pstr(b, "hp_seat"); pu8(b, 0); pu8(b, 3);
    pf64(b, 0.0); pf64(b, 0.5); pf64(b, 0.0);
    pf64(b, 0); pf64(b, 0); pf64(b, 0); pf64(b, 1);
    pstr(b, "door_1"); pu8(b, 1); pu8(b, 3);  // interior anchor (kind 1)
    pf64(b, 0.0); pf64(b, 0.0); pf64(b, 2.0);
    pf64(b, 0); pf64(b, 0.7071067811865476); pf64(b, 0); pf64(b, 0.7071067811865476);
    return b;
}

// DEF0 v2 emit order copied from tools/cook/src/hulldef.cpp (5.9b decode
// in hullvalidate.cpp is the sibling reader; both must agree with cooked.h).
Bytes def_payload() {
    Bytes b;
    pu32(b, 2);                          // payload_ver
    pstr(b, "class.light_freighter");
    pu8(b, 1); pf64(b, 1000.0);          // has_mass + kg
    pu8(b, 1); pf64(b, 0.0); pf64(b, 0.5); pf64(b, -0.5);   // com
    pu8(b, 1); pf64(b, 4200.0); pf64(b, 9800.0); pf64(b, 11200.0);  // inertia
    pf64(b, 2.0); pu32(b, 4); pf64(b, 2.2); pf64(b, 0.1);   // grid
    pu32(b, 1);                          // sections: A = [-1,-1,-1 .. 1,1,1]
    pstr(b, "A"); pu32(b, 1);
    for (int v : {-1, -1, -1, 1, 1, 1}) pi32(b, v);
    pu32(b, 2);                          // components
    pstr(b, "eng_1"); pstr(b, "thruster.main"); pu8(b, 0);
    pstr(b, "hp_engine"); pu8(b, 0);     // socket, face (none)
    pu32(b, 0);                          // tags
    pu32(b, 1); pstr(b, "thrust.max_N"); pf64(b, 5000.0);   // attrs
    pu32(b, 1);                          // facet: operate.station
    pstr(b, "operate.station"); pf64(b, 2.0); pu8(b, 0); pu8(b, 0);
    pstr(b, "door_1"); pstr(b, "door.hatch"); pu8(b, 3);
    pstr(b, "door_1"); pu8(b, 5);        // socket, face +z (#46c1)
    pu32(b, 2);                          // tags (#18 role/facet, def order)
    pstr(b, "use.door"); pstr(b, "station.engineer");
    pu32(b, 0);                          // attrs
    pu32(b, 1);                          // facet: door.toggle
    pstr(b, "door.toggle"); pf64(b, 2.0); pu8(b, 1); pu8(b, 0);
    pu32(b, 2);                          // catalog
    pstr(b, "thruster.main"); pu8(b, 0);
    pstr(b, "door.hatch"); pu8(b, 3);
    pu32(b, 1);                          // wires: w1 power, eng_1 -> hp_engine
    pstr(b, "w1"); pu8(b, 0); pu8(b, 1); pf64(b, 100.0);
    pu8(b, 1); pstr(b, "eng_1"); pu8(b, 0); pstr(b, "hp_engine");
    pu32(b, 0);                          // seams
    pu32(b, 1); pstr(b, "../samples/test_ship.glb");  // refs
    return b;
}

// vector-based substring search (Bytes has no find/replace).
std::size_t find_sub(const Bytes& hay, const std::string& needle, std::size_t from = 0) {
    for (std::size_t i = from; i + needle.size() <= hay.size(); ++i) {
        if (std::equal(needle.begin(), needle.end(), hay.begin() + std::ptrdiff_t(i))) return i;
    }
    return SIZE_MAX;  // not-found sentinel
}
bool bad_at(const Bytes&, std::size_t pos) { return pos == SIZE_MAX; }

// u32(len) + text as a search pattern. NOTE: must be range-constructed —
// a std::string from a "\\xNN..." literal stops at the first embedded NUL.
std::string mklen(const std::string& text) {
    Bytes b;
    pu32(b, static_cast<std::uint32_t>(text.size()));
    b.insert(b.end(), text.begin(), text.end());
    return std::string(b.begin(), b.end());
}

Unit mk_unit(std::uint32_t type, Bytes payload) {
    Unit u;
    u.id = "defs/test_ship.def.toml";
    u.type = type;
    u.payload = std::move(payload);
    return u;
}

}  // namespace

TEST_CASE("loadout_load") {
    auto r = load_hull_loadout(mk_unit(ve::core::cooked::kUnitDef0, def_payload()),
                               mk_unit(ve::core::cooked::kUnitSock, sock_payload()),
                               0xDEADBEEFCAFEULL);
    REQUIRE_MESSAGE(r.has_value(), r.error());
    const Loadout& lo = r->loadout;

    CHECK(r->mesh_unit_id == "test_ship.glb");
    CHECK(lo.hull().mesh_content_id == 0xDEADBEEFCAFEULL);
    CHECK(lo.hull().class_tag == "class.light_freighter");
    CHECK(lo.hull().hull_mass == 1000.0);
    CHECK(lo.hull().hull_com_local.y == 0.5);
    CHECK(lo.hull().hull_com_local.z == -0.5);
    CHECK(lo.hull().hull_inertia_diag.x == 4200.0);
    CHECK(lo.hull().hull_inertia_diag.z == 11200.0);
    CHECK(lo.hull().grid.cell == 2.0);
    CHECK(lo.hull().grid.subcell_ratio == 4);
    CHECK(lo.hull().grid.cell_height == 2.2);
    CHECK(lo.hull().grid.tolerance == 0.1);

    // Every named anchor is a socket (hardpoints AND interior), in payload
    // order; loaded values byte-equal the cooked tables.
    REQUIRE(lo.sockets().size() == 3);
    CHECK(lo.sockets()[0].id == "hp_engine");
    CHECK(lo.sockets()[0].category == socket_category::main_);
    CHECK(lo.sockets()[0].position_local.x == 2.0);
    CHECK(lo.sockets()[0].position_local.z == -2.0);
    CHECK(lo.sockets()[1].category == socket_category::utility_);
    CHECK(lo.sockets()[2].id == "door_1");
    CHECK(lo.sockets()[2].position_local.z == 2.0);
    CHECK(lo.sockets()[2].orientation_local.y == 0.7071067811865476);  // exact: same
    CHECK(lo.sockets()[2].orientation_local.w == 0.7071067811865476);  // constant both sides

    // Components mount uniformly onto anchors (#46a pairing is total).
    REQUIRE(lo.mounts().size() == 2);
    CHECK(lo.mounts()[0].instance_id == "eng_1");
    CHECK(lo.mounts()[0].type_id == "thruster.main");
    CHECK(lo.mounts()[0].socket_id == "hp_engine");
    REQUIRE(lo.mounts()[0].attrs.size() == 1);
    CHECK(lo.mounts()[0].attrs[0].key == "thrust.max_N");
    CHECK(lo.mounts()[0].attrs[0].value == 5000.0);
    CHECK(lo.mounts()[1].instance_id == "door_1");
    CHECK(lo.mounts()[1].socket_id == "door_1");
    // #18: tags ride the mount in def order; loader keeps them verbatim and
    // never interprets them (loadout_seats pins the duplicate_tag gate).
    CHECK(lo.mounts()[0].tags.empty());
    REQUIRE(lo.mounts()[1].tags.size() == 2);
    CHECK(lo.mounts()[1].tags[0] == "use.door");
    CHECK(lo.mounts()[1].tags[1] == "station.engineer");
    CHECK(lo.attr(*lo.mount("door_1"), "nope") == std::nullopt);
    CHECK(lo.mount_at_socket("hp_engine")->instance_id == "eng_1");
    CHECK(lo.free_socket_count() == 1);
}

TEST_CASE("loadout_load_gates") {
    const Bytes def = def_payload(), sock = sock_payload();
    auto err_with = [&](Unit d, Unit s) -> std::string {
        auto r = load_hull_loadout(d, s, 1);
        REQUIRE_FALSE(r.has_value());
        return r.error();
    };
    // Unit type checks.
    CHECK(err_with(mk_unit(ve::core::cooked::kUnitMesh, def),
                   mk_unit(ve::core::cooked::kUnitSock, sock)).find("not type DEF0") !=
          std::string::npos);
    CHECK(err_with(mk_unit(ve::core::cooked::kUnitDef0, def),
                   mk_unit(ve::core::cooked::kUnitMesh, sock)).find("not type SOCK") !=
          std::string::npos);
    // Payload version pins.
    {
        Bytes d = def;
        d[0] = 1;
        CHECK(err_with(mk_unit(ve::core::cooked::kUnitDef0, d),
                       mk_unit(ve::core::cooked::kUnitSock, sock)).find(
                  "DEF0 payload: version must be 2") != std::string::npos);
    }
    {
        Bytes s = sock;
        s[17] = 3;  // payload_ver right after the mesh_unit_id string
        CHECK(err_with(mk_unit(ve::core::cooked::kUnitDef0, def),
                       mk_unit(ve::core::cooked::kUnitSock, s)).find(
                  "SOCK payload: version must be 2") != std::string::npos);
    }
    // Truncation and trailing junk are both refused.
    {
        Bytes d = def;
        d.resize(d.size() - 4);
        CHECK(err_with(mk_unit(ve::core::cooked::kUnitDef0, d),
                       mk_unit(ve::core::cooked::kUnitSock, sock)).find(
                  "truncated or corrupt") != std::string::npos);
    }
    {
        Bytes s = sock;
        s.insert(s.end(), 8, 0xAA);
        CHECK(err_with(mk_unit(ve::core::cooked::kUnitDef0, def),
                       mk_unit(ve::core::cooked::kUnitSock, s)).find(
                  "trailing bytes") != std::string::npos);
    }
    // Out-of-range category byte (cat_or_bad latches reader.bad).
    {
        Bytes s = sock;
        s[17 + 4 + 4 + 13 + 1] = 7;  // ver(17)+count(21)+entry name(25..38): cat @ 39
        CHECK(err_with(mk_unit(ve::core::cooked::kUnitDef0, def),
                       mk_unit(ve::core::cooked::kUnitSock, s)).find(
                  "SOCK payload: truncated or corrupt") != std::string::npos);
    }
    // Assemble is the runtime second line of defense: a massaged def that
    // names a socket that does not exist must still be refused.
    {
        // "hp_engine" string (id 6 + 9 chars) in the component row: swap the
        // socket ref to a name no anchor carries.
        Bytes d = def;
        const std::string old_s = "hp_engine";
        // component row's socket ref: the SECOND occurrence of the id-9
        // string (catalog-free payload: first in socket field, also facet
        // none) — locate explicitly.
        const std::string pat = mklen("hp_engine");
        const auto cpos = find_sub(d, mklen("eng_1"));  // comp 0 id
        REQUIRE_FALSE(bad_at(d, cpos));
        const auto pos = find_sub(d, pat, cpos);  // component's socket ref
        REQUIRE_FALSE(bad_at(d, pos));
        d[pos + 5] = 'x';  // -> "xp_engine": dangling socket ref
        CHECK(err_with(mk_unit(ve::core::cooked::kUnitDef0, d),
                       mk_unit(ve::core::cooked::kUnitSock, sock)).find(
                  "loadout: unknown_socket") != std::string::npos);
        (void)old_s;
    }
    // Category mismatch through a massaged payload (eng_1 onto the utility
    // seat socket). Fresh payload each time: byte surgery on a shared copy
    // after erase/insert invalidates earlier offsets.
    {
        Bytes d = def_payload();
        const auto cpos = find_sub(d, mklen("eng_1"));          // comp 0 id
        REQUIRE_FALSE(bad_at(d, cpos));
        const auto old_ref = mklen("hp_engine");
        const auto spos = find_sub(d, old_ref, cpos);           // socket ref
        REQUIRE_FALSE(bad_at(d, spos));
        const auto rep = mklen("hp_seat");                      // 11 vs 13: real
        d.erase(d.begin() + std::ptrdiff_t(spos),               // splice needed
                d.begin() + std::ptrdiff_t(spos + old_ref.size()));
        d.insert(d.begin() + std::ptrdiff_t(spos), rep.begin(), rep.end());
        CHECK(err_with(mk_unit(ve::core::cooked::kUnitDef0, d),
                       mk_unit(ve::core::cooked::kUnitSock, sock)).find(
                  "loadout: category_mismatch") != std::string::npos);
    }
    // Negative hull mass -> assemble's invalid_hull_mass gate (mass >= 0 is
    // deliberately legal: a hull may declare dry mass 0).
    {
        Bytes d = def_payload();
        const std::size_t flag = 4 + 4 + 21;  // ver + str len + 21-char class_tag
        REQUIRE(d[flag] == 1);                // has_mass flag
        const double bad_mass = -1.0;
        std::uint64_t bits = 0;
        std::memcpy(&bits, &bad_mass, 8);
        for (int i = 0; i < 8; ++i) d[flag + 1 + i] = static_cast<std::uint8_t>(bits >> (8 * i));
        CHECK(err_with(mk_unit(ve::core::cooked::kUnitDef0, d),
                       mk_unit(ve::core::cooked::kUnitSock, sock)).find(
                  "loadout: invalid_hull_mass") != std::string::npos);
    }
}

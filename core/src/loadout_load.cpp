#include "ve/core/sim/loadout_load.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ve::core::sim {
namespace {

using ve::core::cooked::Bytes;
using ve::core::cooked::Unit;

// Bounds-checked little-endian reader over a cooked payload (same doctrine
// as the cooklet validator): any short read latches `bad`, and the first
// violation is what the caller sees — deterministic error text.
struct Reader {
    const Bytes& b;
    std::size_t p = 0;
    bool bad = false;

    std::uint8_t u8() {
        if (p + 1 > b.size()) { bad = true; return 0; }
        return b[p++];
    }
    std::uint32_t u32() {
        if (p + 4 > b.size()) { bad = true; return 0; }
        std::uint32_t v = b[p] | (std::uint32_t(b[p + 1]) << 8) |
                          (std::uint32_t(b[p + 2]) << 16) | (std::uint32_t(b[p + 3]) << 24);
        p += 4;
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    double f64() {
        if (p + 8 > b.size()) { bad = true; return 0.0; }
        std::uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | b[p + std::size_t(i)];
        p += 8;
        double d = 0.0;
        static_assert(sizeof d == 8);
        std::memcpy(&d, &v, 8);
        return d;
    }
    std::string str() {
        const std::uint32_t n = u32();
        if (bad || p + n > b.size()) { bad = true; return {}; }
        std::string s(reinterpret_cast<const char*>(b.data() + p), n);
        p += n;
        return s;
    }
};

const char* as_err(const char* what) { return what; }

socket_category cat_or_bad(std::uint8_t byte, bool& bad) {
    if (byte > 3) { bad = true; return socket_category::utility_; }
    return static_cast<socket_category>(byte);  // cooked 0..3 == enum order
}

const char* assemble_kind_name(loadout_error e) {
    switch (e) {
        case loadout_error::duplicate_socket_id:    return "duplicate_socket_id";
        case loadout_error::unknown_component_type: return "unknown_component_type";
        case loadout_error::unknown_socket:         return "unknown_socket";
        case loadout_error::socket_occupied:        return "socket_occupied";
        case loadout_error::category_mismatch:      return "category_mismatch";
        case loadout_error::duplicate_instance_id:  return "duplicate_instance_id";
        case loadout_error::duplicate_attr_key:     return "duplicate_attr_key";
        case loadout_error::invalid_grid:           return "invalid_grid";
        case loadout_error::invalid_hull_mass:      return "invalid_hull_mass";
    }
    return "unknown";
}

}  // namespace

std::expected<loaded_hull, std::string> load_hull_loadout(
    const Unit& def0, const Unit& sock, std::uint64_t mesh_content_id) {
    using ve::core::cooked::kUnitDef0;
    using ve::core::cooked::kUnitSock;
    if (def0.type != kUnitDef0)
        return std::unexpected(std::string(as_err("def unit is not type DEF0")));
    if (sock.type != kUnitSock)
        return std::unexpected(std::string(as_err("anchor unit is not type SOCK")));

    // ---- DEF0 payload v2 (layout pinned in ve/core/cooked.h) ----
    Reader r{def0.payload};
    if (r.u32() != 2)
        return std::unexpected("DEF0 payload: version must be 2");
    hull_info hull{};
    hull.mesh_content_id = mesh_content_id;
    hull.class_tag = r.str();
    if (r.u8()) hull.hull_mass = r.f64();
    if (r.u8()) {
        hull.hull_com_local.x = r.f64();
        hull.hull_com_local.y = r.f64();
        hull.hull_com_local.z = r.f64();
    }
    if (r.u8()) {
        hull.hull_inertia_diag.x = r.f64();
        hull.hull_inertia_diag.y = r.f64();
        hull.hull_inertia_diag.z = r.f64();
    }
    grid_block grid{};
    grid.cell = r.f64();
    grid.subcell_ratio = static_cast<int>(r.u32());
    grid.cell_height = r.f64();
    grid.tolerance = r.f64();
    hull.grid = grid;
    const std::uint32_t nsec = r.u32();
    for (std::uint32_t s = 0; s < nsec && !r.bad; ++s) {  // decoded+skipped:
        r.str();                                          // sections are a
        const std::uint32_t nb = r.u32();                 // cook-validated
        for (std::uint32_t i = 0; i < nb * 6 && !r.bad; ++i) r.i32();
    }
    std::vector<mount_request> mounts;
    const std::uint32_t ncomp = r.u32();
    for (std::uint32_t i = 0; i < ncomp && !r.bad; ++i) {
        mount_request m;
        m.instance_id = r.str();
        m.type_id = r.str();
        (void)cat_or_bad(r.u8(), r.bad);  // category gate is assemble's job
        m.socket_id = r.str();
        (void)r.u8();                     // face (#46c1): interior graph later
        const std::uint32_t ntag = r.u32();
        for (std::uint32_t t = 0; t < ntag && !r.bad; ++t) r.str();
        const std::uint32_t nattr = r.u32();
        for (std::uint32_t a = 0; a < nattr && !r.bad; ++a)
            m.attrs.push_back({r.str(), r.f64()});
        const std::uint32_t nfac = r.u32();
        for (std::uint32_t f = 0; f < nfac && !r.bad; ++f) {  // facets skipped:
            r.str();                                         // Phase 6
            r.f64();
            r.u8();
            if (r.u8()) r.str();
        }
        mounts.push_back(std::move(m));
    }
    std::vector<component_type> catalog;
    const std::uint32_t ncat = r.u32();
    for (std::uint32_t i = 0; i < ncat && !r.bad; ++i) {
        component_type ct;
        ct.type_id = r.str();
        ct.category = cat_or_bad(r.u8(), r.bad);
        catalog.push_back(std::move(ct));
    }
    const std::uint32_t nwire = r.u32();
    for (std::uint32_t i = 0; i < nwire && !r.bad; ++i) {  // wires skipped:
        r.str();                                          // Phase 6 stats
        r.u8();
        if (r.u8()) r.f64();
        r.u8(); r.str();
        r.u8(); r.str();
    }
    const std::uint32_t nseam = r.u32();
    for (std::uint32_t i = 0; i < nseam && !r.bad; ++i) {  // seams skipped.
        r.str(); r.str();
        const std::uint8_t off = r.u8();
        if (off) for (int k = 0; k < 3; ++k) r.f64();
    }
    const std::uint32_t nref = r.u32();
    for (std::uint32_t i = 0; i < nref && !r.bad; ++i) r.str();
    if (r.bad) return std::unexpected("DEF0 payload: truncated or corrupt");
    if (r.p != def0.payload.size())
        return std::unexpected("DEF0 payload: trailing bytes");

    // ---- SOCK payload v2: every named anchor becomes a socket ----
    Reader s{sock.payload};
    loaded_hull out{};
    out.mesh_unit_id = s.str();
    if (s.u32() != 2)
        return std::unexpected("SOCK payload: version must be 2");
    std::vector<socket_desc> sockets;
    const std::uint32_t n = s.u32();
    for (std::uint32_t i = 0; i < n && !s.bad; ++i) {
        socket_desc sd;
        sd.id = s.str();
        (void)s.u8();  // kind: all anchors are sockets; kind needs no runtime
                       // interpretation (names are cooked-validated #46a)
        sd.category = cat_or_bad(s.u8(), s.bad);
        sd.position_local = {s.f64(), s.f64(), s.f64()};
        sd.orientation_local = {s.f64(), s.f64(), s.f64(), s.f64()};
        sockets.push_back(std::move(sd));
    }
    if (s.bad) return std::unexpected("SOCK payload: truncated or corrupt");
    if (s.p != sock.payload.size())
        return std::unexpected("SOCK payload: trailing bytes");

    auto lo = Loadout::assemble(hull, std::move(sockets), std::move(catalog),
                                std::move(mounts));
    if (!lo)
        return std::unexpected(std::string("loadout: ") + assemble_kind_name(lo.error()));
    out.loadout = std::move(*lo);
    return out;
}

}  // namespace ve::core::sim

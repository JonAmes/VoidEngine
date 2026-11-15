// Phase 8 task 8.19 (decision #43 mode A) — see ve/cook/author_gizmos.h.
// Doctrine recap: every mutation is a text edit round-tripped through the
// real cooker; gate failure reverts the text bit-exact (invalid drop = no
// write-back). Emitted literals ride #42 unit-suffix strings for unit-dim
// scalars ("m"/"rad" are bit-exact in the coercion table, so a re-parse of
// what we emit gives back exactly what we wrote within the emitted
// precision), and bare SI for triples (the hull-attr rule). Fixed decimals
// keep the git diff human-reviewable — gizmo drags never carry sub-micron
// or sub-nanoradian intent anyway.
#include "ve/cook/author_gizmos.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

#include <ve/cook/cook.h>
#include <ve/core/cooked.h>
#include <ve/core/math/base.h>
#include <ve/game/world_load.h>

namespace ve::cook {

using core::math::dvec3;
namespace math = core::math;
using ve::game::WorldDef;

// ---------------------------------------------------------------------------
// projection (the 8.10 pinhole x/y map, Vulkan y-down framebuffer)
// ---------------------------------------------------------------------------

std::optional<std::pair<double, double>> project_px(const dvec3& world,
                                                    const gizmo_view& view) {
    const dvec3 rel = world - view.eye;
    const double d = math::dot(rel, view.f);
    if (!(d > 0.0)) return std::nullopt;  // at/behind the eye plane
    const double x = math::dot(rel, view.s);
    const double y = math::dot(rel, view.u);
    // perspective_offaxis maps x_ndc = 2x/(d(r-l)) - (r+l)/(r-l); the
    // framebuffer flips y (Vulkan NDC y-down), py = h(1+ndc_y_fb)/2.
    const double ndc_x = 2.0 * x / (d * (view.tan_r - view.tan_l))
                         - (view.tan_r + view.tan_l) / (view.tan_r - view.tan_l);
    const double ndc_y_fb = -(2.0 * y / (d * (view.tan_t - view.tan_b))
                              - (view.tan_t + view.tan_b) / (view.tan_t - view.tan_b));
    return std::pair{view.width_px * (1.0 + ndc_x) / 2.0,
                     view.height_px * (1.0 + ndc_y_fb) / 2.0};
}

// ---------------------------------------------------------------------------
// line-oriented world-TOML text editing (write-back layer)
// ---------------------------------------------------------------------------
namespace {

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream f(text);
    std::string ln;
    while (std::getline(f, ln)) out.push_back(ln);
    return out;  // trailing-newline state carried by join_lines
}

std::string join_lines(const std::vector<std::string>& lines, bool trailing_nl) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) out += '\n';
        out += lines[i];
    }
    if (trailing_nl && !out.empty() && out.back() != '\n') out += '\n';
    return out;
}

std::string trim(std::string s) {
    const auto issp = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && issp(s.front())) s.erase(s.begin());
    while (!s.empty() && issp(s.back())) s.pop_back();
    return s;
}

// Parse a value literal on a `key = <lit>` line and re-emit the line with a
// new literal, preserving indent, key spelling and any trailing # comment.
// Returns false when the line is not a key line for `key`.
bool reemit_key_line(std::string& line, std::string_view key, const std::string& new_lit) {
    const std::string t = trim(line);
    if (t.size() < key.size() + 1 || t.compare(0, key.size(), key) != 0) return false;
    std::size_t i = key.size();
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
    if (i >= t.size() || t[i] != '=') return false;
    std::string rhs = trim(t.substr(i + 1));
    std::string comment;
    if (!rhs.empty() && rhs.front() == '"') {
        const auto close = rhs.find('"', 1);
        if (close == std::string::npos) return false;
        std::string tail = trim(rhs.substr(close + 1));
        if (!tail.empty() && tail.front() == '#') comment = "  " + tail;
    } else {
        const auto hash = rhs.find('#');
        if (hash != std::string::npos) {
            comment = "  " + trim(rhs.substr(hash));
            rhs = trim(rhs.substr(0, hash));
        }
    }
    const std::string indent = line.substr(0, line.find_first_not_of(" \t\r"));
    line = indent + t.substr(0, i) + "= " + new_lit + comment;
    return true;
}

// Root-table key: replace in place; insert before the first table header
// (or append at EOF) when absent. Returns the edited text.
bool set_root_key(std::vector<std::string>& lines, std::string_view key,
                  const std::string& lit, bool trailing_nl) {
    std::size_t first_header = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trim(lines[i]);
        if (!t.empty() && t.front() == '[') { first_header = i; break; }
        if (reemit_key_line(lines[i], key, lit)) return true;
    }
    std::string line = std::string(key) + " = " + lit;
    std::size_t at = first_header;
    // insert after the last non-blank root line before the header (or EOF)
    if (first_header == lines.size()) {
        while (at > 0 && trim(lines[at - 1]).empty()) --at;
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), line);
    (void)trailing_nl;
    return true;
}

// Locate the [[row]] block whose `id_key` literal equals `id`; return its
// [start, end) line range, or nullopt.
std::optional<std::pair<std::size_t, std::size_t>> find_row_block(
        const std::vector<std::string>& lines, std::string_view row,
        std::string_view id_key, std::string_view id) {
    const std::string header = "[[" + std::string(row) + "]]";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (trim(lines[i]) != header) continue;
        std::size_t j = i + 1;
        while (j < lines.size() && !(trim(lines[j]).size() >= 2 && trim(lines[j])[0] == '[')) ++j;
        for (std::size_t k = i + 1; k < j; ++k) {
            std::string t = trim(lines[k]);
            if (t.size() > id_key.size() && t.compare(0, id_key.size(), id_key) == 0) {
                const auto eq = t.find('=');
                if (eq == std::string::npos) continue;
                std::string lit = trim(t.substr(eq + 1));
                if (lit.size() >= 2 && lit.front() == '"' && lit.back() == '"')
                    lit = lit.substr(1, lit.size() - 2);
                if (lit == id) return std::pair{i, j};
            }
        }
        i = j - 1;
    }
    return std::nullopt;
}

// Replace (or append) a key inside a located row block.
bool set_row_key(std::vector<std::string>& lines, std::pair<std::size_t, std::size_t> block,
                 std::string_view key, const std::string& lit) {
    for (std::size_t k = block.first + 1; k < block.second; ++k)
        if (reemit_key_line(lines[k], key, lit)) return true;
    // append after the last non-blank line of the block
    std::size_t at = block.second;
    while (at > block.first + 1 && trim(lines[at - 1]).empty()) --at;
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at),
                 std::string(key) + " = " + lit);
    return true;
}

std::string fmt_m(double v)   { char b[64]; std::snprintf(b, sizeof b, "%.6f m", v); return "\"" + std::string(b) + "\""; }
std::string fmt_rad(double v) { char b[64]; std::snprintf(b, sizeof b, "%.9f rad", v); return "\"" + std::string(b) + "\""; }
std::string fmt_triple(const dvec3& v) {
    char b[128];
    std::snprintf(b, sizeof b, "[%f, %f, %f]", v.x, v.y, v.z);
    return std::string(b);
}

bool all_finite(const dvec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// def-ref resolution: `ref` is written relative to the file in `dir`
// ("../bodies/vesper.toml" from world/stations/). Yields the src-tree key
// path ("world/bodies/vesper.toml").
std::string resolve_ref(const std::string& dir, const std::string& ref) {
    std::filesystem::path p = std::filesystem::path(dir) / ref;
    std::filesystem::path out;
    for (const auto& part : p) {
        if (part == "..") { if (!out.empty()) out = out.parent_path(); }
        else if (part != ".") out /= part;
    }
    return out.generic_string();
}

std::string file_write(const std::string& path, const std::string& text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
    return f.good() ? "" : "write failed: " + path;
}

}  // namespace

// ---------------------------------------------------------------------------
// author_world
// ---------------------------------------------------------------------------

bool author_world::load(const std::map<std::string, std::string>& sources,
                        const std::string& src_dir, const std::string& out_dir,
                        std::string* err) {
    m_sources = sources;
    m_src = src_dir;
    m_out = out_dir;
    std::filesystem::remove_all(src_dir);
    std::filesystem::remove_all(out_dir);
    for (const auto& [path, text] : m_sources) {
        const auto w = file_write(src_dir + "/" + path, text);
        if (!w.empty()) { *err = w; return false; }
    }
    return recook_and_swap(err);
}

bool author_world::recook_and_swap(std::string* err) {
    std::string sink;
    if (!err) err = &sink;  // revert path: nobody reads the message
    // the write-back lands where the cooker looks: every live source text
    // is flushed to src_dir first (cook_incremental content-hashes units,
    // so unchanged files are skipped — this IS the #16 watch-loop path,
    // driven synchronously instead of by the timer).
    for (const auto& [path, text] : m_sources) {
        const auto w = file_write(m_src + "/" + path, text);
        if (!w.empty()) { *err = w; return false; }
    }
    const auto res = cook_incremental(m_src, m_out);
    if (!res.ok) { *err = "cook gate: " + res.error; return false; }
    std::map<std::string, cooked_def> fresh;
    for (const auto& [path, text] : m_sources) {
        std::ifstream probe(m_src + "/" + path);
        std::stringstream ss;
        ss << probe.rdbuf();
        // only world defs (root table declares world_class) get WRLD units
        if (ss.str().find("world_class") == std::string::npos) continue;
        core::cooked::Unit u;
        std::string uerr;
        if (!core::cooked::read_unit_file(m_out + "/units/" + path + ".ck", u, &uerr)) {
            *err = "post-cook read: " + uerr;
            return false;
        }
        auto dec = game::decode_world(u.payload);
        if (!dec) { *err = "post-cook decode: " + dec.error(); return false; }
        fresh[path] = cooked_def{*dec};
    }
    m_defs = std::move(fresh);
    return assemble_rails(err);
}

bool author_world::assemble_rails(std::string* err) {
    std::string sink;
    if (!err) err = &sink;
    m_rails = core::sim::rail_world{};
    m_body_ids.clear();
    // System class def(s) drive member order; add parents before children
    // (rail_world invariant: parent < id).
    for (const auto& [path, cd] : m_defs) {
        if (cd.def.world_class != game::WorldClass::system) continue;
        const std::string dir = std::filesystem::path(path).parent_path().generic_string();
        std::vector<std::string> members;
        for (const auto& m : cd.def.members) members.push_back(resolve_ref(dir, m));
        // topological pass until stable (bounded by member count)
        std::vector<std::string> pending = members;
        int guard = static_cast<int>(pending.size()) + 1;
        while (!pending.empty()) {
            if (--guard < 0) { *err = "rail parent cycle among system members"; return false; }
            for (auto it = pending.begin(); it != pending.end();) {
                const auto md = m_defs.find(*it);
                if (md == m_defs.end()) { *err = "member '" + *it + "' did not cook as a world def"; ++it; continue; }
                std::string pdir = std::filesystem::path(*it).parent_path().generic_string();
                std::int32_t parent = -1;
                if (!md->second.def.parent_ref.empty()) {
                    const auto pk = resolve_ref(pdir, md->second.def.parent_ref);
                    const auto bi = m_body_ids.find(pk);
                    if (bi == m_body_ids.end()) { ++it; continue; }  // parent not added yet
                    parent = bi->second;
                }
                auto bd = game::sim_body_def(md->second.def);
                if (md->second.def.world_class == game::WorldClass::station &&
                    bd.surface_radius <= 0.0) {
                    // Same stand-in as the realscale rails test: hull extent
                    // lives in DEF0 geometry, the add_body gate wants > 0.
                    bd.surface_radius = 100.0;
                }
                bd.parent = parent;
                const auto id = m_rails.add_body(bd);
                if (!id) { *err = "add_body refused '" + *it + "' (rail gate)"; return false; }
                m_body_ids[*it] = *id;
                it = pending.erase(it);
            }
        }
    }
    return true;
}

const WorldDef* author_world::def(std::string_view path) const {
    const auto it = m_defs.find(std::string(path));
    return it == m_defs.end() ? nullptr : &it->second.def;
}

std::int32_t author_world::body_id(std::string_view path) const {
    const auto it = m_body_ids.find(std::string(path));
    return it == m_body_ids.end() ? -1 : it->second;
}

bool author_world::drag_body(std::string_view def_path, const dvec3& target,
                             std::int32_t tick, std::string* err) {
    const std::string path(def_path);
    const auto d_it = m_defs.find(path);
    if (d_it == m_defs.end()) { *err = "no def '" + path + "'"; return false; }
    const auto id_it = m_body_ids.find(path);
    if (id_it == m_body_ids.end()) { *err = "'" + path + "' is not an assembled rail body"; return false; }
    const core::sim::body_def* b = m_rails.body(id_it->second);
    if (!b) { *err = "no rail body for '" + path + "'"; return false; }
    if (b->parent < 0) {
        *err = "'" + path + "' rides the static root anchor (#45c): it never moves";
        return false;
    }
    if (!all_finite(target)) { *err = "drag target must be finite"; return false; }
    const auto pp = m_rails.position(b->parent, tick);
    if (!pp) { *err = "parent rail position failed"; return false; }
    const dvec3 local = target - *pp;
    const double r = std::sqrt(local.x * local.x + local.z * local.z);
    if (!(r > 0.0)) { *err = "orbit radius must be > 0 (cursor on the rail axis)"; return false; }
    const core::sim::body_def* par = m_rails.body(b->parent);
    if (par && r < par->surface_radius) {
        char m[160];
        std::snprintf(m, sizeof m, "drop refused: orbit radius %f m is inside parent surface radius %f m (#43 drag gate)", r, par->surface_radius);
        *err = m;
        return false;
    }
    // rails.cpp: local = (r cos(theta), 0, -r sin(theta)),
    // theta = phase + tau*(t_s/period). Invert for the authored phase that
    // puts the body at the cursor NOW.
    const double theta = std::atan2(-local.z, local.x);
    const double t_s = core::sim::tick_to_seconds(tick);
    double phase = theta - math::kTau_d * (t_s / b->orbit_period_s);
    // wrap into [0, 2pi) for human reviewability; rails skip pre-wrapping
    // at EVAL time, but authored data reads better wrapped.
    phase = std::fmod(phase, math::kTau_d);
    if (phase < 0.0) phase += math::kTau_d;

    const std::string old_text = m_sources[path];
    const bool trailing_nl = !old_text.empty() && old_text.back() == '\n';
    auto lines = split_lines(old_text);
    set_root_key(lines, "orbit_radius", fmt_m(r), trailing_nl);
    set_root_key(lines, "orbit_phase", fmt_rad(phase), trailing_nl);
    m_sources[path] = join_lines(lines, trailing_nl);
    if (!recook_and_swap(err)) {
        revert(path, old_text);
        return false;
    }
    return true;
}

// Gate failure: restore the source text bit-exact and re-cook the OLD tree
// so disk, cooked units and assembled rails all snap back to the state
// before the refused drop (nothing but the error message survives).
void author_world::revert(const std::string& path, const std::string& old_text) {
    m_sources[path] = old_text;
    recook_and_swap(nullptr);
}

bool author_world::drag_waypoint(std::string_view sys_path, std::string_view wp_id,
                                 const dvec3& at, std::string* err) {
    const std::string path(sys_path);
    if (!m_defs.count(path)) { *err = "no def '" + path + "'"; return false; }
    if (!all_finite(at)) { *err = "waypoint target must be finite"; return false; }
    const std::string old_text = m_sources[path];
    const bool trailing_nl = !old_text.empty() && old_text.back() == '\n';
    auto lines = split_lines(old_text);
    const auto block = find_row_block(lines, "waypoint", "id", wp_id);
    if (!block) { *err = "no [[waypoint]] id='" + std::string(wp_id) + "'"; return false; }
    set_row_key(lines, *block, "at", fmt_triple(at));
    m_sources[path] = join_lines(lines, trailing_nl);
    if (!recook_and_swap(err)) {
        revert(path, old_text);
        return false;
    }
    return true;
}

bool author_world::drag_poi(std::string_view body_path, std::string_view poi_name,
                            const dvec3& center_dir, std::string* err) {
    const std::string path(body_path);
    if (!m_defs.count(path)) { *err = "no def '" + path + "'"; return false; }
    if (!all_finite(center_dir)) { *err = "poi direction must be finite"; return false; }
    const double n = std::sqrt(center_dir.x * center_dir.x + center_dir.y * center_dir.y
                               + center_dir.z * center_dir.z);
    if (!(n > 0.0)) { *err = "poi center direction must be non-zero"; return false; }
    const dvec3 dir{center_dir.x / n, center_dir.y / n, center_dir.z / n};
    const std::string old_text = m_sources[path];
    const bool trailing_nl = !old_text.empty() && old_text.back() == '\n';
    auto lines = split_lines(old_text);
    const auto block = find_row_block(lines, "poi", "name", poi_name);
    if (!block) { *err = "no [[poi]] name='" + std::string(poi_name) + "'"; return false; }
    set_row_key(lines, *block, "center", fmt_triple(dir));
    m_sources[path] = join_lines(lines, trailing_nl);
    if (!recook_and_swap(err)) {
        revert(path, old_text);
        return false;
    }
    return true;
}

std::vector<author_world::handle_pos> author_world::handles(std::int32_t tick) const {
    std::vector<handle_pos> out;
    for (const auto& [path, cd] : m_defs) {
        const auto& w = cd.def;
        if (w.world_class == game::WorldClass::body || w.world_class == game::WorldClass::station) {
            const auto id = body_id(path);
            if (id < 0) continue;
            const auto p = m_rails.position(id, tick);
            if (!p) continue;
            out.push_back({{path, {}, gizmo_handle::kind::body}, *p});
            // pad/POI handles ride the body surface along their direction
            for (const auto& poi : w.pois) {
                const dvec3 c = *p + poi.stamp.center_dir * w.rails.surface_radius;
                out.push_back({{path, poi.name, gizmo_handle::kind::poi}, c});
            }
        } else if (w.world_class == game::WorldClass::system) {
            const std::string dir = std::filesystem::path(path).parent_path().generic_string();
            for (const auto& wp : w.waypoints) {
                dvec3 c{wp.offset[0], wp.offset[1], wp.offset[2]};
                if (!wp.body_ref.empty()) {
                    const auto pk = resolve_ref(dir, wp.body_ref);
                    const auto id = body_id(pk);
                    if (id >= 0) if (const auto p = m_rails.position(id, tick)) c = c + *p;
                }
                out.push_back({{path, wp.id, gizmo_handle::kind::waypoint}, c});
            }
        }
    }
    std::ranges::sort(out, [](const handle_pos& a, const handle_pos& b) {
        return a.h.def_path < b.h.def_path ||
               (a.h.def_path == b.h.def_path && a.h.row < b.h.row);
    });
    return out;
}

std::optional<gizmo_handle> author_world::pick_at(double px, double py, double radius_px,
                                                  const gizmo_view& view,
                                                  std::int32_t tick) const {
    std::optional<gizmo_handle> best;
    double best_d = std::numeric_limits<double>::max();
    for (const auto& hp : handles(tick)) {
        const auto pp = project_px(hp.world, view);
        if (!pp) continue;
        const double dx = pp->first - px, dy = pp->second - py;
        if (dx * dx + dy * dy > radius_px * radius_px) continue;
        const dvec3 rel = hp.world - view.eye;
        const double d = math::dot(rel, view.f);
        if (d < best_d) { best_d = d; best = hp.h; }
    }
    if (best && m_sel) {
        const auto e = m_entities.find(best->def_path);
        if (e != m_entities.end()) m_sel->select(e->second);
    }
    return best;
}

}  // namespace ve::cook

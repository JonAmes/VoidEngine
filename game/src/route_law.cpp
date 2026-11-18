// Phase 9 tasks 9.5a/9.5 — shared wire-route text grammar + spatial law
// (see route_law.h).
#include "ve/game/route_law.h"

#include <charconv>
#include <cmath>
#include <cstdlib>

namespace ve::game {

namespace {
// Trim spaces/tabs and TOML bracket decoration from a comma-separated token.
std::string_view trim_cell(std::string_view t) {
    const auto b = t.find_first_not_of(" \t[");
    const auto e = t.find_last_not_of(" \t]");
    if (b == std::string_view::npos) return {};
    return t.substr(b, e - b + 1);
}

// Iterate comma-joined integer tokens; nullptr out_ptr per token on failure.
template <typename F>
bool each_int(std::string_view text, F&& on_int) {
    std::string_view v = text;
    for (;;) {
        const auto cm = v.find(',');
        const auto tok = trim_cell(cm == std::string_view::npos ? v : v.substr(0, cm));
        if (!tok.empty()) {
            long long n = 0;
            const auto res = std::from_chars(tok.data(), tok.data() + tok.size(), n);
            if (res.ec != std::errc{} || res.ptr != tok.data() + tok.size())
                return false;  // caller builds the token-specific message
            on_int(n);
        }
        if (cm == std::string_view::npos) break;
        v = v.substr(cm + 1);
    }
    return true;
}
} // namespace

bool route_text_ok(std::string_view text, std::string& msg) {
    std::size_t n = 0;
    // One pass tracking the first offending token for the message.
    std::string_view v = text;
    for (;;) {
        const auto cm = v.find(',');
        const auto tok = trim_cell(cm == std::string_view::npos ? v : v.substr(0, cm));
        if (!tok.empty()) {
            long long x = 0;
            const auto res = std::from_chars(tok.data(), tok.data() + tok.size(), x);
            if (res.ec != std::errc{} || res.ptr != tok.data() + tok.size()) {
                msg = "cell coord '" + std::string(tok) + "' is not an integer";
                return false;
            }
            ++n;
        }
        if (cm == std::string_view::npos) break;
        v = v.substr(cm + 1);
    }
    // #51: routes ride the CELL lattice — whole [x,y,z] triples (3 coords,
    // NOT the 6 bounds of an int_boxes box; 9.5a split them apart).
    if (n == 0 || n % 3 != 0) {
        msg = "route needs whole [x,y,z] cells (3 coords each)";
        return false;
    }
    return true;
}

bool parse_route_text(std::string_view text, std::vector<route_cell>& route, std::string& msg) {
    if (!route_text_ok(text, msg))
        return false;
    std::vector<long long> nums;
    each_int(text, [&](long long n) { nums.push_back(n); });
    route.clear();
    for (std::size_t i = 0; i < nums.size(); i += 3) {
        // int32 range check: cells beyond int32 cannot ride the lattice.
        if (nums[i] < INT32_MIN || nums[i] > INT32_MAX || nums[i + 1] < INT32_MIN ||
            nums[i + 1] > INT32_MAX || nums[i + 2] < INT32_MIN || nums[i + 2] > INT32_MAX) {
            msg = "route cell coords out of range";
            return false;
        }
        route.push_back({static_cast<std::int32_t>(nums[i]),
                         static_cast<std::int32_t>(nums[i + 1]),
                         static_cast<std::int32_t>(nums[i + 2])});
    }
    return true;
}

namespace {
std::string cell_str(const route_cell& c) {
    return "(" + std::to_string(c[0]) + "," + std::to_string(c[1]) +
           "," + std::to_string(c[2]) + ")";
}
bool cell_in_box(const route_cell& c, const route_box& box) {
    for (int ax = 0; ax < 3; ++ax)
        if (c[ax] < box.bounds[ax] || c[ax] > box.bounds[ax + 3]) return false;
    return true;
}
std::vector<std::size_t> sections_containing(const std::vector<route_section>& secs,
                                             const route_cell& c) {
    std::vector<std::size_t> hit;
    for (std::size_t s = 0; s < secs.size(); ++s)
        for (const auto& box : secs[s].boxes)
            if (cell_in_box(c, box)) { hit.push_back(s); break; }
    return hit;
}
} // namespace

std::string route_law_error(const std::vector<route_section>& sections,
                            const std::vector<route_door_seam>& seams,
                            const route_wire& w) {
    // Strings moved byte-for-byte out of the cook gate (9.3 -> 9.5): cook
    // hullvalidate is now a wrapper, so editor refusal == cook refusal.
    if (w.route.empty())
        return "wire '" + w.id + "' has an empty route (#51)";
    if (w.route.front() != w.from_cell)
        return "wire '" + w.id + "' route starts at " +
               cell_str(w.route.front()) + ", not at its 'from' "
               "endpoint cell " + cell_str(w.from_cell) + " (#51 G12a)";
    if (w.route.back() != w.to_cell)
        return "wire '" + w.id + "' route ends at " +
               cell_str(w.route.back()) + ", not at its 'to' "
               "endpoint cell " + cell_str(w.to_cell) + " (#51 G12a)";
    for (std::size_t i = 0; i + 1 < w.route.size(); ++i) {
        const route_cell& a = w.route[i];
        const route_cell& b = w.route[i + 1];
        const int man = std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) +
                        std::abs(a[2] - b[2]);
        if (man != 1)
            return "wire '" + w.id + "' route cells " +
                   std::to_string(i) + " " + cell_str(a) + " and " +
                   std::to_string(i + 1) + " " + cell_str(b) +
                   " are not 6-adjacent (#51 G12b)";
    }
    for (const auto& cell : w.route) {
        const auto hit = sections_containing(sections, cell);
        if (hit.size() != 1)
            return "wire '" + w.id + "' route cell " + cell_str(cell) +
                   " is in " + std::to_string(hit.size()) +
                   " sections — not inside the hull lattice "
                   "(#51 G12c)";
    }
    for (std::size_t i = 0; i + 1 < w.route.size(); ++i) {
        const route_cell& a = w.route[i];
        const route_cell& b = w.route[i + 1];
        const auto ha = sections_containing(sections, a);
        const auto hb = sections_containing(sections, b);
        if (sections[ha[0]].name == sections[hb[0]].name) continue;
        bool seam = false;
        for (const auto& s : seams)
            if ((s.a == a && s.b == b) || (s.a == b && s.b == a)) {
                seam = true;
                break;
            }
        if (!seam)
            return "wire '" + w.id + "' route crosses section '" +
                   sections[ha[0]].name + "' -> '" + sections[hb[0]].name +
                   "' between cells " + cell_str(a) + " -> " + cell_str(b) +
                   " with no door seam (#51 G12d)";
    }
    return {};
}

std::string route_text_of(const std::vector<route_cell>& route) {
    std::string out;
    for (const auto& c : route) {
        if (!out.empty())
            out += ", ";
        out += std::to_string(c[0]) + ", " + std::to_string(c[1]) + ", " + std::to_string(c[2]);
    }
    return out;
}

} // namespace ve::game

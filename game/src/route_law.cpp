// Phase 9 task 9.5a — shared wire-route text grammar (see route_law.h).
#include "ve/game/route_law.h"

#include <charconv>
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

// Phase 8 task 8.1 — the "NUMBER SUFFIX" def-string grammar, moved verbatim
// out of the cooklet's TOML parser (tools/cook/src/toml.cpp) so the def-forms
// entry gate and the cook parser share ONE implementation (#42: enforced on
// entry, and still hard-rejected at cook). Typed failures; the historical
// message strings live in entry_error_message so ve::cook::si_from_string
// stays byte-identical (tests/cook/test_toml.cpp pins substrings).
#include "ve/game/units.h"

#include <charconv>
#include <cmath>
#include <string>

namespace ve::game {

std::expected<double, entry_failure> parse_unit_entry(std::string_view text,
                                                      std::optional<unit_dim> want) {
    auto fail = [](entry_error kind, std::string token, unit_dim want_dim = unit_dim::length) {
        return std::unexpected(entry_failure{kind, std::move(token), want_dim});
    };
    // trim ASCII whitespace so " 1.5 AU " parses like "1.5 AU".
    std::size_t a = 0, b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t')) ++a;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t')) --b;
    const std::string_view s = text.substr(a, b - a);
    const std::size_t ws = s.find_last_of(" \t");
    std::string num_tok, suffix;
    if (ws == std::string_view::npos) {
        num_tok = std::string(s);  // bare number: error below (#42 strict)
    } else {
        num_tok = std::string(s.substr(0, ws));
        suffix = std::string(s.substr(ws + 1));
        // split was at the LAST whitespace, so only interior extra tokens
        // can carry whitespace — on either side ("1 2 m", "1 x m").
        if (suffix.find_first_of(" \t") != std::string::npos ||
            num_tok.find_first_of(" \t") != std::string::npos)
            return fail(entry_error::too_many_tokens, std::string(s));
    }
    double value = 0.0;
    const char* first = num_tok.data();
    const char* last = num_tok.data() + num_tok.size();
    const auto res = std::from_chars(first, last, value);
    if (res.ec != std::errc{} || res.ptr != last || !std::isfinite(value))
        return fail(entry_error::bad_number, num_tok);
    if (suffix.empty()) return fail(entry_error::bare_suffix, num_tok);
    const auto coerced = coerce_to_si(value, suffix);
    if (!coerced) return fail(entry_error::unknown_suffix, suffix);
    if (want && dim_of_suffix(suffix) != *want)
        return fail(entry_error::wrong_dim, suffix, *want);
    return *coerced;
}

std::string entry_error_message(const entry_failure& f) {
    switch (f.kind) {
        case entry_error::too_many_tokens:
            return "unit coercion: too many tokens: '" + f.token + "'";
        case entry_error::bad_number:
            return "unit coercion: bad number '" + f.token + "'";
        case entry_error::bare_suffix:
            return "unit coercion: bare number '" + f.token + "' (suffix mandatory per #42)";
        case entry_error::unknown_suffix:
            return "unit coercion: unknown suffix '" + f.token + "'";
        case entry_error::wrong_dim:
            return "unit coercion: suffix '" + f.token + "' is not a " +
                   std::string(dim_name(f.want)) + " unit";
    }
    return "unit coercion: bad value";
}

std::expected<double, std::string> si_from_string(std::string_view text) {
    auto r = parse_unit_entry(text, std::nullopt);
    if (!r) return std::unexpected(entry_error_message(r.error()));
    return *r;
}

}  // namespace ve::game

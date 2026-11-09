// Phase 5 task 5.7 — TOML subset parser implementation (see toml.h for the
// subset + determinism contract). Recursive-descent char scanner; no
// dependencies beyond stdlib.
#include "ve/cook/toml.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>

#include <ve/game/units.h>

namespace ve::cook {
namespace {

bool bare_key_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

struct Parser {
    const std::string& s;
    std::size_t i = 0;
    int line = 1;
    std::string err_msg;
    TomlTable* cur = nullptr;  // current table opened by [header]/[[arr]]

    explicit Parser(const std::string& text) : s(text) {}

    bool fail(const std::string& msg) {
        if (err_msg.empty())
            err_msg = "line " + std::to_string(line) + ": " + msg;
        return false;
    }

    bool eof() const { return i >= s.size(); }
    char peek() const { return eof() ? '\0' : s[i]; }

    void skip_inline_ws() {
        while (!eof() && (s[i] == ' ' || s[i] == '\t')) ++i;
    }
    void skip_comment() {
        if (peek() == '#')
            while (!eof() && s[i] != '\n') ++i;
    }
    // one newline consumed (line tracked); returns false at EOF
    bool skip_to_next_line() {
        skip_inline_ws();
        skip_comment();
        if (eof()) return false;
        if (s[i] == '\n') { ++i; ++line; return true; }
        if (s[i] == '\r') { ++i; if (peek() == '\n') ++i; ++line; return true; }
        return false;
    }

    bool parse_quoted_key(std::string& out) {
        if (peek() != '"') return fail("quoted key must start with \"");
        TomlValue v;
        if (!parse_string(v)) return false;
        out = v.str;
        return true;
    }

    // dotted key path (quoted segments allowed)
    bool parse_key_path(std::vector<std::string>& out) {
        out.clear();
        for (;;) {
            skip_inline_ws();
            std::string k;
            if (peek() == '"') {
                if (!parse_quoted_key(k)) return false;
            } else {
                if (eof() || !bare_key_char(s[i]))
                    return fail("expected key");
                while (!eof() && bare_key_char(s[i])) k += s[i++];
            }
            out.push_back(std::move(k));
            skip_inline_ws();
            if (peek() == '.') { ++i; continue; }
            return true;
        }
    }

    bool parse_string(TomlValue& v) {
        ++i;  // opening quote
        std::string out;
        while (!eof() && s[i] != '"') {
            char c = s[i];
            if (c == '\n') return fail("unterminated string");
            if (c == '\\') {
                ++i;
                if (eof()) return fail("unterminated escape");
                switch (s[i]) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case 'b': out += '\b'; break;
                    case 't': out += '\t'; break;
                    case 'n': out += '\n'; break;
                    case 'f': out += '\f'; break;
                    case 'r': out += '\r'; break;
                    case 'u': {
                        if (i + 4 >= s.size()) return fail("bad \\u escape");
                        const std::string hex = s.substr(i + 1, 4);
                        char* end = nullptr;
                        const long cp = std::strtol(hex.c_str(), &end, 16);
                        if (end != hex.c_str() + 4) return fail("bad \\u escape");
                        if (cp >= 0xD800 && cp <= 0xDFFF)
                            return fail("lone surrogate in \\u escape");
                        // UTF-8 encode (BMP: 1-3 bytes)
                        if (cp < 0x80) out += static_cast<char>(cp);
                        else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        i += 4;
                        break;
                    }
                    default: return fail("unknown escape");
                }
                ++i;
            } else {
                out += c;
                ++i;
            }
        }
        if (eof()) return fail("unterminated string");
        ++i;  // closing quote
        v.kind = TomlValue::kString;
        v.str = std::move(out);
        return true;
    }

    bool parse_value(TomlValue& v) {
        skip_inline_ws();
        const char c = peek();
        if (c == '"') return parse_string(v);
        if (c == '[') {
            ++i;
            v.kind = TomlValue::kArray;
            for (;;) {
                skip_inline_ws();
                if (peek() == ']') { ++i; return true; }
                if (eof()) return fail("unterminated array");
                TomlValue e;
                if (!parse_value(e)) return false;
                v.array.push_back(std::move(e));
                for (;;) {  // arrays may span lines (TOML) — skip them
                    if (s[i] == '\n') { ++i; ++line; }
                    else if (s[i] == '\r') { ++i; if (peek() == '\n') ++i; ++line; }
                    else break;
                    skip_inline_ws();
                    skip_comment();
                    if (eof()) return fail("unterminated array");
                }
                if (peek() == ',') { ++i; continue; }
                if (peek() == ']') { ++i; return true; }
                return fail("expected , or ] in array");
            }
        }
        if (s.compare(i, 4, "true") == 0) { i += 4; v.kind = TomlValue::kBoolean; v.boolean = true; return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; v.kind = TomlValue::kBoolean; v.boolean = false; return true; }
        // number: strict grammar [-+]digits[.digits][eE[-+]digits]
        const std::size_t start = i;
        if (peek() == '+' || peek() == '-') ++i;
        std::size_t digits = 0;
        while (!eof() && s[i] >= '0' && s[i] <= '9') { ++i; ++digits; }
        bool real = false;
        if (peek() == '.') {
            real = true; ++i;
            while (!eof() && s[i] >= '0' && s[i] <= '9') { ++i; ++digits; }
        }
        if (digits == 0) { i = start; return fail("expected value"); }
        if (peek() == 'e' || peek() == 'E') {
            real = true; ++i;
            if (peek() == '+' || peek() == '-') ++i;
            std::size_t ed = 0;
            while (!eof() && s[i] >= '0' && s[i] <= '9') { ++i; ++ed; }
            if (ed == 0) return fail("bad exponent");
        }
        const std::string tok = s.substr(start, i - start);
        if (real) {
            v.kind = TomlValue::kReal;
            v.real = std::strtod(tok.c_str(), nullptr);
            if (!std::isfinite(v.real)) return fail("number overflow: " + tok);
        } else {
            v.kind = TomlValue::kInteger;
            errno = 0;
            v.integer = std::strtoll(tok.c_str(), nullptr, 10);
            if (errno == ERANGE) return fail("integer overflow: " + tok);
        }
        return true;
    }

    bool run(TomlValue& root) {
        root.kind = TomlValue::kTable;
        cur = &root.table;  // key/value insert target; last header opened
        for (;;) {
            skip_inline_ws();
            skip_comment();
            if (eof()) return true;
            if (s[i] == '\n' || s[i] == '\r') { skip_to_next_line(); continue; }
            if (s[i] == '[') {
                ++i;
                const bool arr = peek() == '[';
                if (arr) ++i;
                std::vector<std::string> path;
                if (!parse_key_path(path)) return false;
                skip_inline_ws();
                if (peek() != ']') return fail("expected ]");
                ++i;
                if (arr && peek() != ']') return fail("expected ]]");
                if (arr) ++i;
                TomlTable* tbl = &root.table;
                for (std::size_t d = 0; d + 1 < path.size(); ++d) {
                    auto it = tbl->find(path[d]);
                    if (it == tbl->end()) {
                        TomlValue fresh; fresh.kind = TomlValue::kTable;
                        it = tbl->emplace(path[d], std::move(fresh)).first;
                    } else if (it->second.kind != TomlValue::kTable) {
                        return fail("cannot redefine '" + path[d] + "' as table");
                    }
                    tbl = &it->second.table;
                }
                auto it = tbl->find(path.back());
                if (arr) {
                    if (it == tbl->end()) {
                        TomlValue fresh; fresh.kind = TomlValue::kArray;
                        it = tbl->emplace(path.back(), std::move(fresh)).first;
                    } else if (it->second.kind != TomlValue::kArray) {
                        return fail("cannot append table to non-array '" + path.back() + "'");
                    }
                    TomlValue fresh; fresh.kind = TomlValue::kTable;
                    it->second.array.push_back(std::move(fresh));
                    cur = &it->second.array.back().table;  // keys target the NEW entry
                    if (!skip_to_next_line() && !eof())
                        return fail("garbage after header");
                    continue;
                }
                if (it != tbl->end() && it->second.kind != TomlValue::kTable)
                    return fail("cannot redefine '" + path.back() + "' as table");
                if (it == tbl->end()) {
                    TomlValue fresh; fresh.kind = TomlValue::kTable;
                    it = tbl->emplace(path.back(), std::move(fresh)).first;
                }
                cur = &it->second.table;  // subsequent keys land in this table

                if (!skip_to_next_line() && !eof())
                    return fail("garbage after header");
                continue;
            }
            // key = value
            std::vector<std::string> path;
            if (!parse_key_path(path)) return false;
            skip_inline_ws();
            if (peek() != '=') return fail("expected = after key");
            ++i;
            TomlValue v;
            if (!parse_value(v)) return false;
            skip_inline_ws();
            if (!eof() && s[i] != '#' && s[i] != '\n' && s[i] != '\r')
                return fail("garbage after value");
            // insert at dotted path relative to the CURRENT table
            TomlTable* parent = cur;
            for (std::size_t d = 0; d + 1 < path.size(); ++d) {
                auto it = parent->find(path[d]);
                if (it == parent->end()) {
                    TomlValue t; t.kind = TomlValue::kTable;
                    it = parent->emplace(path[d], std::move(t)).first;
                } else if (it->second.kind != TomlValue::kTable) {
                    return fail("cannot descend into non-table '" + path[d] + "'");
                }
                parent = &it->second.table;
            }
            if (parent->count(path.back()))
                return fail("duplicate key '" + path.back() + "'");
            (*parent)[path.back()] = std::move(v);
            if (!skip_to_next_line() && !eof())
                return fail("garbage after value");
        }
    }
};

}  // namespace

bool parse_toml(const std::string& text, TomlValue& root, std::string* err) {
    Parser p(text);
    if (!p.run(root)) {
        if (err) *err = p.err_msg;
        return false;
    }
    return true;
}

std::expected<double, std::string> si_from_string(const std::string& s) {
    // "NUMBER SUFFIX" with exactly one whitespace boundary. Extra whitespace
    // tolerated at the ends; interior splits on the LAST whitespace so values
    // like "1.5e 3 m" stay rejected by the number grammar (only one token
    // before the suffix is allowed).
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    const std::size_t ws = s.find_last_of(" \t", b - 1);
    std::string num_tok, suffix;
    if (ws == std::string::npos || ws < a) {
        num_tok = s.substr(a, b - a);  // no suffix -> bare error below
    } else {
        num_tok = s.substr(a, ws - a);
        suffix = s.substr(ws + 1, b - ws - 1);
        if (suffix.find_first_of(" \t") != std::string::npos ||
            num_tok.find_first_of(" \t") != std::string::npos)
            return std::unexpected("unit coercion: too many tokens: '" + s + "'");
    }
    // strict number grammar; from_chars rejects inf/nan spellings
    double val = 0.0;
    const char* first = num_tok.c_str();
    const char* last = num_tok.c_str() + num_tok.size();
    const auto res = std::from_chars(first, last, val);
    // from_chars DOUBLE accepts "inf"/"nan" spellings — never valid def data.
    if (res.ec != std::errc{} || res.ptr != last || !std::isfinite(val))
        return std::unexpected("unit coercion: bad number '" + num_tok + "'");
    const auto si = ve::game::coerce_to_si(val, suffix);
    if (!si) {
        switch (si.error()) {
            case ve::game::unit_error::bare_suffix:
                return std::unexpected("unit coercion: bare number '" + num_tok +
                                       "' (suffix mandatory per #42)");
            case ve::game::unit_error::unknown_suffix:
                return std::unexpected("unit coercion: unknown suffix '" + suffix + "'");
        }
    }
    return *si;
}

}  // namespace ve::cook

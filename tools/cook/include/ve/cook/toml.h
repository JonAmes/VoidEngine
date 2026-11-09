#pragma once
// Phase 5 task 5.7 — hand-rolled TOML SUBSET parser (no new dependency).
// Subset chosen for hull-def / component-def authoring (5.8+):
//   * comments (# to end of line), blank lines
//   * [table] and [a.b] headers; [[array-of-tables]] headers (append)
//   * key = value; keys bare ([A-Za-z0-9_-]+) or "quoted"; dotted keys (a.b = v)
//   * values: "basic strings" (escapes \" \\ \b \t \n \f \r \uXXXX as UTF-8),
//     integers (decimal, optional sign), floats (optional sign, fraction and/or
//     exponent), true/false, inline arrays [v, v,] (nest to any depth)
// NOT supported (add only when a def needs them): multi-line strings, raw
// strings, datetimes, inline tables, unicode escapes > U+FFFF.
// Determinism contract: tables iterate in KEY ORDER (std::map) — never
// insertion order — so every consumer sees a fixed sequence. Duplicate keys
// (value or table colliding) are a parse error. All errors carry line numbers.
#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <vector>

namespace ve::cook {

struct TomlValue;
using TomlTable = std::map<std::string, TomlValue>; // ordered: deterministic iteration

struct TomlValue {
    enum Kind { kString, kInteger, kReal, kBoolean, kArray, kTable } kind = kString;
    std::string str;
    std::int64_t integer = 0;
    double real = 0.0;
    bool boolean = false;
    std::vector<TomlValue> array;
    TomlTable table;

    bool is_table() const { return kind == kTable; }
    bool is_string() const { return kind == kString; }
    bool is_number() const { return kind == kInteger || kind == kReal; }
    // Numeric value as double whether authored int or float.
    double num() const { return kind == kInteger ? static_cast<double>(integer) : real; }
};

// root is always a kTable on success. err gets a "line N: ..." message on failure.
bool parse_toml(const std::string& text, TomlValue& root, std::string* err);

// #42 unit coercion at parse time (the runtime never sees suffixes): a def
// value authored as a "NUMBER SUFFIX" string ("5 km", "0.5 deg/day") -> SI
// double via the ve::game table. Missing suffix -> bare_suffix, unknown ->
// unknown_suffix, bad number -> error. Bare NUMBER values are NOT coerced —
// call sites must ask for a unit string where the field has a unit.
std::expected<double, std::string> si_from_string(const std::string& s);

}  // namespace ve::cook

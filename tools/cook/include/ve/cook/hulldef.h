#pragma once
// Phase 5 task 5.8 — hull def TOML -> normalized binary DEF0 payload.
// A .toml def that declares a top-level `class_tag` is a HULL DEF and is
// compiled into the DEF0 v1 binary documented in ve/core/cooked.h; every
// unit-suffixed field is coerced to SI at cook time (#42), so the runtime
// never sees TOML or suffixes. A def WITHOUT class_tag keeps its raw source
// bytes (non-hull defs get their schema in a later phase). Refs are
// extracted either way; the caller resolves them to manifest deps.
#include <string>
#include <vector>

#include "ve/cook/cook.h"  // Bytes

namespace ve::cook {

// Parse + schema-validate + normalize. On false, *err carries the reason.
// payload_out: DEF0 v1 binary (hull def) or raw source bytes (plain def).
// refs_out: top-level `refs = [...]` strings in authored order (may be empty).
bool cook_hulldef_unit(const std::string& toml_text, Bytes& payload_out,
                       std::vector<std::string>& refs_out, std::string* err);

}  // namespace ve::cook

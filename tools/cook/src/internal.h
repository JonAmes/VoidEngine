#pragma once
#include "ve/cook/cook.h"

// cooklet-internal helpers shared between TUs (not public API).
// File IO + hashes now come from ve::core::cooked (single-source codec).
namespace ve::cook {
// def's directory + ref, lexically normalized; false if it escapes the root.
// Defined in cook.cpp; glb.cpp uses it for texture URIs (3.8).
bool resolve_ref(const std::string& def_id, const std::string& ref, std::string& id_out);
} // namespace ve::cook

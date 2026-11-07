#pragma once
#include "ve/cook/cook.h"

#include <string>

// cooklet-internal helpers shared between TUs (not public API).
namespace ve::cook::detail {
bool read_bytes(const std::string& path, Bytes& out, std::string* err);
bool write_bytes(const std::string& path, const Bytes& data, std::string* err);
} // namespace ve::cook::detail

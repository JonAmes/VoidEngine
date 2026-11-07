#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ve::cook {

using Bytes = std::vector<std::uint8_t>;

// ---- content IDs (decision #13): the stable ID of every unit is its path
// relative to the source root, slash-separated. Renaming = new content (git
// notices); no UUIDs. -------------------------------------------------------

constexpr std::uint32_t fourcc(const char* s) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) << 0) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[3])) << 24);
}

inline constexpr std::uint32_t kUnitRawD = fourcc("RAWD"); // raw passthrough bytes
inline constexpr std::uint32_t kUnitTex0 = fourcc("TEX0"); // decoded texture (0.7)
inline constexpr std::uint32_t kUnitMesh = fourcc("MESH"); // cooked mesh (0.8)
inline constexpr std::uint32_t kUnitMat0 = fourcc("MAT0"); // cooked material (0.8)
inline constexpr std::uint32_t kUnitDef0 = fourcc("DEF0"); // normalized def

inline constexpr std::uint32_t kCookFormat = 1;

// ---- cooked unit: self-contained, independently loadable ------------------
// 'VECK' | u32 format | u32 type | u32 id_len | id bytes | u64 payload_len |
// payload | u32 crc32(payload)
struct Unit {
    std::string id;
    std::uint32_t type = 0;
    Bytes payload;
};

bool write_unit_file(const std::string& path, const Unit& unit, std::string* err);
bool read_unit_file(const std::string& path, Unit& out, std::string* err);

// ---- manifest.bin: content ID -> deps + source hash ------------------------
// 'VEMF' | u32 format | u32 entry_count | entries sorted by content ID:
//   u32 id_len, id | u32 dep_count, (u32 len, dep)* | 32 bytes sha256(source)
struct ManifestEntry {
    std::string id;
    std::vector<std::string> deps; // content IDs this unit needs
    std::array<std::uint8_t, 32> source_sha{};
};

// std::map keeps entries ordered by content ID — deterministic bytes.
using Manifest = std::map<std::string, ManifestEntry>;

bool write_manifest(const std::string& path, const Manifest& m, std::string* err);
bool read_manifest(const std::string& path, Manifest& out, std::string* err);

// ---- incremental cook ------------------------------------------------------
struct CookCounts {
    int cooked = 0;
    int skipped = 0;
    int failures = 0;
};
struct CookResult {
    bool ok = false;          // false iff any source failed to cook
    std::string error;        // first failure message (actionable)
    CookCounts counts;
    std::vector<std::string> unit_ids; // all cooked+skipped ids, sorted
};

// Scans src_dir, cooks every file whose source hash differs from the manifest,
// resolves def refs into the dependency graph, writes units under
// out_dir/units/<id>.ck and a deterministic out_dir/manifest.bin.
CookResult cook_incremental(const std::string& src_dir, const std::string& out_dir);

// ---- watch mode: incremental cook on a fixed interval (dev hot-swap loop,
// decision #16: editors write source, the watch cooklet re-cooks instantly).
void watch_loop(const std::string& src_dir, const std::string& out_dir,
                int interval_ms, const std::atomic<bool>& stop,
                CookCounts* totals_out = nullptr);

// ---- helpers exposed for tests ---------------------------------------------
std::array<std::uint8_t, 32> sha256(const Bytes& data);
std::uint32_t crc32(const Bytes& data);

// Resolve a def's raw ref string (relative to the def's directory) to a
// source-root-relative content ID. Returns false if it escapes the root.
bool resolve_ref(const std::string& def_id, const std::string& ref,
                 std::string& id_out);

// Parse the minimal def subset we support in Phase 0 (key = "string" /
// key = ["a", "b"] lines; '#' comments). Expands with the defs phase.
struct DefData {
    std::vector<std::string> refs;
};
bool parse_def(const Bytes& data, DefData& out, std::string* err);

} // namespace ve::cook

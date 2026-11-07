#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <ve/core/cooked.h>

namespace ve::cook {

// The cooked binary format (units, manifest, hashes) is shared with the
// runtime and lives in core/ — the codec must be single-sourced so cook and
// load can never drift. Bring it into the cooklet's namespace.
using namespace ve::core::cooked;

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

#include "ve/cook/cook.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <set>
#include <thread>

#include "internal.h"

namespace ve::cook {
namespace {

std::string unit_path(const std::string& out_dir, const std::string& id) {
    return (std::filesystem::path(out_dir) / "units" / (id + ".ck")).string();
}
std::string manifest_path(const std::string& out_dir) {
    return (std::filesystem::path(out_dir) / "manifest.bin").string();
}

std::uint32_t unit_type_for(const std::string& id) {
    std::filesystem::path p(id);
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".toml") return kUnitDef0;
    return kUnitRawD; // .png -> TEX0 in 0.7; .glb -> MESH in 0.8
}

// Deterministic scan: every regular file under src_dir, relative ids, sorted.
std::vector<std::string> scan_sources(const std::string& src_dir) {
    std::set<std::string> ids;
    std::error_code ec;
    for (const auto& e : std::filesystem::recursive_directory_iterator(src_dir, ec)) {
        if (e.is_regular_file()) {
            auto rel = std::filesystem::relative(e.path(), src_dir, ec);
            ids.insert(rel.generic_string()); // '/' separators
        }
    }
    return {ids.begin(), ids.end()};
}

} // namespace

bool resolve_ref(const std::string& def_id, const std::string& ref, std::string& id_out) {
    // def's directory + ref, lexically normalized; must stay inside the root.
    std::filesystem::path p = std::filesystem::path(def_id).parent_path() / ref;
    std::vector<std::string> parts;
    for (const auto& seg : p) {
        std::string s = seg.string();
        if (s == "." || s.empty()) continue;
        if (s == "..") {
            if (parts.empty()) return false; // escapes the source root
            parts.pop_back();
            continue;
        }
        parts.push_back(s);
    }
    if (parts.empty()) return false;
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    id_out = out;
    return true;
}

bool parse_def(const Bytes& data, DefData& out, std::string* err) {
    std::string text(data.begin(), data.end());
    std::size_t pos = 0;
    int line_no = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        ++line_no;
        bool in_quote = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '"') in_quote = !in_quote;
            else if (line[i] == '#' && !in_quote) { line.resize(i); break; }
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            auto issp = [](unsigned char c) { return std::isspace(c) != 0; };
            while (!s.empty() && issp(s.front())) s.erase(s.begin());
            while (!s.empty() && issp(s.back())) s.pop_back();
            return s;
        };
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (key != "refs") continue;
        if (val.size() >= 2 && val.front() == '[' && val.back() == ']') {
            // collect every quoted string in the array
            std::size_t i = 1;
            while (i < val.size()) {
                auto q1 = val.find('"', i);
                if (q1 == std::string::npos) break;
                auto q2 = val.find('"', q1 + 1);
                if (q2 == std::string::npos) {
                    if (err) *err = "unterminated string in refs array (line " + std::to_string(line_no) + ")";
                    return false;
                }
                out.refs.push_back(val.substr(q1 + 1, q2 - q1 - 1));
                i = q2 + 1;
            }
        } else {
            if (err) *err = "refs must be a string array (line " + std::to_string(line_no) + ")";
            return false;
        }
    }
    return true;
}

CookResult cook_incremental(const std::string& src_dir, const std::string& out_dir) {
    CookResult result;
    std::string err;

    Manifest manifest;
    read_manifest(manifest_path(out_dir), manifest, &err); // absence is fine

    auto ids = scan_sources(src_dir);
    result.unit_ids = ids;

    bool manifest_changed = false;
    for (const auto& id : ids) {
        std::string src = (std::filesystem::path(src_dir) / id).string();
        Bytes source;
        if (!read_file_bytes(src, source, &err)) {
            result.error = err;
            ++result.counts.failures;
            continue;
        }
        auto sha = sha256(source);

        auto it = manifest.find(id);
        if (it != manifest.end() && it->second.source_sha == sha &&
            std::filesystem::exists(unit_path(out_dir, id))) {
            ++result.counts.skipped; // content hash unchanged -> no re-cook
            continue;
        }

        Unit unit;
        unit.id = id;
        unit.type = unit_type_for(id);
        unit.payload = std::move(source);

        ManifestEntry entry;
        entry.id = id;
        entry.source_sha = sha;

        if (unit.type == kUnitDef0) {
            DefData def;
            bool def_ok = parse_def(unit.payload, def, &err);
            if (!def_ok) {
                result.error = "def '" + id + "': " + err;
                ++result.counts.failures;
                continue;
            }
            std::set<std::string> id_set(ids.begin(), ids.end());
            for (const auto& ref : def.refs) {
                std::string dep_id;
                if (!resolve_ref(id, ref, dep_id)) {
                    result.error = "def '" + id + "' ref '" + ref + "' escapes the source root";
                    def_ok = false;
                    break;
                }
                if (!id_set.count(dep_id)) {
                    result.error = "def '" + id + "' references missing '" + dep_id + "'";
                    def_ok = false;
                    break;
                }
                entry.deps.push_back(dep_id);
            }
            if (!def_ok) {
                ++result.counts.failures;
                continue;
            }
            std::sort(entry.deps.begin(), entry.deps.end()); // deterministic graph bytes
        }

        if (!write_unit_file(unit_path(out_dir, id), unit, &err)) {
            result.error = err;
            ++result.counts.failures;
            continue;
        }
        manifest[id] = std::move(entry);
        manifest_changed = true;
        ++result.counts.cooked;
    }

    // Manifest bytes are deterministic (entries ordered by content ID).
    if (manifest_changed || !std::filesystem::exists(manifest_path(out_dir))) {
        if (!write_manifest(manifest_path(out_dir), manifest, &err)) {
            result.error = err;
            ++result.counts.failures;
        }
    }

    result.ok = result.counts.failures == 0;
    if (!result.ok && result.error.empty()) result.error = "cook failures occurred";
    return result;
}

void watch_loop(const std::string& src_dir, const std::string& out_dir,
                int interval_ms, const std::atomic<bool>& stop,
                CookCounts* totals_out) {
    while (!stop.load()) {
        auto r = cook_incremental(src_dir, out_dir);
        if (totals_out) {
            totals_out->cooked += r.counts.cooked;
            totals_out->skipped += r.counts.skipped;
            totals_out->failures += r.counts.failures;
        }
        // sleep in small slices so stop is responsive
        for (int slept = 0; slept < interval_ms && !stop.load(); slept += 10) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

} // namespace ve::cook

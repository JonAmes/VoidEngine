#include "ve/cook/cook.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <set>
#include <thread>

#include "internal.h"
#include "ve/cook/hulldef.h"
#include "ve/cook/hullvalidate.h"
#include "ve/cook/worlddef.h"
#include "ve/cook/wrldvalidate.h"
#include "font.h"
#include "glb.h"
#include "png.h"
#include "shader.h"

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
    if (ext == ".png") return kUnitTex0; // decoded to RGBA8 at cook time (0.7)
    if (ext == ".glb") return kUnitMesh; // parsed at cook time (0.8)
    if (ext == ".vert" || ext == ".frag" || ext == ".comp") return kUnitShdr; // SPIR-V at cook time (2.5)
    if (ext == ".ttf") return kUnitFont0; // glyph atlas at cook time (3.12)
    return kUnitRawD;
}

std::uint32_t ext_stage(const std::string& id) {
    auto ext = std::filesystem::path(id).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".frag") return kShdrStageFrag;
    if (ext == ".comp") return kShdrStageComp;
    return kShdrStageVert;
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


CookResult cook_incremental(const std::string& src_dir, const std::string& out_dir) {
    CookResult result;
    std::string err;

    Manifest manifest;
    read_manifest(manifest_path(out_dir), manifest, &err); // absence is fine

    auto ids = scan_sources(src_dir);
    result.unit_ids = ids;

    bool manifest_changed = false;
    std::set<std::string> cooked_ids;  // units (re)written this run; 5.9b trigger
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
        std::vector<std::string> mesh_tex_deps; // filled by the .glb cook (3.8)
        Unit sock_unit;                 // 5.6: companion SOCK unit for .glb sources
        bool has_sock = false;

        if (unit.type == kUnitTex0) {
            // PNG -> TEX0: one-time decode to RGBA8 at cook time. Payload:
            // u32 width | u32 height | u32 format | raw w*h*4 top-down.
            // Runtime loaders copy pixels; no decoder in the engine.
            int w = 0, h = 0;
            Bytes rgba;
            if (!decode_png_file(src, w, h, rgba, &err)) {
                result.error = "texture '" + id + "': " + err;
                ++result.counts.failures;
                continue;
            }
            auto put32 = [](Bytes& o, std::uint32_t v) {
                o.push_back(static_cast<std::uint8_t>(v & 0xFF));
                o.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
                o.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
                o.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
            };
            Bytes payload;
            put32(payload, static_cast<std::uint32_t>(w));
            put32(payload, static_cast<std::uint32_t>(h));
            put32(payload, kTexFmtRgba8);
            payload.insert(payload.end(), rgba.begin(), rgba.end());
            unit.payload = std::move(payload);
        } else if (unit.type == kUnitMesh) {
            // .glb -> MESH: hand-rolled GLB parse, TRS-baked meshes,
            // materials (texture refs -> content IDs, 3.8), hp_* socket
            // records (decision #14).
            Unit g, sk;
            if (!cook_glb_unit(source, id, g, sk, mesh_tex_deps, &err)) {
                result.error = "glb '" + id + "': " + err;
                ++result.counts.failures;
                continue;
            }
            unit.payload = std::move(g.payload);
            sock_unit = std::move(sk);  // 5.6: hull .glb also produces a SOCK unit
            has_sock = true;
        } else if (unit.type == kUnitFont0) {
            // .ttf -> FONT0: stb_truetype rasterization once at cook time
            // (#15: the runtime never parses fonts).
            if (!cook_font_unit(source, unit.payload, &err)) {
                result.error = "font '" + id + "': " + err;
                ++result.counts.failures;
                continue;
            }
        } else if (unit.type == kUnitShdr) {
            // GLSL -> SPIR-V once at cook time (decision #13: runtime never
            // compiles). Payload: u32 stage | SPIR-V bytes.
            const std::uint32_t stage = ext_stage(id);
            Bytes spirv;
            if (!compile_glsl(src, spirv, &err)) {
                result.error = "shader '" + id + "': " + err;
                ++result.counts.failures;
                continue;
            }
            Bytes payload;
            payload.push_back(static_cast<std::uint8_t>(stage & 0xFF));
            payload.push_back(static_cast<std::uint8_t>((stage >> 8) & 0xFF));
            payload.push_back(static_cast<std::uint8_t>((stage >> 16) & 0xFF));
            payload.push_back(static_cast<std::uint8_t>((stage >> 24) & 0xFF));
            payload.insert(payload.end(), spirv.begin(), spirv.end());
            unit.payload = std::move(payload);
        } else {
            unit.payload = std::move(source);
        }

        ManifestEntry entry;
        entry.id = id;
        entry.source_sha = sha;

        if (unit.type == kUnitMesh) {
            // material texture refs (3.8): dangling URIs fail the cook —
            // same doctrine as def refs. Edges are manifest-authoritative
            // (#13): the runtime load-closure walks mesh -> texture.
            const std::set<std::string> id_set(ids.begin(), ids.end());
            std::set<std::string> dep_set;
            bool deps_ok = true;
            for (const auto& dep : mesh_tex_deps) {
                if (!id_set.count(dep)) {
                    result.error = "glb '" + id + "' references missing '" + dep + "'";
                    deps_ok = false;
                    break;
                }
                dep_set.insert(dep); // dedup (same tex in both slots) + sorted bytes
            }
            if (!deps_ok) {
                ++result.counts.failures;
                continue;
            }
            entry.deps.assign(dep_set.begin(), dep_set.end());
        }

        if (unit.type == kUnitDef0) {
            // 5.8: hull defs compile to the normalized DEF0 v1 payload
            // (cooked.h); plain defs keep raw bytes; refs -> manifest deps.
            // 8.7 (#45d): a .toml declaring world_class is a WORLD def —
            // same source slot, different unit type (WRLD) and gates.
            const std::string text(unit.payload.begin(), unit.payload.end());
            std::vector<std::string> def_refs;
            std::string def_err;
            if (toml_source_has_world_class(text)) {
                unit.type = kUnitWrld;
                if (!cook_worlddef_source(text, unit.payload, def_refs, &def_err)) {
                    result.error = "world def '" + id + "': " + def_err;
                    ++result.counts.failures;
                    continue;
                }
            } else if (!cook_hulldef_unit(text, unit.payload, def_refs, &def_err)) {
                result.error = "def '" + id + "': " + def_err;
                ++result.counts.failures;
                continue;
            }
            bool def_ok = true;
            std::set<std::string> id_set(ids.begin(), ids.end());
            for (const auto& ref : def_refs) {
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
            // Dedup: a station's hull def and its build-plan modules can be
            // the same def (8.7) — one manifest edge per dep.
            entry.deps.erase(std::unique(entry.deps.begin(), entry.deps.end()),
                             entry.deps.end());
        }

        if (!write_unit_file(unit_path(out_dir, id), unit, &err)) {
            result.error = err;
            ++result.counts.failures;
            continue;
        }
        manifest[id] = std::move(entry);
        if (has_sock) {
            // Companion SOCK unit (5.6): same source hash (its lifecycle is
            // tied to the mesh cook), manifest edge sock -> mesh content ID.
            const std::string sid = id + ".sock";
            sock_unit.id = sid;
            if (!write_unit_file(unit_path(out_dir, sid), sock_unit, &err)) {
                result.error = err;
                ++result.counts.failures;
                continue;
            }
            ManifestEntry se;
            se.id = sid;
            se.source_sha = sha;
            se.deps = {id};
            manifest[sid] = std::move(se);
        }
        manifest_changed = true;
        cooked_ids.insert(id);
        if (has_sock) cooked_ids.insert(id + ".sock");
        ++result.counts.cooked;
    }

    // ---- 5.9b hull cross-validation (#44 gate list, #46 conventions) ----
    // Run for every DEF0 whose own or input units were (re)cooked this run;
    // reused units were validated in the run that cooked them. Gate order is
    // fixed inside cook_hull_validate (deterministic first-error). #46b: a
    // lattice snap that rewrote coords rewrites the affected SOCK unit.
    if (result.counts.failures == 0 && !cooked_ids.empty()) {
        // A cooked hull def is a DEF0 unit whose payload begins with the
        // payload-version u32 (2). Plain defs are the SAME unit type with raw
        // TOML bytes (5.8 passthrough) -- their first 4 bytes can never be
        // 0x00000002, so this peeks without parsing.
        auto is_hull_payload = [](const Bytes& p) {
            return p.size() >= 4 && p[0] == 2 && p[1] == 0 && p[2] == 0 && p[3] == 0;
        };
        auto load_hull = [&](const std::string& did, HullInputs& h, std::string* e) -> bool {
            Unit du;
            if (!read_unit_file(unit_path(out_dir, did), du, e) || du.type != kUnitDef0 ||
                !is_hull_payload(du.payload)) {
                *e = "def unit missing or not a cooked hull (DEF0 v2): " + did;
                return false;
            }
            h.def_id = did;
            if (!decode_def_payload(du.payload, h.def, e)) return false;
            for (const auto& dep : manifest[did].deps) {
                if (dep.size() < 4 || dep.compare(dep.size() - 4, 4, ".glb") != 0) continue;
                Unit su;
                if (!read_unit_file(unit_path(out_dir, dep + ".sock"), su, e) ||
                    su.type != kUnitSock) {
                    *e = "SOCK unit missing for mesh dep '" + dep + "' of '" + did + "'";
                    return false;
                }
                SockView sv;
                if (!decode_sock_payload(su.payload, sv, e)) return false;
                h.socks.push_back(std::move(sv));
            }
            for (const auto& seam : h.def.seams) {
                std::string pid;  // authored ref was validated at cook time
                resolve_ref(did, seam.partner_ref, pid);
                h.seam_partner_ids.push_back(pid);
            }
            return true;
        };
        // Hulls to validate, in manifest (content-ID) order.
        std::vector<std::string> hull_ids;
        for (const auto& m : manifest) {
            Unit du;
            std::string e;
            if (!read_unit_file(unit_path(out_dir, m.first), du, &e) || du.type != kUnitDef0 ||
                !is_hull_payload(du.payload))
                continue;  // plain passthrough def: no hull gates (#46)
            bool touched = cooked_ids.count(m.first) != 0;
            if (!touched)
                for (const auto& dep : m.second.deps)
                    if (cooked_ids.count(dep) || cooked_ids.count(dep + ".sock")) {
                        touched = true;
                        break;
                    }
            if (touched) hull_ids.push_back(m.first);
        }
        for (const auto& did : hull_ids) {
            HullInputs h;
            std::string verr;
            if (!load_hull(did, h, &verr)) {
                result.error = verr;
                ++result.counts.failures;
                break;
            }
            std::map<std::string, HullInputs> partners;
            bool load_ok = true;
            for (const std::string& pid : h.seam_partner_ids) {
                if (pid.empty() || partners.count(pid)) continue;
                HullInputs ph;
                if (!load_hull(pid, ph, &verr)) { load_ok = false; break; }
                partners[pid] = std::move(ph);
            }
            if (!load_ok) {
                result.error = verr;
                ++result.counts.failures;
                break;
            }
            std::vector<SockView> snapped;
            if (!cook_hull_validate(h, partners, snapped, &verr)) {
                result.error = verr;
                ++result.counts.failures;
                break;
            }
            for (std::size_t i = 0; i < h.socks.size() && i < snapped.size(); ++i) {
                const Bytes enc = encode_sock_payload(snapped[i]);
                const std::string sid = h.socks[i].mesh_id + ".sock";
                Unit su;
                if (!read_unit_file(unit_path(out_dir, sid), su, &err)) continue;
                if (su.payload == enc) continue;  // no snap change -> byte-stable
                su.payload = std::move(enc);
                if (!write_unit_file(unit_path(out_dir, sid), su, &err)) {
                    result.error = err;
                    ++result.counts.failures;
                    break;
                }
                cooked_ids.insert(sid);
            }
            if (result.counts.failures) break;
        }

        // ---- 8.8 world-def cross-field gates (#45d) ----
        // Touched WRLD units (own or dep cooked this run) in manifest
        // (content-ID) order; gate order is fixed inside cook_wrld_validate
        // (deterministic first-error, like the hull pass above). World/hull
        // deps are loaded transitively; a missing dep is a GATE message,
        // never a crash.
        if (result.counts.failures == 0) {
            std::vector<std::string> wrld_ids;
            for (const auto& m : manifest) {
                Unit du;
                std::string e;
                if (!read_unit_file(unit_path(out_dir, m.first), du, &e) ||
                    du.type != kUnitWrld)
                    continue;
                bool touched = cooked_ids.count(m.first) != 0;
                if (!touched)
                    for (const auto& dep : m.second.deps)
                        if (cooked_ids.count(dep)) {
                            touched = true;
                            break;
                        }
                if (touched) wrld_ids.push_back(m.first);
            }
            const RefResolve resolve = [](const std::string& from_id,
                                          const std::string& ref,
                                          std::string& id_out) {
                return resolve_ref(from_id, ref, id_out);
            };
            for (const auto& wid : wrld_ids) {
                Unit du;
                std::string e;
                if (!read_unit_file(unit_path(out_dir, wid), du, &e)) {
                    result.error = e;
                    ++result.counts.failures;
                    break;
                }
                auto self_wd = game::decode_world(du.payload);
                if (!self_wd) {
                    result.error = "world def decode failed for '" + wid +
                                   "': " + self_wd.error();
                    ++result.counts.failures;
                    break;
                }
                // Transitive world-dep closure: parent chains + members.
                std::map<std::string, game::WorldDef> wdeps;
                wdeps.emplace(wid, *self_wd);
                std::vector<std::string> queue{wid};
                bool decode_bad = false;
                while (!queue.empty() && !decode_bad) {
                    const std::string cur_id = queue.back();
                    queue.pop_back();
                    const game::WorldDef& d = wdeps[cur_id];
                    std::vector<std::string> refs;
                    if (!d.parent_ref.empty()) refs.push_back(d.parent_ref);
                    for (const auto& mem : d.members) refs.push_back(mem);
                    for (const auto& ref : refs) {
                        std::string rid;
                        if (!resolve_ref(cur_id, ref, rid) || rid.empty() ||
                            wdeps.count(rid))
                            continue;  // gate reports unresolvable refs
                        Unit ru;
                        if (!read_unit_file(unit_path(out_dir, rid), ru, &e) ||
                            ru.type != kUnitWrld)
                            continue;  // missing / not a world def: gate message
                        auto rwd = game::decode_world(ru.payload);
                        if (!rwd) {
                            result.error = "world def decode failed for '" + rid +
                                           "': " + rwd.error();
                            decode_bad = true;
                            break;
                        }
                        wdeps.emplace(rid, *rwd);
                        queue.push_back(rid);
                    }
                }
                if (decode_bad) {
                    ++result.counts.failures;
                    break;
                }
                // Hull-dep closure for stations: hull def + plan modules.
                std::map<std::string, HullInputs> hulls;
                if (self_wd->world_class == game::WorldClass::station) {
                    auto add_hull = [&](const std::string& ref) {
                        std::string hid;
                        if (ref.empty() || !resolve_ref(wid, ref, hid) ||
                            hid.empty() || hulls.count(hid))
                            return;
                        HullInputs h;
                        std::string he;
                        if (!load_hull(hid, h, &he)) return;  // gate reports the miss
                        hulls.emplace(hid, std::move(h));
                    };
                    add_hull(self_wd->hull_def_ref);
                    for (const auto& p : self_wd->build_plans)
                        for (const auto& s : p.steps) add_hull(s.module_def_ref);
                }
                std::string verr;
                if (!cook_wrld_validate(wid, *self_wd, wdeps, hulls, resolve, &verr)) {
                    result.error = verr;
                    ++result.counts.failures;
                    break;
                }
            }
        }
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

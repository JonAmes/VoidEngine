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

    // ---- 9.1 (#52a, decision #55): component TYPE def pre-scan ----
    // A type def's [attrs] preset merges INTO the hull DEF0 payload, so a
    // hull's cook identity must include the sources of the type defs its
    // entries use — a preset edit re-merges the hull instead of leaving a
    // stale payload. Pre-read the .toml sources (content-ID order, so the
    // FIRST declarer of a component_type owns it deterministically; parse
    // errors surface once in the main loop's unit cook), then pre-parse the
    // hull defs to learn their type-def deps + effective entry art refs.
    std::map<std::string, std::string> toml_texts;  // id -> current source text
    for (const auto& id : ids) {
        if (unit_type_for(id) != kUnitDef0) continue;
        Bytes data;
        if (!read_file_bytes((std::filesystem::path(src_dir) / id).string(), data, &err))
            continue;  // read errors surface in the main loop
        std::string text(data.begin(), data.end());
        if (toml_source_has_world_class(text)) continue;  // WRLD defs: untouched
        toml_texts.emplace(id, std::move(text));
    }
    std::map<std::string, std::pair<std::string, CompTypeDef>> comptypes;  // type -> (owner id, def)
    for (const auto& [id, text] : toml_texts) {
        if (!toml_source_has_component_type(text)) continue;
        CompTypeDef td;
        std::string te;
        if (cook_comptype_source(text, td, &te))
            comptypes.try_emplace(td.type, id, std::move(td));
    }
    std::map<std::string, CompTypeDef> type_view;  // type -> def (for hull merge)
    for (const auto& [t, own] : comptypes) type_view.emplace(t, own.second);
    // hull id -> sorted unique type-def SOURCE ids it consumes (effective-sha
    // input below); parse-failed hulls are skipped here (main loop reports).
    std::map<std::string, std::vector<std::string>> hull_typedeps;
    // 9.2 (#52b): the parsed hull set drives the two cook passes (below)
    // and the art/mount resolution stage; requests carry each entry's OWN
    // refs override, hull_refs the top-level authored refs (SOCK sources).
    std::set<std::string> hull_def_set;
    std::map<std::string, std::vector<CompArtRequest>> hull_reqs;
    std::map<std::string, std::vector<std::string>> hull_refs;
    for (const auto& [id, text] : toml_texts) {
        if (toml_source_has_component_type(text)) continue;
        Bytes dummy;
        std::vector<std::string> dummy_refs, used_types;
        std::string pe;
        if (!cook_hulldef_unit(text, dummy, dummy_refs, &pe, &type_view, &used_types))
            continue;
        // Hull-only check: a cooked hull payload starts with the version u32
        // (4 as of #51); plain-def passthrough bytes can never be 0x00000004
        // (5.9b peek). Keep in lockstep with kPayloadVersion in hulldef.cpp.
        if (!(dummy.size() >= 4 && dummy[0] == 4 && dummy[1] == 0 && dummy[2] == 0 && dummy[3] == 0))
            continue;
        hull_def_set.insert(id);
        hull_refs.emplace(id, std::move(dummy_refs));
        std::vector<CompArtRequest> reqs;
        std::string qe;
        if (cook_hulldef_comp_requests(text, reqs, &qe))
            hull_reqs.emplace(id, std::move(reqs));
        std::set<std::string> dep_ids;
        for (const auto& t : used_types) {
            auto o = comptypes.find(t);
            if (o != comptypes.end()) dep_ids.insert(o->second.first);
        }
        if (!dep_ids.empty())
            hull_typedeps.emplace(id, std::vector<std::string>(dep_ids.begin(), dep_ids.end()));
    }

    // ---- 9.2 (#52b): hull art resolution (runs between the cook passes) ----
    // A component's art candidates are its OWN refs (authored against the
    // hull's dir) or, when absent, its component TYPE def's refs (authored
    // against the TYPE def's dir — re-resolving them against the hull dir
    // would point at the wrong content). The first candidate asset whose
    // model carries a node NAMED by the component id or its type id (#52c
    // keyed models) is the resolved art: its content ID lands in the DEF0
    // v3 art_ref AND joins the hull's manifest deps (per-component edge).
    // Mount TRS comes from the merged SOCK anchors of every authored .glb
    // ref. Resolution reads SOURCES (always on disk) plus .sock units from
    // pass 1 — hull defs therefore cook in the SECOND pass below.
    std::map<std::string, std::map<std::string, std::string>> hull_art;       // def -> comp -> art id
    std::map<std::string, std::map<std::string, MountTRS>> hull_mounts;       // def -> anchor -> TRS
    std::map<std::string, std::vector<std::string>> hull_art_deps;            // def -> art dep edges
    std::map<std::string, std::vector<std::string>> art_key_cache;            // glb id -> node names
    std::set<std::string> art_key_reject;  // not a .glb / unreadable / unparsable
    auto keyed_names = [&](const std::string& aid) -> const std::vector<std::string>* {
        if (art_key_reject.count(aid)) return nullptr;
        if (auto it = art_key_cache.find(aid); it != art_key_cache.end())
            return &it->second;
        if (aid.size() < 4 || aid.compare(aid.size() - 4, 4, ".glb") != 0) {
            art_key_reject.insert(aid);
            return nullptr;
        }
        Bytes src;
        std::string ne;
        if (!read_file_bytes((std::filesystem::path(src_dir) / aid).string(), src, &ne) ||
            !glb_node_names(src, art_key_cache[aid], &ne)) {
            art_key_reject.insert(aid);
            return nullptr;
        }
        return &art_key_cache[aid];
    };
    auto resolve_hull_art_and_mounts = [&] {
        for (const auto& did : hull_def_set) {  // content-ID order: deterministic
            std::vector<std::string> glb_deps;
            auto add_glb = [&](const std::string& basis, const std::string& ref) {
                std::string aid;
                if (!resolve_ref(basis, ref, aid) || aid.empty()) return;
                if (aid.size() < 4 || aid.compare(aid.size() - 4, 4, ".glb") != 0) return;
                glb_deps.push_back(aid);
            };
            for (const auto& r : hull_refs[did]) add_glb(did, r);
            for (const auto& req : hull_reqs[did]) {
                for (const auto& r : req.own_refs) add_glb(did, r);
                if (req.own_refs.empty())
                    if (auto td = comptypes.find(req.type); td != comptypes.end())
                        for (const auto& r : td->second.second.refs)
                            add_glb(td->second.first, r);
            }
            std::sort(glb_deps.begin(), glb_deps.end());
            glb_deps.erase(std::unique(glb_deps.begin(), glb_deps.end()), glb_deps.end());
            std::map<std::string, MountTRS> anchors;
            for (const auto& gid : glb_deps) {
                Unit su;
                std::string se;
                if (!read_unit_file(unit_path(out_dir, gid + ".sock"), su, &se) ||
                    su.type != kUnitSock)
                    continue;  // missing SOCK: G2/G11 report it, no cook-crash
                SockView sv;
                if (!decode_sock_payload(su.payload, sv, &se)) continue;
                for (const auto& a : sv.anchors) {
                    MountTRS m;
                    m.t = a.t;
                    m.q = a.q;
                    anchors.try_emplace(a.name, m);  // first-declared wins
                }
            }
            std::map<std::string, std::string> art;
            std::vector<std::string> edges;
            for (const auto& req : hull_reqs[did]) {
                const std::vector<std::string>* refs = &req.own_refs;
                std::string basis = did;
                if (refs->empty()) {
                    auto td = comptypes.find(req.type);
                    if (td == comptypes.end()) continue;  // no type def: G11 cooks out
                    basis = td->second.first;             // refs are type-def-relative
                    refs = &td->second.second.refs;
                }
                for (const auto& r : *refs) {
                    std::string aid;
                    if (!resolve_ref(basis, r, aid) || aid.empty()) continue;
                    const auto* names = keyed_names(aid);
                    if (!names) continue;
                    bool keyed = false;
                    for (const auto& n : *names)
                        if (n == req.id || n == req.type) { keyed = true; break; }
                    if (keyed) {  // first keyed candidate, authored order
                        art.emplace(req.id, aid);
                        edges.push_back(aid);
                        break;
                    }
                }
            }
            std::sort(edges.begin(), edges.end());
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
            if (!art.empty()) hull_art.emplace(did, std::move(art));
            if (!anchors.empty()) hull_mounts.emplace(did, std::move(anchors));
            if (!edges.empty()) hull_art_deps.emplace(did, std::move(edges));
        }
    };

    // 9.2 (#52b) two passes: non-hull sources in content-ID order first,
    // hull defs LAST (their DEF0 v3 art fields read pass-1 units; an
    // incrementally SKIPPED dep keeps its prior-run unit on disk, so a
    // touched hull always sees current dep bytes).
    for (std::size_t pass = 0; pass < 2; ++pass) {
        if (pass == 1) resolve_hull_art_and_mounts();
        for (const auto& id : ids) {
        if ((hull_def_set.count(id) != 0) != (pass == 1)) continue;
        std::string src = (std::filesystem::path(src_dir) / id).string();
        Bytes source;
        if (!read_file_bytes(src, source, &err)) {
            result.error = err;
            ++result.counts.failures;
            continue;
        }
        auto sha = sha256(source);
        // #55: a hull def's stored identity = its own bytes + the bytes of
        // every component type def it uses, so a preset edit re-cooks (and
        // re-merges) the hull even when the hull source is untouched.
        if (auto hd = hull_typedeps.find(id); hd != hull_typedeps.end()) {
            Bytes joined = source;
            for (const auto& tid : hd->second) {
                const std::string& ttext = toml_texts[tid];
                joined.insert(joined.end(), ttext.begin(), ttext.end());
            }
            sha = sha256(joined);
        }

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
        Bytes tubes_payload;            // #51c: companion tube MESH for hull defs
        bool has_tubes = false;

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
            } else if (toml_source_has_component_type(text)) {
                // #55: the type def validates, then keeps its raw source
                // bytes (no new payload kind — the merge resolves into hull
                // DEF0 payloads; hull effective shas cover this source, so a
                // merged hull can never go stale). refs -> manifest deps.
                CompTypeDef td;
                if (!cook_comptype_source(text, td, &def_err)) {
                    result.error = "component type def '" + id + "': " + def_err;
                    ++result.counts.failures;
                    continue;
                }
                if (auto owner = comptypes.find(td.type);
                    owner != comptypes.end() && owner->second.first != id) {
                    result.error = "component_type '" + td.type +
                                   "' declared by both '" + owner->second.first +
                                   "' and '" + id + "' (#55)";
                    ++result.counts.failures;
                    continue;
                }
                def_refs = td.refs;
            } else {
                std::vector<std::string> used_types, entry_refs;
                auto art_it = hull_art.find(id);
                auto anch_it = hull_mounts.find(id);
                if (!cook_hulldef_unit(text, unit.payload, def_refs, &def_err,
                                       &type_view, &used_types, &entry_refs,
                                       art_it == hull_art.end() ? nullptr : &art_it->second,
                                       anch_it == hull_mounts.end() ? nullptr : &anch_it->second,
                                       &tubes_payload)) {
                    result.error = "def '" + id + "': " + def_err;
                    ++result.counts.failures;
                    continue;
                }
                has_tubes = true;  // #51c: emit the <def id>.wires sibling unit
                // #52b edges: hull -> type defs it merged with (IDs already
                // resolved; inherited art reaches the hull transitively
                // through this edge) + each entry's OWN art refs override
                // (#55b) joined into def_refs for the missing-ref gate.
                for (const auto& t : used_types)
                    if (auto o = comptypes.find(t); o != comptypes.end())
                        entry.deps.push_back(o->second.first);
                def_refs.insert(def_refs.end(), entry_refs.begin(), entry_refs.end());
                // #52b per-component art dep edges: RESOLVED content IDs —
                // they join entry.deps directly (never re-resolved as refs).
                if (auto ae = hull_art_deps.find(id); ae != hull_art_deps.end())
                    entry.deps.insert(entry.deps.end(), ae->second.begin(), ae->second.end());
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
        if (has_tubes) {
            // #51c companion tube MESH unit (wire visuals): thin tubes cooked
            // from the wire routes — one submesh per wire, name = wire id,
            // so the render side mounts/dims each tube by reachability
            // ("dark not dead"). Same source-hash lifecycle as the .sock
            // sibling; manifest edge wires -> hull def (derived from DEF0).
            const std::string wid = id + ".wires";
            Unit wu;
            wu.id = wid;
            wu.type = kUnitMesh;
            wu.payload = std::move(tubes_payload);
            if (!write_unit_file(unit_path(out_dir, wid), wu, &err)) {
                result.error = err;
                ++result.counts.failures;
                continue;
            }
            ManifestEntry we;
            we.id = wid;
            we.source_sha = sha;
            we.deps = {id};
            manifest[wid] = std::move(we);
        }
        manifest_changed = true;
        cooked_ids.insert(id);
        if (has_sock) cooked_ids.insert(id + ".sock");
        if (has_tubes) cooked_ids.insert(id + ".wires");
        ++result.counts.cooked;
        }
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
            return p.size() >= 4 && p[0] == 4 && p[1] == 0 && p[2] == 0 && p[3] == 0;
        };
        auto load_hull = [&](const std::string& did, HullInputs& h, std::string* e) -> bool {
            Unit du;
            if (!read_unit_file(unit_path(out_dir, did), du, e) || du.type != kUnitDef0 ||
                !is_hull_payload(du.payload)) {
                *e = "def unit missing or not a cooked hull (DEF0 v3): " + did;
                return false;
            }
            h.def_id = did;
            // #52b: manifest deps feed gate G11 (resolved art must ride a
            // per-component dep edge). Skipped hulls keep the manifest's
            // previous-run edges; fresh hulls got theirs this run.
            if (auto mit = manifest.find(did); mit != manifest.end())
                h.deps = mit->second.deps;
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

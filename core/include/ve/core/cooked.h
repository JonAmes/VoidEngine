#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Cooked-artifact binary format (decision #13). This lives in core/ because
// the runtime loader reads exactly these bytes: cooked binary is the engine's
// native data format (the doctrine forbids *text* parsing at runtime, not
// loading cooked units). The cooklet produces them; core consumes them; both
// share one codec so cook/load can never drift apart.
namespace ve::core::cooked {

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
inline constexpr std::uint32_t kUnitSock = fourcc("SOCK"); // cooked socket table (5.6)
inline constexpr std::uint32_t kUnitMat0 = fourcc("MAT0"); // cooked material (0.8)
inline constexpr std::uint32_t kUnitDef0 = fourcc("DEF0"); // normalized def
inline constexpr std::uint32_t kUnitShdr = fourcc("SHDR"); // cooked SPIR-V (2.5)
inline constexpr std::uint32_t kUnitFont0 = fourcc("FONT0"); // cooked glyph atlas (3.12)
inline constexpr std::uint32_t kUnitWrld = fourcc("WRLD"); // cooked world def (8.7, #45d)

// SOCK payload (5.6, v2 since 5.9a #46a): the hull's ONE named-anchor table —
// hardpoint sockets AND #30a interior empties (door_/seat_/panel_/counter_/
// berth_ prefixes), authored local TRS, node-index order (deterministic):
//   str mesh_unit_id, u32 payload_ver=2, u32 count,
//   entries: str name, u8 kind [0 hp_* socket / 1 door_ / 2 seat_ / 3 panel_
//     / 4 counter_ / 5 berth_], u8 category (ratified 0..3; interior anchors
//     are always 3 utility), 3*f64 translation, 4*f64 quaternion.
// 5.9b cross-validation SNAPS lattice coordinates into this payload in place
// (#46b) — runtime sees only perfect lattice coords for anchor positions.
// The name is the pairing key against def component ids (#46a).

// DEF0 payload (5.8): a .toml def declaring a top-level `class_tag` is a
// HULL DEF; the payload is the normalized binary v2 (LE, same doctrine;
// v2 = facet target field + seams, decision #46):
//   u32 payload_ver=2, str class_tag,
//   u8 has_hull_mass f64 | u8 has_com 3*f64 | u8 has_inertia_diag 3*f64,
//   grid {f64 cell_m, u32 subcell_ratio, f64 cell_height_m, f64 tolerance_m},
//   u32 sections (str name, u32 boxes, box = 6*i32 lo.xyz then hi.xyz, CELL
//     coords),
//   u32 components (str id, str type, u8 category [0 main/1 rcs/2 gun/3
//     utility = core socket_category encoding], str socket,
//     u8 face [0 none, 1 +x, 2 -x, 3 +y, 4 -y, 5 +z, 6 -z; required for
//     door.*/airlock.* types, #46c1], u32 tags str*,
//     u32 attrs {str key, f64} in key order, u32 facets {str action,
//     f64 range_m, u8 manual, u8 has_target [str target]} (#28a cross-ref),)
//   u32 catalog (str type_id, u8 category; first-seen order),
//   u32 wires (str id, u8 type [0 power/1 signal/2 power+signal],
//     u8 has_capacity f64, u8 from_kind [0 socket/1 component] str from_ref,
//     u8 to_kind str to_ref),
//   u32 seams (str partner_def_ref, str socket, u8 has_offset 3*f64) (#46d;
//     partners also ride refs/manifest deps),
//   u32 refs (authored order; manifest deps resolve them separately).
// All #42 unit suffixes are already coerced to SI — the runtime never sees
// TOML. A def WITHOUT class_tag keeps its raw source bytes (non-hull defs).
inline constexpr std::uint32_t kCookFormat = 6; // v6: WRLD world-def payload class (8.7 #45d); v5: DEF0 hull-def binary + SOCK v2 one named-anchor table (#46); v4: .glb cook gained SOCK unit + strict socket_type (#21); v3: MAT0 texture refs (3.8); v2: MESH tan (3.6)

// SHDR payload: u32 stage (kShdrStage*) | SPIR-V binary as cooked by
// glslangValidator at cook time. Runtime never compiles GLSL (decision #13)
// and never parses source — vkCreateShaderModule gets these bytes verbatim.
inline constexpr std::uint32_t kShdrStageVert = 0;
inline constexpr std::uint32_t kShdrStageFrag = 1;
inline constexpr std::uint32_t kShdrStageComp = 2;

// TEX0 payload: u32 width | u32 height | u32 format | raw pixels (top-down).

inline constexpr std::uint32_t kTexFmtRgba8 = 0;

// FONT0 payload (3.12): .ttf rasterized ONCE at cook time via stb_truetype
// (cooklet-side only; #15 "zero font parsing at runtime"). Cook px size and
// codepoint range are fixed (see tools/cook/src/font.h). All little-endian:
//   u32 px_size | u32 atlas_w | u32 atlas_h |
//   i32 ascent | i32 descent | u32 first_cp | u32 glyph_count,
//   then glyph_count * { i32 atlas_x, atlas_y, w, h,  // atlas rect; w==h==0 = blank
//                        i32 bearing_x, bearing_y, f32 advance },
//   then atlas_w*atlas_h*4 RGBA bytes (top-down; glyph coverage replicated to
//   R=G=B=A — subpixel-free grayscale mask). Codepoint = first_cp + index.

// MESH payload (cooked hull .glb, decision #14; all little-endian):
//   u32 mesh_count, then per mesh (node TRS baked in, triangles only):
//     u32 name_len, name | u32 material_index (0xFFFFFFFF = none)
//     u32 vertex_count | f32 pos[vc*3] | f32 nrm[vc*3] | f32 uv[vc*2]
//     f32 tan[vc*4]  // MikkTSpace bake, w = bOrient (+1/-1);
//                    // reconstruction: bitangent = cross(normal, tangent) * w
//     u32 tri_count | u32 idx[tc*3]
//   u32 material_count, then per material (MAT0 block; consumed by 3.10
//   shading — the loader walks it for integrity):
//     u32 name_len, name | f32 base_color[4] | f32 metallic | f32 roughness
//     f32 emissive[3] (KHR emissive_strength folded in)
//     u32 alpha_mode (0 opaque, 1 blend, 2 mask)  // doubleSided dropped:
//                                                   the cooklet owns culling
//     u32 basecolor_tex_len, content-ID bytes ('' = none)   // 3.8: textures
//     u32 mr_tex_len, content-ID bytes ('' = none)          // are TEX0 units
//     // referenced ids also appear as manifest dep edges (mesh -> texture);
//     // only EXTERNAL glTF image URIs are accepted — every texture is its
//     // own source file with its own content ID and hash
//   u32 socket_count, then per socket (mesh-less hp_* node, decision #14):
//     u32 name_len, name | f32 translation[3] (world, TRS-baked)
//     u32 extras_len, extras as stable JSON bytes (keys sorted)
//   u32 root_extras_len, root-node extras JSON bytes (hull_class, mass_t...)

// SOCK payload (5.6; one per hull .glb, unit id = <glb id>.sock; the
// runtime hardpoint table — the MESH socket section above is debug/author
// verbatim, SOCK is authoritative). Categories are the RATIFIED set
// (decision #21): 0=main, 1=rcs, 2=gun, 3=utility; anything else fails
// the cook. All little-endian:
//   str mesh_content_id (link back to the MESH unit) | u32 socket_count,
//   then per socket (node index order — deterministic):
//     str id (hp_* node name) | u8 category
//     f64 position[3] (world, TRS-baked) | f64 orientation[4] (x,y,z,w)

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

// ---- whole-file byte IO (cooklet + loaders both need these) -----------------
bool read_file_bytes(const std::string& path, Bytes& out, std::string* err);
bool write_file_bytes(const std::string& path, const Bytes& data, std::string* err);

// ---- hashes -----------------------------------------------------------------
// sha256/crc32 are hand-rolled and guarded by FIPS known vectors
// (tests/tools/test_container.cpp: hash_known_vectors). Self-consistency is
// NOT sufficient — a wrong constant table still round-trips.
std::array<std::uint8_t, 32> sha256(const Bytes& data);
std::uint32_t crc32(const Bytes& data);

} // namespace ve::core::cooked

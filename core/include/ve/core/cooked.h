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
inline constexpr std::uint32_t kUnitMat0 = fourcc("MAT0"); // cooked material (0.8)
inline constexpr std::uint32_t kUnitDef0 = fourcc("DEF0"); // normalized def
inline constexpr std::uint32_t kUnitShdr = fourcc("SHDR"); // cooked SPIR-V (2.5)

inline constexpr std::uint32_t kCookFormat = 2; // v2: MESH payload gained tan[vc*4] (3.6)

// SHDR payload: u32 stage (kShdrStage*) | SPIR-V binary as cooked by
// glslangValidator at cook time. Runtime never compiles GLSL (decision #13)
// and never parses source — vkCreateShaderModule gets these bytes verbatim.
inline constexpr std::uint32_t kShdrStageVert = 0;
inline constexpr std::uint32_t kShdrStageFrag = 1;
inline constexpr std::uint32_t kShdrStageComp = 2;

// TEX0 payload: u32 width | u32 height | u32 format | raw pixels (top-down).
inline constexpr std::uint32_t kTexFmtRgba8 = 0;

// MESH payload (cooked hull .glb, decision #14; all little-endian):
//   u32 mesh_count, then per mesh (node TRS baked in, triangles only):
//     u32 name_len, name | u32 material_index (0xFFFFFFFF = none)
//     u32 vertex_count | f32 pos[vc*3] | f32 nrm[vc*3] | f32 uv[vc*2]
//     f32 tan[vc*4]  // MikkTSpace bake, w = bOrient (+1/-1);
//                    // reconstruction: bitangent = cross(normal, tangent) * w
//     u32 tri_count | u32 idx[tc*3]
//   u32 material_count, then per material:
//     u32 name_len, name | f32 base_color[4] | f32 metallic | f32 roughness
//     f32 emissive[3] (KHR emissive_strength folded in)
//     u32 alpha_mode (0 opaque, 1 blend, 2 mask)  // doubleSided dropped:
//                                                   the cooklet owns culling
//   u32 socket_count, then per socket (mesh-less hp_* node, decision #14):
//     u32 name_len, name | f32 translation[3] (world, TRS-baked)
//     u32 extras_len, extras as stable JSON bytes (keys sorted)
//   u32 root_extras_len, root-node extras JSON bytes (hull_class, mass_t...)

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

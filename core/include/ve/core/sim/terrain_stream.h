#pragma once
// Phase 8 (8.17, #29/#13): streamed display-mesh gen + seeded scatter for
// the analytic terrain (7.2). NOT sim physics — pure deterministic mesh/
// instance gen that sim (contacts use the sampler directly) and renderer
// (this module) can both drive from the same terrain_def + stamps, so no
// baked terrain data exists anywhere (#29a). The chunk lattice is a
// quadtree over the six cube faces of 7.2's face chart; the morph-skirt
// rule below is what makes adjacent LODs crack-free without stitching
// passes. The biome/palette table (#29d, def data on the world def) rides
// as ordered `surface_band` rows: first match wins, same hysteresis-free
// band contract as the 6.9 reservoirs / 20c reputation / 8.15 LOD bands.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "ve/core/math/vec.h"
#include "ve/core/sim/terrain.h"

namespace ve::core::sim {

// Chunk addressing: level L subdivides each face into 2^L x 2^L quads in
// face UV space; x indexes u, y indexes v (both in [0, 2^L)). Face code
// matches cube_face: 2*axis + (dominant negative), axis X=0 Y=1 Z=2.
struct chunk_key {
    std::uint8_t face = 0;
    std::uint8_t level = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
};
constexpr bool operator==(const chunk_key& a, const chunk_key& b) {
    return a.face == b.face && a.level == b.level && a.x == b.x && a.y == b.y;
}

// Fixed mesh granularity per chunk (determinism + trivially shared edges:
// adjacent chunks sample the SAME face-UV grid lines, 4 cells per edge).
constexpr int kChunk_cells = 4;
constexpr int kChunk_verts = kChunk_cells + 1;

// Skirt control (#13): per-edge delta in quadtree levels of the SELECTED
// neighbor. 0 => same level (shared samples already agree); k >= 1 => the
// neighbor is k levels coarser and this edge's BETWEEN-knot vertices get
// morphed (see gen_chunk). Edge order: 0 low-u, 1 high-u, 2 low-v, 3 high-v.
struct chunk_edges {
    std::array<std::int8_t, 4> neighbor_delta = {0, 0, 0, 0};
};

// One vertex: unit dir (face-UV unprojection, 7.2 chart) + relief height
// above mean radius. The renderer composes R + h itself; triangulation of
// the fixed kChunk_verts x kChunk_verts row-major grid is left implicit.
struct chunk_vertex {
    math::dvec3 dir{0.0, 1.0, 0.0};
    double height_m = 0.0;
};
struct chunk_mesh {
    chunk_key key{};
    std::array<chunk_vertex, kChunk_verts * kChunk_verts> verts{};
};

// Generate one chunk from the SHARED sampler (same terrain_height the sim
// uses — renderer and sim cannot disagree, 7.2 rule). Pure: bit-identical
// for identical inputs, any time, anywhere. Skirt rule: vertices that sit
// at a coarser neighbor's sample positions ("knots", every 2^delta-th
// vertex along that edge) keep the EXACT sampled height — hence bit-match
// the coarse neighbor's shared vertex; the between-knot vertices are lerped
// along the knot pair. The two LOD polylines then coincide pointwise along
// the whole shared edge: no cracks, no stitching pass.
chunk_mesh gen_chunk(const terrain_def& def, std::span<const terrain_stamp> stamps,
                     const chunk_key& key, const chunk_edges& edges);

// Inverse of cube_face (exact round trip for in-range uv; out-of-range is
// clamped — chunk UVs never leave the face). Exposed so callers can
// resolve face coords (POIs, scatter jitter) to dirs.
math::dvec3 face_unproject(int face, double u, double v);

// Biome / palette band row (#29d): the world def's biome table maps to
// these rows (color/roughness resolution stays render-side via the index;
// scatter density rides here because it is def data). Ordered: FIRST row
// whose [lo_m, hi_m] contains the surface height wins; no match = bare.
struct surface_band {
    double lo_m = 0.0;
    double hi_m = 0.0;
    double scatter_per_km2 = 0.0;
    double max_slope_rad = 3.14159265358979;  // tilt of normal off radial
};

// Index of the first matching band, or -1 (bare).
int surface_band_at(std::span<const surface_band> bands, double height_m);

// Seeded scatter (#20d seed pattern; 8.17 "scatter instanced from seed +
// authored density tables within bounds"): deterministic lattice walk over
// the six faces at a fixed cell count per face edge. Per cell: hash bits
// accept the cell with probability min(1, band density x cell area), a
// second hash pair jitters the position, height + slope are then evaluated
// AT the jittered dir and the candidate must still sit inside its band's
// bounds (height window, max slope) — "within authored bounds" is a proof,
// not a statistical wish. Deterministic ORDER: face ascending, x ascending,
// y ascending (ordered containers only, like all sim iteration).
struct scatter_params {
    std::uint64_t seed = 0;
    std::uint32_t cells_per_face_edge = 16;  // lattice granularity (>= 1)
};
struct scatter_instance {
    math::dvec3 dir{0.0, 1.0, 0.0};
    double height_m = 0.0;
    std::int32_t band = -1;
    std::uint32_t variant = 0;  // hash-picked art variant (0..3)
};
std::vector<scatter_instance> scatter_field(const terrain_def& def,
                                            std::span<const terrain_stamp> stamps,
                                            double mean_radius_m,
                                            std::span<const surface_band> bands,
                                            const scatter_params& params);

}  // namespace ve::core::sim

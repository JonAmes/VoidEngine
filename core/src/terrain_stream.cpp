// Phase 8 (8.17): streamed chunk gen + seeded scatter. See terrain_stream.h.
// Determinism is load-bearing: gen_chunk/scatter_field feed render-side
// caches keyed by content; the tests pin bit-identical regeneration.

#include "ve/core/sim/terrain_stream.h"

#include <cmath>
#include <vector>

#include "ve/core/math/vec.h"

namespace ve::core::sim {
namespace {

// Same mixing as terrain.cpp's noise lattice (independent copy on purpose:
// scatter streams must not perturb sampler evolution).
constexpr std::uint64_t splitmix64(std::uint64_t z) {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

constexpr double kPi = 3.14159265358979323846;

// Uniform mapping of [0,1) from the high bits (double has 53-bit mantissa).
constexpr double unit(std::uint64_t h) {
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

}  // namespace

math::dvec3 face_unproject(int face, double u, double v) {
    const auto cl = [](double t) {
        return t < -1.0 ? -1.0 : (t > 1.0 ? 1.0 : t);
    };
    u = cl(u);
    v = cl(v);
    const int axis = face / 2;
    const double s = (face & 1) ? -1.0 : 1.0;
    // Must mirror cube_face exactly (terrain.cpp): face code 2*axis + neg,
    // u/v are the ratios of the non-dominant coords over |dominant| in the
    // component order fixed there. Verified by the round-trip test.
    math::dvec3 raw;
    switch (axis) {
    case 0: raw = {s, u, v}; break;   // X dominant: u=Y, v=Z (cube_face order)
    case 1: raw = {v, s, u}; break;   // Y dominant: u=Z, v=X ((axis+1)%3 cyclic)
    default: raw = {u, v, s}; break;  // Z dominant: u=X, v=Y
    }
    return math::normalize(raw);
}

chunk_mesh gen_chunk(const terrain_def& def, std::span<const terrain_stamp> stamps,
                     const chunk_key& key, const chunk_edges& edges) {
    chunk_mesh out;
    out.key = key;
    const double span = 2.0 / static_cast<double>(1u << key.level);  // face-UV width
    const double step = span / kChunk_cells;
    const double u0 = -1.0 + key.x * span;
    const double v0 = -1.0 + key.y * span;

    for (int iy = 0; iy < kChunk_verts; ++iy) {
        for (int ix = 0; ix < kChunk_verts; ++ix) {
            const double u = u0 + ix * step;
            const double v = v0 + iy * step;
            chunk_vertex& vert = out.verts[static_cast<std::size_t>(iy) * kChunk_verts + ix];
            vert.dir = face_unproject(key.face, u, v);

            // Exact sampled height everywhere first; skirt edges then
            // overwrite the BETWEEN-knot vertices only.
            vert.height_m = terrain_height(def, stamps, vert.dir);

            // Which skirt rule applies to this vertex (if any): a vertex on
            // an edge with neighbor_delta k >= 1 morphs toward its neighbor
            // ALONG that edge whenever its index is not a multiple of
            // 2^k (i.e. it is not a knot shared with the coarse neighbor).
            // Corner vertices on two skirt edges are handled u-first, then
            // v: the order is part of the published contract (pure).
            auto skirt = [&](bool along_u, int edge) {
                const std::int8_t delta = edges.neighbor_delta[edge];
                if (delta <= 0)
                    return;
                const bool on_edge = along_u ? (iy == 0 || iy == kChunk_cells)
                                             : (ix == 0 || ix == kChunk_cells);
                if (!on_edge)
                    return;
                const int idx = along_u ? ix : iy;
                const int stride = 1 << delta;  // coarse-neighbor sample spacing
                if (idx % stride == 0)
                    return;  // knot: exact height already matches bit-for-bit
                const int lo = idx - (idx % stride);
                const int hi = lo + stride;
                const double t = static_cast<double>(idx - lo) / static_cast<double>(stride);
                const auto at = [&](int i) {
                    const double mu = along_u ? u0 + i * step : u0 + ix * step;
                    const double mv = along_u ? v0 + iy * step : v0 + i * step;
                    return terrain_height(def, stamps, face_unproject(key.face, mu, mv));
                };
                vert.height_m = at(lo) + t * (at(hi) - at(lo));
            };
            skirt(true, (iy == 0) ? 2 : 3);   // horizontal edges: low-v / high-v
            skirt(false, (ix == 0) ? 0 : 1);  // vertical edges: low-u / high-u
        }
    }
    return out;
}

int surface_band_at(std::span<const surface_band> bands, double height_m) {
    for (std::size_t i = 0; i < bands.size(); ++i)
        if (height_m >= bands[i].lo_m && height_m <= bands[i].hi_m)
            return static_cast<int>(i);
    return -1;
}

std::vector<scatter_instance> scatter_field(const terrain_def& def,
                                            std::span<const terrain_stamp> stamps,
                                            double mean_radius_m,
                                            std::span<const surface_band> bands,
                                            const scatter_params& params) {
    std::vector<scatter_instance> out;
    if (bands.empty() || params.cells_per_face_edge == 0 || mean_radius_m <= 0.0)
        return out;
    const std::uint32_t n = params.cells_per_face_edge;
    const double du = 2.0 / n;
    const double cell_m2 = mean_radius_m * mean_radius_m * du * du;  // base area, scaled below

    for (int face = 0; face < 6; ++face) {
        for (std::uint32_t iy = 0; iy < n; ++iy) {
            for (std::uint32_t ix = 0; ix < n; ++ix) {
                const auto f64 = static_cast<std::uint64_t>(face);
                const auto y64 = static_cast<std::uint64_t>(iy);
                const auto x64 = static_cast<std::uint64_t>(ix);
                const std::uint64_t cell_hash = splitmix64(
                    params.seed + f64 * 0x9E3779B97F4A7C15ULL + y64 * 0xC2B2AE3D27D4EB4FULL + x64);
                const double u = -1.0 + (ix + unit(cell_hash)) * du;
                const double v = -1.0 + (iy + unit(splitmix64(cell_hash))) * du;
                const math::dvec3 dir = face_unproject(face, u, v);
                // Gnomonic solid-angle element on the unit sphere:
                // dOmega = du dv / (1 + u^2 + v^2)^(3/2); area = R^2 dOmega.
                const double area = cell_m2 / std::pow(1.0 + u * u + v * v, 1.5);
                const double height = terrain_height(def, stamps, dir);
                const int band = surface_band_at(bands, height);
                if (band < 0)
                    continue;
                const surface_band& row = bands[static_cast<std::size_t>(band)];
                // Density is per km^2 (WorldBiomeBand contract, #29d):
                // cell area (m^2) * rate / 1e6 = expected count per cell.
                const double expect = row.scatter_per_km2 * area / 1.0e6;
                if (expect <= 0.0)
                    continue;
                const std::uint64_t acc_hash = splitmix64(cell_hash ^ 0xD1B54A32D192ED03ULL);
                if (unit(acc_hash) > expect)
                    continue;
                // Bounds re-check AT the jittered dir (proof, not wish):
                // slope = angle between radial dir and the surface normal.
                const math::dvec3 normal = terrain_normal(def, mean_radius_m, stamps, dir);
                const double cos_tilt = math::dot(normal, dir);
                const double c = cos_tilt < -1.0 ? -1.0 : (cos_tilt > 1.0 ? 1.0 : cos_tilt);
                const double tilt = std::acos(c);
                if (tilt > row.max_slope_rad)
                    continue;
                out.push_back(scatter_instance{
                    .dir = dir,
                    .height_m = height,
                    .band = band,
                    .variant = static_cast<std::uint32_t>(
                        splitmix64(acc_hash ^ 0xA0761D6478BD642FULL) & 3u),
                });
            }
        }
    }
    return out;
}

}  // namespace ve::core::sim

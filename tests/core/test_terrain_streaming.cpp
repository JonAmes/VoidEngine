// Phase 8 task 8.17 — streamed terrain gen + seeded scatter (#29/#13).
// The chunk lattice is the six-face quadtree of 7.2's chart; the load-
// bearing properties: (a) knot vertices between LOD levels agree BIT-FOR-
// BIT with the shared sampler, so adjacent chunks of different levels
// share exact vertices; (b) morphed skirt vertices lie exactly ON the
// coarse polyline, so the two LOD polylines coincide along the whole
// shared edge (crack-free with no stitching pass); (c) gen is pure: the
// same inputs regenerate bit-identical meshes at any time; (d) scatter is
// seeded, deterministic in content AND order, and every instance sits
// inside its band's authored bounds by construction (checked here at the
// jittered dir, not assumed).

#include <cmath>
#include <cstdint>
#include <vector>

#include <ve/core/math/vec.h>
#include <ve/core/sim/terrain.h>
#include <ve/core/sim/terrain_stream.h>

#include <doctest/doctest.h>

using namespace ve::core;
using namespace ve::core::sim;

namespace {

terrain_def relief_def() {
    terrain_def def;
    def.seed = 0xB17E5EEDULL;
    def.relief_m = 300.0;
    def.base_frequency = 0.8;
    def.octaves = 2;
    def.lacunarity = 2.0;
    def.gain = 0.5;
    return def;
}

double hash_sum(const chunk_mesh& m) {
    double h = 0.0;
    for (const auto& v : m.verts)
        h = h * 31.0 + v.height_m * 4096.0 + (v.dir.x + v.dir.y * 2.0 + v.dir.z * 4.0) * 1024.0;
    return h;
}

double hash_sum(const std::vector<scatter_instance>& sc) {
    double h = static_cast<double>(sc.size());
    for (const auto& s : sc)
        h = h * 31.0 + s.height_m * 1024.0 + s.dir.x * 7.0 + s.dir.y * 13.0 + s.dir.z * 17.0 +
            static_cast<double>(s.band) + static_cast<double>(s.variant) * 0.25;
    return h;
}

}  // namespace

TEST_CASE("terrain_chunks") {
    const terrain_def def = relief_def();
    const double R = 500000.0;

    // ---- face_unproject round-trips cube_face exactly ----
    for (int face = 0; face < 6; ++face) {
        // Interior uv only: exact face-edge ties follow cube_face's axis-
        // preference rule (X > Y > Z), not this round trip.
        for (double u : {-0.9, -0.5, 0.0, 0.3}) {
            for (double v : {-0.9, -0.5, 0.0, 0.3}) {
                const math::dvec3 d = face_unproject(face, u, v);
                const face_uv back = cube_face(d);
                CHECK(back.face == face);
                CHECK(back.u == doctest::Approx(u).epsilon(1e-12).scale(1.0));
                CHECK(back.v == doctest::Approx(v).epsilon(1e-12).scale(1.0));
            }
        }
    }

    // ---- one stamp on top: knots must still agree bit-for-bit ----
    std::vector<terrain_stamp> stamps;
    stamps.push_back(terrain_stamp{
        .type = stamp_type::crater,
        .center_dir = math::normalize(math::dvec3{1.0, 0.1, 0.0}),
        .radius_rad = 0.12,
        .level_m = -150.0,
        .blend = 0.25});

    // Coarse: whole face 0 at level 0. Fine: its (1,1) child at level 1.
    const chunk_key ck{.face = 0, .level = 0, .x = 0, .y = 0};
    const chunk_key fk{.face = 0, .level = 1, .x = 1, .y = 1};
    const chunk_edges no_skirt{};
    const chunk_mesh coarse = gen_chunk(def, stamps, ck, no_skirt);
    const chunk_mesh fine = gen_chunk(def, stamps, fk, no_skirt);

    // (a) knots agree BIT-FOR-BIT with the shared sampler path: child
    // (1,1)'s knot (2a, 2b) sits at face-uv that the PARENT samples at
    // vertex (2+a, 2+b) (coarse step 0.5, fine step 0.25, child origin 0),
    // so the two heights/dirs must match exactly.
    for (int a = 0; a <= kChunk_cells / 2; ++a)
        for (int b = 0; b <= kChunk_cells / 2; ++b) {
            const auto& fv = fine.verts[(2 * b) * kChunk_verts + 2 * a];
            const auto& cv = coarse.verts[(2 + b) * kChunk_verts + 2 + a];
            CHECK(fv.height_m == cv.height_m);
            CHECK(fv.dir.x == cv.dir.x);
            CHECK(fv.dir.y == cv.dir.y);
            CHECK(fv.dir.z == cv.dir.z);
        }
    // gen_chunk rides the SHARED 7.2 sampler (renderer and sim cannot
    // disagree): chunk center height == direct terrain_height, exact bits.
    CHECK(coarse.verts[2 * kChunk_verts + 2].height_m ==
          terrain_height(def, stamps, math::dvec3{1.0, 0.0, 0.0}));
    bool lit = false;
    for (const auto& v : coarse.verts)
        if (std::fabs(v.height_m) > 1.0)
            lit = true;
    CHECK(lit);

    // (b) skirt morph sits EXACTLY on the coarse polyline: fine (1,1) with
    // its low-u neighbor one level coarser (edge 0, delta 1). Knots stay
    // exact; between-knot vertices are the exact lerp of the knot pair,
    // hence share the coarse neighbor's straight segment.
    const chunk_edges skirted{.neighbor_delta = {1, 0, 0, 0}};
    const chunk_mesh morphed = gen_chunk(def, stamps, fk, skirted);
    const auto uv_at = [&](int ix, int iy) {
        const double span = 2.0 / (1u << fk.level);
        const double step = span / kChunk_cells;
        return math::dvec2{-1.0 + fk.x * span + ix * step, -1.0 + fk.y * span + iy * step};
    };
    // Low-u edge (edge 0, delta 1): the morphing runs ALONG the shared
    // edge (v direction, column ix = 0). Knots iy = 0, 2, 4 keep the
    // EXACT sampled height (bit-match the coarse neighbor's vertices);
    // between-knot vertices sit EXACTLY on the coarse straight segment
    // joining their knot pair -> the two LOD polylines coincide.
    const auto h_at = [&](int ix, int iy) {
        const auto uv = uv_at(ix, iy);
        return terrain_height(def, stamps, face_unproject(0, uv.x, uv.y));
    };
    for (int iy = 0; iy <= kChunk_cells; iy += 2)
        CHECK(morphed.verts[iy * kChunk_verts].height_m == h_at(0, iy));  // knots exact
    for (int iy = 0; iy + 2 <= kChunk_cells; iy += 2) {
        const double k_lo = h_at(0, iy), k_hi = h_at(0, iy + 2);
        CHECK(morphed.verts[(iy + 1) * kChunk_verts].height_m == k_lo + 0.5 * (k_hi - k_lo));
    }
    // morphing DID change the surface between knots (non-vacuous): the
    // skirted mesh differs from the fully-sampled one.
    bool changed = false;
    for (std::size_t i = 0; i < morphed.verts.size(); ++i)
        if (morphed.verts[i].height_m != fine.verts[i].height_m)
            changed = true;
    CHECK(changed);

    // sibling edges at the SAME level agree without any skirt (delta 0):
    // chunk (1,0) low-u column == chunk (0,0) high-u column, bit-for-bit.
    const chunk_key ka{.face = 0, .level = 1, .x = 1, .y = 0};
    const chunk_key kb{.face = 0, .level = 1, .x = 0, .y = 0};
    const chunk_mesh a = gen_chunk(def, stamps, ka, no_skirt);
    const chunk_mesh b = gen_chunk(def, stamps, kb, no_skirt);
    for (int iy = 0; iy < kChunk_verts; ++iy) {
        const auto& va = a.verts[iy * kChunk_verts];
        const auto& vb = b.verts[iy * kChunk_verts + kChunk_cells];
        CHECK(va.height_m == vb.height_m);
        CHECK(va.dir.y == vb.dir.y);
    }

    // (c) purity: same inputs regenerate bit-identical meshes (the renderer
    // can stream a chunk at any time and cache by content).
    const chunk_mesh again = gen_chunk(def, stamps, fk, skirted);
    CHECK(hash_sum(morphed) == hash_sum(again));
    CHECK(terrain_height(def, stamps, math::dvec3{1.0, 0.0, 0.0}) ==
          terrain_height(def, stamps, math::dvec3{2.0, 0.0, 0.0}));  // dir normalization
    (void)R;
}

TEST_CASE("scatter_seed") {
    const terrain_def def = relief_def();
    const double R = 500000.0;
    std::vector<terrain_stamp> stamps;

    // Authored bounds: two bands with a bare gap; densities per km^2,
    // slopes off-radial (band 0 is a near-vertical-walls-free deep band).
    std::vector<surface_band> bands{
        surface_band{-1000.0, -200.0, 2.0e-4, 0.30},
        surface_band{200.0, 1000.0, 4.0e-4, 3.14},
    };

    const scatter_params params{.seed = 0xC0FFEEULL, .cells_per_face_edge = 16};
    const auto field = scatter_field(def, stamps, R, bands, params);

    // Nonzero, bounded (statistical band, wide on purpose): the seeded
    // walk produced SOMETHING within sane limits, and the zero-density and
    // empty-table cases produce NOTHING.
    CHECK(field.size() > 20);
    CHECK(field.size() < 20000);
    std::vector<surface_band> bare = bands;
    for (auto& row : bare)
        row.scatter_per_km2 = 0.0;
    CHECK(scatter_field(def, stamps, R, bare, params).empty());
    CHECK(scatter_field(def, stamps, R, {}, params).empty());

    // Determinism: identical seed regenerates bit-identical content, and
    // order is the published lattice order (face, x, y ascending): the v
    // coordinate never decreases along the field.
    // Exact content pin (count 195 + ordered hash for seed 0xC0FFEE):
    // locks the full walk mixing — any hash-lattice degeneration moves it.
    CHECK(hash_sum(field) == 4.7481477251082266e+294);
    const auto field2 = scatter_field(def, stamps, R, bands, params);
    REQUIRE(field2.size() == field.size());
    for (std::size_t i = 0; i < field.size(); ++i) {
        CHECK(field2[i].height_m == field[i].height_m);
        CHECK(field2[i].dir.x == field[i].dir.x);
        CHECK(field2[i].dir.y == field[i].dir.y);
        CHECK(field2[i].dir.z == field[i].dir.z);
        CHECK(field2[i].band == field[i].band);
        CHECK(field2[i].variant == field[i].variant);
        if (i > 0) {  // published lattice order: face, then v within a face
            const face_uv pv = cube_face(field[i - 1].dir);
            const face_uv cv = cube_face(field[i].dir);
            CHECK(cv.face >= pv.face);
            // Within a face, v can only wander inside one cell row's
            // jitter band; it can never fall back into an EARLIER row.
            const double du = 2.0 / params.cells_per_face_edge;
            if (cv.face == pv.face)
                CHECK(cv.v >= pv.v - 1.01 * du);
        }
    }
    // A different seed changes the field (content, not just luck).
    const auto other = scatter_field(def, stamps, R, bands,
                                     scatter_params{.seed = 0xBEEFULL, .cells_per_face_edge = 16});
    CHECK(hash_sum(other) != hash_sum(field));

    // Bounds proof: every instance re-evaluates inside its band at the
    // jittered dir — height window, first-match band, slope limit.
    std::size_t band0 = 0, band1 = 0;
    for (const auto& s : field) {
        CHECK(s.band == surface_band_at(bands, s.height_m));
        const surface_band& row = bands[static_cast<std::size_t>(s.band)];
        CHECK(s.height_m >= row.lo_m);
        CHECK(s.height_m <= row.hi_m);
        const math::dvec3 n = terrain_normal(def, R, stamps, s.dir);
        const double c = math::dot(n, s.dir);
        const double tilt = std::acos(c < -1.0 ? -1.0 : (c > 1.0 ? 1.0 : c));
        CHECK(tilt <= row.max_slope_rad + 1e-12);
        CHECK(s.variant < 4u);
        CHECK(s.height_m == terrain_height(def, stamps, s.dir));
        if (s.band == 0)
            ++band0;
        else
            ++band1;
    }
    // Both authored bands actually scatter (bounds overlap real terrain):
    CHECK(band0 > 0);
    CHECK(band1 > 0);
}

// 3.6: cook-side MikkTSpace tangent bake (#14). Pins the hand-rolled port's
// reference semantics: tangent tracks uv, w = bOrient, and the payload
// reconstruction bitangent = cross(normal, tangent) * w recovers the true
// dP/duv (so DCC-baked normal maps land in the right basis).
#include <ve/cook/mikktspace.h>

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

namespace {

using ve::cook::bake_mikk_tangents;
using ve::cook::baked_tangent;

bool near(float got, float want, float tol = 1e-4f) { return std::fabs(got - want) < tol; }

// cross(normal, tangent) * w — the documented payload reconstruction.
void check_recon_bitangent(const std::vector<float>& nrm, int v,
                           const baked_tangent& t,
                           float bx, float by, float bz) {
    const float cx = nrm[v * 3 + 1] * t.z - nrm[v * 3 + 2] * t.y;
    const float cy = nrm[v * 3 + 2] * t.x - nrm[v * 3] * t.z;
    const float cz = nrm[v * 3] * t.y - nrm[v * 3 + 1] * t.x;
    CHECK(near(cx * t.w, bx, 1e-4f));
    CHECK(near(cy * t.w, by, 1e-4f));
    CHECK(near(cz * t.w, bz, 1e-4f));
}

void check_unit(const baked_tangent& t) {
    const float len2 = t.x * t.x + t.y * t.y + t.z * t.z;
    CHECK(near(len2, 1.f, 1e-4f));
    CHECK(std::fabs(t.w) == 1.f);  // exact sign, never smeared
}

} // namespace

TEST_CASE("mikk_flat_quad") {
    // CCW from +Z, natural uv, normal +Z: tangent +X, w=+1, bitangent +Y.
    // Vertices 4/5 duplicate 0/1 bitwise but are referenced by NO triangle:
    // the reference leaves them in the default space (1,0,0,-1).
    std::vector<float> pos{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0};
    std::vector<float> nrm(18, 0.f);
    for (int i = 0; i < 6; ++i) nrm[i * 3 + 2] = 1.f;
    std::vector<float> uv{0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1};
    std::vector<std::uint32_t> tri{0, 1, 2, 0, 2, 3};
    const auto t = bake_mikk_tangents(pos, nrm, uv, tri);
    REQUIRE(t.size() == 6);
    for (int v = 0; v < 4; ++v) {
        check_unit(t[v]);
        CHECK(near(t[v].x, 1.f, 1e-4f));
        CHECK(near(t[v].y, 0.f, 1e-4f));
        CHECK(near(t[v].z, 0.f, 1e-4f));
        CHECK(t[v].w == 1.f);
        check_recon_bitangent(nrm, v, t[v], 0.f, 1.f, 0.f);
    }
    CHECK(near(t[4].x, 1.f, 1e-4f));
    CHECK(t[4].w == -1.f);
    CHECK(near(t[5].x, 1.f, 1e-4f));
    CHECK(t[5].w == -1.f);
}

TEST_CASE("mikk_split_quad_seam") {
    // Four triangles fanning around center vertex 4: the 4-rule grouping must
    // produce ONE consistent space at the center (seam-free weld), not four.
    std::vector<float> pos{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, .5f, .5f, 0};
    std::vector<float> nrm(15, 0.f);
    for (int i = 0; i < 5; ++i) nrm[i * 3 + 2] = 1.f;
    std::vector<float> uv{0, 0, 1, 0, 1, 1, 0, 1, .5f, .5f};
    std::vector<std::uint32_t> tri{0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4};
    const auto t = bake_mikk_tangents(pos, nrm, uv, tri);
    REQUIRE(t.size() == 5);
    CHECK(near(t[4].x, 1.f, 1e-4f));
    CHECK(near(t[4].y, 0.f, 1e-4f));
    CHECK(t[4].w == 1.f);
    check_recon_bitangent(nrm, 4, t[4], 0.f, 1.f, 0.f);
}

TEST_CASE("mikk_flipped_winding_parity") {
    // Winding flipped AND consistent flipped normal (negative scale): the
    // tangent still tracks uv (+X), but w flips to -1 so the reconstructed
    // bitangent still equals the true dP/dt (+Y) — this is exactly what
    // negative-scaled geometry needs so bakes match.
    std::vector<float> pos{0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0};
    std::vector<float> nrm(18, 0.f);
    for (int i = 0; i < 6; ++i) nrm[i * 3 + 2] = -1.f;
    std::vector<float> uv{0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1};
    std::vector<std::uint32_t> tri{0, 1, 2, 3, 4, 5};
    const auto t = bake_mikk_tangents(pos, nrm, uv, tri);
    REQUIRE(t.size() == 6);
    for (int v = 0; v < 6; ++v) {
        check_unit(t[v]);
        CHECK(near(t[v].x, 1.f, 1e-4f));
        CHECK(near(t[v].y, 0.f, 1e-4f));
        CHECK(t[v].w == -1.f);
        check_recon_bitangent(nrm, v, t[v], 0.f, 1.f, 0.f);
    }
}

TEST_CASE("mikk_degenerate_uv_default_space") {
    // Zero uv area everywhere: no recoverable derivatives, so the reference
    // contract is the default space (1,0,0,-1) — never garbage, never a hang.
    std::vector<float> pos{0, 0, 0, 1, 0, 0, 1, 1, 0};
    std::vector<float> nrm{0, 0, 1, 0, 0, 1, 0, 0, 1};
    std::vector<float> uv{0, 0, 0, 0, 0, 0};
    std::vector<std::uint32_t> tri{0, 1, 2};
    const auto t = bake_mikk_tangents(pos, nrm, uv, tri);
    REQUIRE(t.size() == 3);
    for (const auto& tv : t) {
        CHECK(near(tv.x, 1.f, 1e-4f));
        CHECK(near(tv.y, 0.f, 1e-4f));
        CHECK(near(tv.z, 0.f, 1e-4f));
        CHECK(tv.w == -1.f);
    }
}

TEST_CASE("mikk_empty_and_deterministic") {
    // Empty input -> empty output (no crash). Same input twice -> bitwise
    // equal output: cooked bytes must be golden-stable (0.9).
    CHECK(bake_mikk_tangents({}, {}, {}, {}).empty());
    std::vector<float> pos{0, 0, 0, 2, 0, 0, 2, 2, 1, 0, 0, 0, 2, 2, 1, 0, 0, 1};
    std::vector<float> nrm{0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0};
    std::vector<float> uv{0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};
    std::vector<std::uint32_t> tri{0, 1, 2, 3, 4, 5};
    const auto a = bake_mikk_tangents(pos, nrm, uv, tri);
    const auto b = bake_mikk_tangents(pos, nrm, uv, tri);
    REQUIRE(a.size() == 6);
    for (std::size_t v = 0; v < a.size(); ++v) {
        CHECK(a[v].x == b[v].x);
        CHECK(a[v].y == b[v].y);
        CHECK(a[v].z == b[v].z);
        CHECK(a[v].w == b[v].w);
    }
}

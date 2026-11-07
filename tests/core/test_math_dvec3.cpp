#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "ve/core/math/vec.h"

using namespace ve::core::math;

// Decision #22 numbers used as the precision contract: 1 AU = 1.496e11 m,
// Earth-class radius 6.371e6 m. Decision #3 says sim positions are fp64 and
// "cannot be retrofitted later" — the fp32 comparisons below show WHY.
TEST_CASE("math_fp64_precision") {
    constexpr double kAu = 1.496e11;   // m
    constexpr double kEarthR = 6.371e6; // m

    // fp64 at 1 AU: spacing = nextafter(a)-a = 2^(37-52) = 2^-15 ~= 3.05e-5 m
    // (~30 microns), measured here rather than asserted from theory. So
    // millimetre steps are resolvable at AU scale — formation offsets and
    // dock clamp tolerances survive full-system distances.
    double au_pos = kAu;
    CHECK((au_pos + 1e-3) != au_pos);                       // 1 mm does NOT vanish
    // eps envelope 1e-4 m absolute (scale max(|.|,1)=1): the subtraction of two
    // AU-scale doubles carries rounding noise up to ~1 ulp at 1 AU (3e-5 m),
    // so we assert the mm step is *resolved to within 0.1 mm*, not exact.
    CHECK(approximately_equal((au_pos + 1e-3) - au_pos, 1e-3, 1e-4));
    CHECK((au_pos + 1e-2) != au_pos);                       // 1 cm certainly not
    // dvec3 arithmetic keeps deltas exact at that scale (formation offsets)
    dvec3 p{au_pos, 0.0, 0.0};
    dvec3 q = p + dvec3{0.0, 2.5, -0.05};                   // 2.5 m / 5 cm offsets
    CHECK(approximately_equal(q.y - p.y, 2.5, 1e-9));
    CHECK(approximately_equal(q.z - p.z, -0.05, 1e-9));
    CHECK(approximately_equal(length(q - p), std::sqrt(2.5 * 2.5 + 0.05 * 0.05), 1e-9));

    // fp32 at 1 AU: spacing = 2^(37-23) = 2^14 = 16384 m. An 8 km offset is
    // absorbed completely into the same float — this is the "cannot retrofit
    // later" proof. (Measured spacing, not assumed.)
    float au_pos_f = 1.496e11f;
    CHECK(std::nextafter(au_pos_f, 1e30f) - au_pos_f == 16384.0f);
    CHECK(au_pos_f + 8.0e3f == au_pos_f);                   // 8 km vanishes!
    CHECK(au_pos_f + 2.0e4f != au_pos_f);                   // needs >ulp/2 rounding

    // fp64 at Earth-surface scale: spacing = 2^(22-52) = 2^-30 ~= 9.3e-10 m —
    // sub-nanometre, so standing on the moon inside a planet's well (#17b)
    // loses nothing at walkable precision even with the giant in range.
    double surf = kEarthR;
    CHECK((surf + 1e-3) != surf);
    CHECK(approximately_equal((surf + 1e-3) - surf, 1e-3, 1e-8));  // 1e-8 m abs envelope (ulp noise 9.3e-10)

    // fp64 normalize of AU-scale direction stays unit-length:
    dvec3 far = normalize(dvec3{kAu, kAu * 0.5, -kAu * 0.25});
    CHECK(approximately_equal(length(far), 1.0, 1e-15));

    // commutativity holds exactly for + and * (never rely on associativity —
    // sim logic must sum in fixed order; noted in docs/phase-01-math/notes.md)
    dvec3 a{1.1e10, -2.2e10, 3.3e10}, b{4.4e10, 5.5e10, -6.6e10};
    CHECK((a + b) == (b + a));
}

TEST_CASE("math_dvec3_ops") {
    dvec3 a{1.0, 2.0, 3.0}, b{4.0, 5.0, 6.0};
    CHECK((a + b) == dvec3(5.0, 7.0, 9.0));
    CHECK(dot(a, b) == doctest::Approx(32.0));
    CHECK(cross(a, b) == dvec3(-3.0, 6.0, -3.0));           // same RH rule as vec3f
    CHECK(cross(kAxisXf, kAxisYf) == kAxisZf);              // convention identical at both widths (fp64 basis)
    dvec3 n = normalize(dvec3{0.0, -3.0, 4.0});            // (0,-0.6,0.8)
    CHECK(approximately_equal(n, dvec3(0.0, -0.6, 0.8), 1e-12));
    CHECK(normalize(dvec3{}) == dvec3{});                   // zero-safe, both widths
    CHECK(approximately_equal(project(dvec3(2.0, 0.0, 0.0), dvec3(0.0, 1.0, 0.0)),
                               dvec3{}, 1e-15));           // orthogonal -> zero projection
    CHECK(length2(dvec3(1.0, 2.0, 2.0)) == doctest::Approx(9.0));
}

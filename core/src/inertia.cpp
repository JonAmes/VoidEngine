#include "ve/core/sim/inertia.h"

#include <array>
#include <cmath>
#include <utility>

#include "ve/core/math/mat3.h"

namespace ve::core::sim {
using ve::core::math::dmat3;
using ve::core::math::dvec3;

std::expected<ship_inertia, inertia_error> inertia_from_parts(
    std::span<const mass_point> parts) {
    double total = 0.0;
    dvec3 com{};
    for (const auto& p : parts) {
        total += p.mass;
        com = com + p.pos * p.mass;
    }
    if (!(total > 0.0))
        return std::unexpected(inertia_error::non_positive_total_mass);
    com = com / total;

    // Parallel-axis theorem per part: I about ship CoM = I_own + m*(|d|^2 E
    // - d d^T), d = part com - ship com. `pos` IS the part's own center of
    // mass (authored that way; cook-side for modules with offset CoM, 5.8).
    dmat3 tensor = dmat3::zero();
    for (const auto& p : parts) {
        const dvec3 d = p.pos - com;
        tensor = tensor + p.local_tensor +
                 (dmat3::diag(dvec3{dot(d, d), dot(d, d), dot(d, d)}) - outer(d, d)) *
                     p.mass;
    }
    return ship_inertia{total, com, tensor};
}

principal_frame principal_axes(const dmat3& sym) {
    // Explicit symmetric working copy (row-major mirror; only the upper
    // triangle matters). Fixed sweep/pair order -> bitwise deterministic.
    double a[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) a[r][c] = sym.m[c * 3 + r];
    double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};  // eigenvector accumulator

    // Cyclic Jacobi, 12 sweeps of all 3 pairs: 3x3 converges in far fewer
    // (quadratic once small); the count is a fixed constant, not adaptive.
    for (int sweep = 0; sweep < 12; ++sweep) {
        for (const auto& pq : std::array<std::pair<int, int>, 3>{{{0, 1}, {0, 2}, {1, 2}}}) {
            const int p = pq.first, q = pq.second;
            const double apq = a[p][q];
            if (apq == 0.0) continue;
            double theta = (a[q][q] - a[p][p]) / (2.0 * apq);
            double t = (theta >= 0.0 ? 1.0 : -1.0) /
                       (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
            const double c = 1.0 / std::sqrt(t * t + 1.0);
            const double s = t * c;
            const double app = a[p][p], aqq = a[q][q];
            a[p][p] = c * c * app - 2.0 * s * c * apq + s * s * aqq;
            a[q][q] = s * s * app + 2.0 * s * c * apq + c * c * aqq;
            a[p][q] = a[q][p] = 0.0;
            for (int k = 0; k < 3; ++k) {
                // A update skips the p/q rows (handled above); the V
                // accumulator updates ALL rows (NR jacobi) — skipping p/q
                // there silently corrupts the eigenvectors.
                if (k != p && k != q) {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = a[p][k] = c * akp - s * akq;
                    a[k][q] = a[q][k] = s * akp + c * akq;
                }
                const double vkp = v[k][p], vkq = v[k][q];
                v[k][p] = c * vkp - s * vkq;
                v[k][q] = s * vkp + c * vkq;
            }
        }
    }

    // Sort ascending by principal value (selection sort over 3: stable and
    // fixed order).
    double principal[3] = {a[0][0], a[1][1], a[2][2]};
    for (int i = 0; i < 3; ++i)
        for (int j = i + 1; j < 3; ++j)
            if (principal[j] < principal[i]) {
                std::swap(principal[i], principal[j]);
                for (int k = 0; k < 3; ++k) std::swap(v[k][i], v[k][j]);
            }

    // Sign convention: largest-|component| of each axis made positive.
    for (int j = 0; j < 3; ++j) {
        int kmax = 0;
        for (int k = 1; k < 3; ++k)
            if (std::fabs(v[k][j]) > std::fabs(v[kmax][j])) kmax = k;
        if (v[kmax][j] < 0.0)
            for (int k = 0; k < 3; ++k) v[k][j] = -v[k][j];
    }

    dmat3 axes{{v[0][0], v[1][0], v[2][0],   // column 0 = axis 0 (column-major)
                v[0][1], v[1][1], v[2][1],
                v[0][2], v[1][2], v[2][2]}};
    // Right-handedness: eigenvectors of a symmetric matrix are orthogonal;
    // the sign convention could still yield a reflection — flip the axis of
    // the SMALLEST principal value (deterministic choice).
    if (determinant(axes) < 0.0) {
        for (int k = 0; k < 3; ++k) axes.m[k] = -axes.m[k];
    }
    return principal_frame{axes, dvec3{principal[0], principal[1], principal[2]}};
}

}  // namespace ve::core::sim

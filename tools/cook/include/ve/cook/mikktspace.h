#pragma once
// Hand-rolled MikkTSpace tangent bake (#14: cook-side, so the runtime pays
// nothing and cooked data matches DCC "tangent space" bakes). This is a
// port of Morten S. Mikkelsen's public reference algorithm mikktspace.c
// ((C) 2011 Morten S. Mikkelsen, zlib-style license — full text at the top
// of mikktspace.cpp), NOT a vendored library: no new external dependency.
//
// Triangles-only port: glTF primitives are TRIANGLES (cook enforces mode
// 4), so the reference's 4-gon diagonal split, quad orientation fix, and
// QUAD_ONE_DEGEN_TRI path are unreachable and are not ported. Deviations
// that preserve output bit-for-bit are marked in the .cpp ("PORT NOTE").
//
// Determinism: the reference sorts int arrays with randomized-pivot
// quicksorts (INTERNAL_RND_SORT_SEED). Every such sort has a total order
// on distinct keys, so std::sort yields the identical sequence — we use
// std::sort and keep the pipeline order-dependent parts scan-ordered.

#include <cstdint>
#include <span>
#include <vector>

namespace ve::cook {

// Per-vertex tangent (x, y, z, w), w in {+1,-1}: MikkTSpace's bOrient sign.
// Reconstruction convention (our payload, our shaders):
//     bitangent = cross(normal, tangent) * w
// This is the glTF formula (their w = bOrient too), and it reconstructs
// the reference's own vOt exactly: for a good triangle with winding-
// consistent normal, cross(N, vOs)*bOrient == vOt (verified on the
// reference's InitTriInfo fS sign handling: the fS compensation in eq
// 18/19 makes vOs/vOt the true uv derivatives and bOrient carries the
// winding parity — a negative-scale / winding-flipped face gets the same
// tangent but w = -1).
struct baked_tangent {
    float x, y, z, w;
};

// pos/nrm: 3*vc, uv: 2*vc, tri: 3*tris; tri entries index the vertex arrays.
// Returns one tangent per vertex, normalized. Corners weld on bitwise-equal
// pos+normal+uv (reference semantics), and every corner of a vertex shares
// them, so per-vertex output equals the per-corner result.
// Vertices no healthy triangle covers fall back to the reference default
// (1,0,0) with bOrient false -> w = -1.
std::vector<baked_tangent> bake_mikk_tangents(std::span<const float> pos,
                                              std::span<const float> nrm,
                                              std::span<const float> uv,
                                              std::span<const std::uint32_t> tri);

} // namespace ve::cook

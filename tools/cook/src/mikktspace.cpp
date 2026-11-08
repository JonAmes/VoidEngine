// Port of mikktspace.c — original license, reproduced from
// https://github.com/mmikk/MikkTSpace (master, fetched 2026-11-08):
//
//   Copyright (C) 2011 by Morten S. Mikkelsen
//
//   This software is provided 'as-is', without any express or implied
//   warranty.  In no event will the authors be held liable for any damages
//   arising from the use of this software.
//
//   Permission is granted to anyone to use this software for any purpose,
//   including commercial applications, and to alter it and redistribute it
//   freely, subject to the following restrictions:
//
//   1. The origin of this software must not be misrepresented; you must not
//      claim that you wrote the original software. If you use this software
//      in a product, an acknowledgment in the product documentation would
//      be appreciated but is not required.
//   2. Altered source versions must be plainly marked as such, and must not
//      be misrepresented as being the original software.
//   3. This notice may not be removed or altered from any source
//      distribution.
//
// See ve/cook/mikktspace.h for the port scope (triangles-only) and the
// determinism notes. Function names below track the reference's names so a
// reviewer can diff against mikktspace.c.

#include "ve/cook/mikktspace.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <map>

namespace ve::cook {
namespace {

struct V3 {
    float x{}, y{}, z{};
};

V3 vadd(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 vsub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 vscale(float k, V3 v) { return {k * v.x, k * v.y, k * v.z}; }
float vdot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float vlen(V3 v) { return std::sqrt(vdot(v, v)); }
V3 vnormalize(V3 v) { return vscale(1.0f / vlen(v), v); }

// Reference veq: exact component equality (bit-level intent: welds are
// bitwise-identical attributes, NOT epsilon welds).
bool veq(V3 a, V3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

// Reference NotZero / VNotZero: "not zero and not subnormal".
bool not_zero(float f) { return std::fabs(f) > FLT_MIN; }
bool vnot_zero(V3 v) { return not_zero(v.x) || not_zero(v.y) || not_zero(v.z); }

enum e_flags : int { // reference flags, same values
    kMarkDegenerate = 1,
    kGroupWithAnything = 4,
    kOrientPreserving = 8,
    // kQuadOneDegenTri (2) intentionally absent: quad faces are unreachable
    // in this triangles-only port, so the flag is never set or read.
};

struct TriInfo {
    int face_neighbors[3] = {-1, -1, -1}; // reference FaceNeighbors
    int assigned_group[3] = {-1, -1, -1}; // index into groups, -1 = NULL
    // normalized first order face derivatives
    V3 v_os{}, v_ot{};
    float mag_s = 0, mag_t = 0; // original magnitudes
    int org_face = 0;
    int flag = 0;
    int tspaces_offs = 0;
};

struct Space { // reference STSpace
    V3 v_os{}, v_ot{};
    float mag_s = 1, mag_t = 1;
    int counter = 0; // averaging counter, reference semantics
    bool orient = false;
};

struct Group { // reference SGroup
    std::vector<int> faces;
    int vertex_representative = -1;
    bool orient_preserving = false;
};

struct SubGroup { // reference SSubGroup
    std::vector<int> tri_members;
};

// Corner slots: with triangles-only faces the reference's MakeIndex face-
// corner encoding is exactly (face*3 + vert), so a "corner index" is just
// the original slot into the emitted triangle list. pos_at/normal_at/
// uv_at decode slot -> vertex row, matching GetPosition/GetNormal/
// GetTexCoord.
struct MeshView {
    std::span<const float> pos, nrm, uv;
    std::span<const std::uint32_t> tri;

    std::uint32_t vert_of(int slot) const { return tri[static_cast<std::size_t>(slot)]; }
    V3 pos_at(int slot) const {
        const std::uint32_t v = vert_of(slot);
        return {pos[v * 3], pos[v * 3 + 1], pos[v * 3 + 2]};
    }
    V3 normal_at(int slot) const {
        const std::uint32_t v = vert_of(slot);
        return {nrm[v * 3], nrm[v * 3 + 1], nrm[v * 3 + 2]};
    }
    V3 uv_at(int slot) const {
        const std::uint32_t v = vert_of(slot);
        return {uv[v * 2], uv[v * 2 + 1], 1.0f}; // reference pads z=1
    }
};

// --- GenerateInitialVerticesIndexList (triangles-only) ---------------------
void generate_initial_lists(std::vector<TriInfo>& infos, std::vector<int>& tri_list,
                            int n_tris) {
    infos.assign(static_cast<std::size_t>(n_tris), TriInfo{});
    tri_list.assign(static_cast<std::size_t>(n_tris) * 3, 0);
    for (int t = 0; t < n_tris; ++t) {
        TriInfo& ti = infos[static_cast<std::size_t>(t)];
        ti.org_face = t;
        ti.tspaces_offs = t * 3; // iTSpacesOffs += verts, verts==3 always
        for (int k = 0; k < 3; ++k)
            tri_list[static_cast<std::size_t>(t) * 3 + static_cast<std::size_t>(k)] = t * 3 + k;
    }
}

// --- GenerateSharedVerticesIndexList ---------------------------------------
// PORT NOTE: the reference accelerates with a position grid + recursive
// split (MergeVertsFast); the weld relation itself is exact first-match on
// bitwise-equal (pos, normal, texcoord) in scan order. A map keyed on the
// 8 floats with IEEE comparison reproduces that relation AND the chosen
// representative (first corner in scan order) exactly, without the grid's
// allocation limit / recursion. -0.0 == +0.0 and NaN never weld — identical
// to veq()'s component ==.
void weld_corners(std::vector<int>& tri_list, const MeshView& m) {
    std::map<std::array<float, 8>, int> first_slot; // key -> tri_list entry of first corner
    for (int s = 0; s < static_cast<int>(tri_list.size()); ++s) {
        const int slot = tri_list[static_cast<std::size_t>(s)];
        const std::uint32_t v = m.vert_of(slot);
        const std::array<float, 8> key{m.pos[v * 3],      m.pos[v * 3 + 1], m.pos[v * 3 + 2],
                                       m.nrm[v * 3],      m.nrm[v * 3 + 1], m.nrm[v * 3 + 2],
                                       m.uv[v * 2],       m.uv[v * 2 + 1]};
        const auto it = first_slot.find(key);
        if (it == first_slot.end())
            first_slot.emplace(key, s);
        else
            tri_list[static_cast<std::size_t>(s)] = tri_list[static_cast<std::size_t>(it->second)];
    }
}

// --- DegenPrologue (quad-pair marking removed: unreachable for tris) ------
void degen_prologue(std::vector<TriInfo>& infos, std::vector<int>& tri_list,
                    int nr_tris_in, int tot_tris) {
    // Move degenerate triangles to the back of infos/tri_list without
    // reordering the good ones (reference loop, verbatim control flow).
    int next_good = 1;
    bool still_finding = true;
    int t = 0;
    while (t < nr_tris_in && still_finding) {
        const bool good = (infos[static_cast<std::size_t>(t)].flag & kMarkDegenerate) == 0;
        if (good) {
            if (next_good < t + 2)
                next_good = t + 2;
        } else {
            bool just_a_degen = true;
            while (just_a_degen && next_good < tot_tris) {
                const bool g2 = (infos[static_cast<std::size_t>(next_good)].flag & kMarkDegenerate) == 0;
                if (g2)
                    just_a_degen = false;
                else
                    ++next_good;
            }
            ++next_good;
            const int t0 = t, t1 = next_good - 1;
            if (!just_a_degen) {
                for (int i = 0; i < 3; ++i) {
                    const int tmp = tri_list[static_cast<std::size_t>(t0) * 3 + static_cast<std::size_t>(i)];
                    tri_list[static_cast<std::size_t>(t0) * 3 + static_cast<std::size_t>(i)] =
                        tri_list[static_cast<std::size_t>(t1) * 3 + static_cast<std::size_t>(i)];
                    tri_list[static_cast<std::size_t>(t1) * 3 + static_cast<std::size_t>(i)] = tmp;
                }
                std::swap(infos[static_cast<std::size_t>(t0)], infos[static_cast<std::size_t>(t1)]);
            } else {
                still_finding = false; // reference: "not supposed to happen"; code still works
            }
        }
        if (still_finding) ++t;
    }
}

// --- GetEdge (reference, verbatim) -----------------------------------------
void get_edge(int* i0_out, int* i1_out, int* edgenum_out, const int* indices,
              int i0_in, int i1_in) {
    *edgenum_out = -1;
    if (indices[0] == i0_in || indices[0] == i1_in) {
        if (indices[1] == i0_in || indices[1] == i1_in) {
            *edgenum_out = 0; // first edge
            *i0_out = indices[0];
            *i1_out = indices[1];
        } else {
            *edgenum_out = 2; // third edge
            *i0_out = indices[2];
            *i1_out = indices[0];
        }
    } else {
        *edgenum_out = 1; // second edge
        *i0_out = indices[1];
        *i1_out = indices[2];
    }
}

// --- BuildNeighborsFast (edge pairing; sorting made deterministic) --------
// PORT NOTE: reference does three randomized-pivot quicksorts (i0, then i1
// within i0 ties, then f within (i0,i1) ties = lexicographic (i0,i1,f)).
// The key triple is distinct per edge row, so std::sort with the same
// comparator yields the identical sequence; pairing scan is unchanged.
void build_neighbors(std::vector<TriInfo>& infos, const std::vector<int>& tri_list,
                     int nr_tris_in) {
    struct Edge { int i0, i1, f; };
    std::vector<Edge> edges(static_cast<std::size_t>(nr_tris_in) * 3);
    for (int f = 0; f < nr_tris_in; ++f)
        for (int i = 0; i < 3; ++i) {
            const int i0 = tri_list[static_cast<std::size_t>(f) * 3 + static_cast<std::size_t>(i)];
            const int i1 = tri_list[static_cast<std::size_t>(f) * 3 + static_cast<std::size_t>((i < 2) ? (i + 1) : 0)];
            Edge& e = edges[static_cast<std::size_t>(f) * 3 + static_cast<std::size_t>(i)];
            e.i0 = i0 < i1 ? i0 : i1;
            e.i1 = !(i0 < i1) ? i0 : i1;
            e.f = f;
        }
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
        if (a.i0 != b.i0) return a.i0 < b.i0;
        if (a.i1 != b.i1) return a.i1 < b.i1;
        return a.f < b.f;
    });

    const int entries = nr_tris_in * 3;
    for (int i = 0; i < entries; ++i) {
        const int i0 = edges[static_cast<std::size_t>(i)].i0;
        const int i1 = edges[static_cast<std::size_t>(i)].i1;
        const int f = edges[static_cast<std::size_t>(i)].f;
        TriInfo& tri_a = infos[static_cast<std::size_t>(f)];

        int i0_a = 0, i1_a = 0, edgenum_a = 0, edgenum_b = 0;
        get_edge(&i0_a, &i1_a, &edgenum_a, &tri_list[static_cast<std::size_t>(f) * 3], i0, i1);
        const bool unassigned_a = tri_a.face_neighbors[edgenum_a] == -1;
        if (!unassigned_a)
            continue;

        // scan forward for the first still-unassigned edge with the same
        // (reversed) endpoint pair — reference loop, verbatim
        bool not_found = true;
        int j = i + 1;
        while (j < entries && i0 == edges[static_cast<std::size_t>(j)].i0 &&
               i1 == edges[static_cast<std::size_t>(j)].i1 && not_found) {
            const int t = edges[static_cast<std::size_t>(j)].f;
            int i0_b = 0, i1_b = 0;
            // flip i0_b and i1_b (get true index ordering)
            get_edge(&i1_b, &i0_b, &edgenum_b, &tri_list[static_cast<std::size_t>(t) * 3],
                     edges[static_cast<std::size_t>(j)].i0, edges[static_cast<std::size_t>(j)].i1);
            const bool unassigned_b = infos[static_cast<std::size_t>(t)].face_neighbors[edgenum_b] == -1;
            if (i0_a == i0_b && i1_a == i1_b && unassigned_b)
                not_found = false;
            else
                ++j;
        }
        if (!not_found) {
            const int t = edges[static_cast<std::size_t>(j)].f;
            tri_a.face_neighbors[edgenum_a] = t;
            infos[static_cast<std::size_t>(t)].face_neighbors[edgenum_b] = f;
        }
    }
}

// --- InitTriInfo -----------------------------------------------------------
void init_tri_info(std::vector<TriInfo>& infos, const std::vector<int>& tri_list,
                   const MeshView& m, int nr_tris_in) {
    for (int f = 0; f < nr_tris_in; ++f) {
        TriInfo& ti = infos[static_cast<std::size_t>(f)];
        // assumed bad until derivatives prove otherwise
        ti.flag |= kGroupWithAnything;
    }

    // evaluate first order derivatives (reference eq 18/19)
    for (int f = 0; f < nr_tris_in; ++f) {
        TriInfo& ti = infos[static_cast<std::size_t>(f)];
        const int* row = &tri_list[static_cast<std::size_t>(f) * 3];
        const V3 v1 = m.pos_at(row[0]), v2 = m.pos_at(row[1]), v3 = m.pos_at(row[2]);
        const V3 t1 = m.uv_at(row[0]), t2 = m.uv_at(row[1]), t3 = m.uv_at(row[2]);

        const float t21x = t2.x - t1.x;
        const float t21y = t2.y - t1.y;
        const float t31x = t3.x - t1.x;
        const float t31y = t3.y - t1.y;
        const V3 d1 = vsub(v2, v1);
        const V3 d2 = vsub(v3, v1);

        const float signed_area_stx2 = t21x * t31y - t21y * t31x;
        const V3 vos = vsub(vscale(t31y, d1), vscale(t21y, d2)); // eq 18
        const V3 vot = vadd(vscale(-t31x, d1), vscale(t21x, d2)); // eq 19

        ti.flag |= (signed_area_stx2 > 0 ? kOrientPreserving : 0);

        if (not_zero(signed_area_stx2)) {
            const float abs_area = std::fabs(signed_area_stx2);
            const float len_os = vlen(vos);
            const float len_ot = vlen(vot);
            const float s = (ti.flag & kOrientPreserving) == 0 ? -1.0f : 1.0f;
            if (not_zero(len_os)) ti.v_os = vscale(s / len_os, vos);
            if (not_zero(len_ot)) ti.v_ot = vscale(s / len_ot, vot);
            // evaluate magnitudes prior to normalization
            ti.mag_s = len_os / abs_area;
            ti.mag_t = len_ot / abs_area;
            if (not_zero(ti.mag_s) && not_zero(ti.mag_t))
                ti.flag &= ~kGroupWithAnything; // good triangle
        }
    }
    // PORT NOTE: the reference's quad orientation-fix loop is unreachable
    // with unique org-face numbers (all faces triangles) and is not ported.

    build_neighbors(infos, tri_list, nr_tris_in);
}

// --- Build4RuleGroups + AssignRecur ----------------------------------------
bool assign_recur(const std::vector<int>& tri_list, std::vector<TriInfo>& infos,
                  std::vector<Group>& groups, int my_tri, int group_idx) {
    TriInfo& my = infos[static_cast<std::size_t>(my_tri)];
    Group& group = groups[static_cast<std::size_t>(group_idx)];

    const int vert_rep = group.vertex_representative;
    const int* verts = &tri_list[static_cast<std::size_t>(my_tri) * 3];
    int i = -1;
    if (verts[0] == vert_rep) i = 0;
    else if (verts[1] == vert_rep) i = 1;
    else if (verts[2] == vert_rep) i = 2;
    if (i < 0) // structurally unreachable (neighbors share the rep vertex); guard, not assert:
        return false; // corrupt glTF must never abort the cook

    if (my.assigned_group[i] == group_idx) return true;
    else if (my.assigned_group[i] != -1) return false;

    if ((my.flag & kGroupWithAnything) != 0) {
        // first to group with a group-with-anything triangle determines its
        // orientation — the only order dependency in the reference, kept.
        if (my.assigned_group[0] == -1 && my.assigned_group[1] == -1 && my.assigned_group[2] == -1) {
            my.flag &= ~kOrientPreserving;
            my.flag |= (group.orient_preserving ? kOrientPreserving : 0);
        }
    }
    const bool orient = (my.flag & kOrientPreserving) != 0;
    if (orient != group.orient_preserving) return false;

    group.faces.push_back(my_tri);
    my.assigned_group[i] = group_idx;

    const int neigh_l = my.face_neighbors[i];
    const int neigh_r = my.face_neighbors[i > 0 ? (i - 1) : 2];
    if (neigh_l >= 0) assign_recur(tri_list, infos, groups, neigh_l, group_idx);
    if (neigh_r >= 0) assign_recur(tri_list, infos, groups, neigh_r, group_idx);
    return true;
}

void build_4rule_groups(std::vector<TriInfo>& infos, const std::vector<int>& tri_list,
                        std::vector<Group>& groups, int nr_tris_in) {
    for (int f = 0; f < nr_tris_in; ++f)
        for (int i = 0; i < 3; ++i) {
            TriInfo& ti = infos[static_cast<std::size_t>(f)];
            if ((ti.flag & kGroupWithAnything) != 0 || ti.assigned_group[i] != -1)
                continue;
            const int vert_index = tri_list[static_cast<std::size_t>(f) * 3 + static_cast<std::size_t>(i)];
            const int gi = static_cast<int>(groups.size());
            groups.emplace_back();
            Group& g = groups.back();
            g.vertex_representative = vert_index;
            g.orient_preserving = (ti.flag & kOrientPreserving) != 0;
            ti.assigned_group[i] = gi;
            g.faces.push_back(f);

            const bool or_pre = (ti.flag & kOrientPreserving) != 0;
            const int neigh_l = ti.face_neighbors[i];
            const int neigh_r = ti.face_neighbors[i > 0 ? (i - 1) : 2];
            if (neigh_l >= 0) assign_recur(tri_list, infos, groups, neigh_l, gi);
            if (neigh_r >= 0) assign_recur(tri_list, infos, groups, neigh_r, gi);
            (void)or_pre; // reference checks assert(answer || orientation differs); guard-only here
        }
}

// --- helpers for GenerateTSpaces -------------------------------------------
V3 project_normalize(V3 v, V3 n) {
    v = vsub(v, vscale(vdot(n, v), n));
    if (vnot_zero(v)) v = vnormalize(v);
    return v;
}

Space eval_tspace(const std::vector<int>& members, const std::vector<TriInfo>& infos,
                  const std::vector<int>& tri_list, const MeshView& m, int vert_rep) {
    Space res;
    float angle_sum = 0;
    for (const int f : members) {
        const TriInfo& ti = infos[static_cast<std::size_t>(f)];
        // only valid triangles contribute
        if ((ti.flag & kGroupWithAnything) != 0) continue;

        int i = -1;
        const int* row = &tri_list[static_cast<std::size_t>(f) * 3];
        if (row[0] == vert_rep) i = 0;
        else if (row[1] == vert_rep) i = 1;
        else if (row[2] == vert_rep) i = 2;
        if (i < 0) continue; // unreachable; guard (see assign_recur)

        const V3 n = m.normal_at(row[i]);
        const V3 vos = project_normalize(ti.v_os, n);
        const V3 vot = project_normalize(ti.v_ot, n);

        const int i2 = row[i < 2 ? (i + 1) : 0];
        const int i1 = row[i];
        const int i0 = row[i > 0 ? (i - 1) : 2];

        const V3 p0 = m.pos_at(i0), p1 = m.pos_at(i1), p2 = m.pos_at(i2);
        V3 v1 = project_normalize(vsub(p0, p1), n);
        V3 v2 = project_normalize(vsub(p2, p1), n);

        // weight contribution by the angle between the two edge vectors
        float cos_a = vdot(v1, v2);
        cos_a = cos_a > 1 ? 1 : (cos_a < -1 ? -1 : cos_a);
        const float angle = std::acos(cos_a);

        res.v_os = vadd(res.v_os, vscale(angle, vos));
        res.v_ot = vadd(res.v_ot, vscale(angle, vot));
        res.mag_s += angle * ti.mag_s;
        res.mag_t += angle * ti.mag_t;
        angle_sum += angle;
    }
    if (vnot_zero(res.v_os)) res.v_os = vnormalize(res.v_os);
    if (vnot_zero(res.v_ot)) res.v_ot = vnormalize(res.v_ot);
    if (angle_sum > 0) {
        res.mag_s /= angle_sum;
        res.mag_t /= angle_sum;
    }
    return res;
}

bool compare_subgroups(const SubGroup& a, const SubGroup& b) {
    if (a.tri_members.size() != b.tri_members.size()) return false;
    return std::equal(a.tri_members.begin(), a.tri_members.end(), b.tri_members.begin());
}

// Reference AvgTSpace: the identity short-circuit matters — averaging
// identical spaces perturbs bits and creates tangent-space splits.
Space avg_tspace(const Space& s0, const Space& s1) {
    Space res;
    if (s0.mag_s == s1.mag_s && s0.mag_t == s1.mag_t && veq(s0.v_os, s1.v_os) && veq(s0.v_ot, s1.v_ot)) {
        res.mag_s = s0.mag_s;
        res.mag_t = s0.mag_t;
        res.v_os = s0.v_os;
        res.v_ot = s0.v_ot;
    } else {
        res.mag_s = 0.5f * (s0.mag_s + s1.mag_s);
        res.mag_t = 0.5f * (s0.mag_t + s1.mag_t);
        res.v_os = vadd(s0.v_os, s1.v_os);
        res.v_ot = vadd(s0.v_ot, s1.v_ot);
        if (vnot_zero(res.v_os)) res.v_os = vnormalize(res.v_os);
        if (vnot_zero(res.v_ot)) res.v_ot = vnormalize(res.v_ot);
    }
    return res;
}

// --- GenerateTSpaces --------------------------------------------------------
// f_angle_threshold = 180 in the reference default (genTangSpaceDefault), so
// thres_cos = cos(180deg) = -1: subgroups split only on exact antiparallel
// derivatives. Kept as a parameter to stay a faithful port.
bool generate_tspaces(std::vector<Space>& tspaces, const std::vector<TriInfo>& infos,
                      const std::vector<Group>& groups, const std::vector<int>& tri_list,
                      float thres_cos, const MeshView& m) {
    int max_faces = 0;
    for (const Group& g : groups)
        max_faces = std::max(max_faces, static_cast<int>(g.faces.size()));
    if (max_faces == 0) return true;

    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        const Group& group = groups[gi];
        std::vector<Space> subgroup_tspace;
        std::vector<SubGroup> unique_subgroups;

        for (std::size_t i = 0; i < group.faces.size(); ++i) {
            const int f = group.faces[i];
            const TriInfo& ti = infos[static_cast<std::size_t>(f)];
            int index = -1;
            for (int k = 0; k < 3; ++k)
                if (ti.assigned_group[k] == static_cast<int>(gi)) { index = k; break; }
            if (index < 0) continue; // unreachable for real group members

            const int vert_index = tri_list[static_cast<std::size_t>(f) * 3 + static_cast<std::size_t>(index)];
            const V3 n = m.normal_at(vert_index);
            const V3 vos = project_normalize(ti.v_os, n);
            const V3 vot = project_normalize(ti.v_ot, n);

            SubGroup tmp;
            for (const int t : group.faces) {
                const TriInfo& tt = infos[static_cast<std::size_t>(t)];
                const V3 vos2 = project_normalize(tt.v_os, n);
                const V3 vot2 = project_normalize(tt.v_ot, n);
                // triangles-only: bSameOrgFace (quad pairing) is always false
                const bool any = ((ti.flag | tt.flag) & kGroupWithAnything) != 0;
                const float cos_s = vdot(vos, vos2);
                const float cos_t = vdot(vot, vot2);
                if (any || (cos_s > thres_cos && cos_t > thres_cos))
                    tmp.tri_members.push_back(t);
            }
            // PORT NOTE: reference QuickSort of int members (distinct keys)
            // == std::sort; total order identical regardless of pivot.
            std::sort(tmp.tri_members.begin(), tmp.tri_members.end());

            std::size_t l = 0;
            while (l < unique_subgroups.size() && !compare_subgroups(tmp, unique_subgroups[l]))
                ++l;
            if (l == unique_subgroups.size()) {
                subgroup_tspace.push_back(
                    eval_tspace(tmp.tri_members, infos, tri_list, m, group.vertex_representative));
                unique_subgroups.push_back(std::move(tmp));
            }

            // output tspace (reference counter semantics: second hit averages)
            Space& out = tspaces[static_cast<std::size_t>(ti.tspaces_offs) + static_cast<std::size_t>(index)];
            if (out.counter == 1) {
                out = avg_tspace(out, subgroup_tspace[l]);
                out.counter = 2;
                out.orient = group.orient_preserving;
            } else { // counter == 0
                out = subgroup_tspace[l];
                out.counter = 1;
                out.orient = group.orient_preserving;
            }
        }
    }
    return true;
}

// --- DegenEpilogue (quad fixup loop removed: unreachable for tris) --------
void degen_epilogue(std::vector<Space>& tspaces, const std::vector<TriInfo>& infos,
                    const std::vector<int>& tri_list, int nr_tris_in, int tot_tris) {
    for (int t = nr_tris_in; t < tot_tris; ++t) {
        // degenerate triangle: copy a space from any good triangle sharing
        // the same welded corner index (reference loop, verbatim)
        const TriInfo& dt = infos[static_cast<std::size_t>(t)];
        for (int i = 0; i < 3; ++i) {
            const int index1 = tri_list[static_cast<std::size_t>(t) * 3 + static_cast<std::size_t>(i)];
            bool not_found = true;
            int j = 0;
            while (not_found && j < 3 * nr_tris_in) {
                if (index1 == tri_list[static_cast<std::size_t>(j)]) not_found = false;
                else ++j;
            }
            if (!not_found) {
                const int tri = j / 3;
                const int vert = j % 3;
                const TriInfo& src = infos[static_cast<std::size_t>(tri)];
                // reference indexes vert_num[vert]; triangles-only vert_num == vert
                tspaces[static_cast<std::size_t>(dt.tspaces_offs) + static_cast<std::size_t>(i)] =
                    tspaces[static_cast<std::size_t>(src.tspaces_offs) + static_cast<std::size_t>(vert)];
            }
        }
    }
}

} // namespace

std::vector<baked_tangent> bake_mikk_tangents(std::span<const float> pos,
                                              std::span<const float> nrm,
                                              std::span<const float> uv,
                                              std::span<const std::uint32_t> tri) {
    std::vector<baked_tangent> out;
    const int vc = static_cast<int>(pos.size() / 3);
    if (vc == 0) return out;
    out.assign(static_cast<std::size_t>(vc), baked_tangent{1, 0, 0, -1}); // reference default space

    const int n_tris = static_cast<int>(tri.size() / 3);
    if (n_tris <= 0 || static_cast<int>(nrm.size()) != vc * 3 || static_cast<int>(uv.size()) != vc * 2)
        return out; // malformed input: defaults, never a crash (callers validate upstream)
    for (const std::uint32_t i : tri)
        if (i >= static_cast<std::uint32_t>(vc))
            return out; // dangling index: defaults (guard, like corrupt-payload policy)

    MeshView m{pos, nrm, uv, tri};

    std::vector<TriInfo> infos;
    std::vector<int> tri_list;
    generate_initial_lists(infos, tri_list, n_tris);

    weld_corners(tri_list, m);

    // mark degenerate triangles (reference test: any two welded-space-
    // comparable positions equal)
    int degen = 0;
    for (int t = 0; t < n_tris; ++t) {
        const int* row = &tri_list[static_cast<std::size_t>(t) * 3];
        const V3 p0 = m.pos_at(row[0]), p1 = m.pos_at(row[1]), p2 = m.pos_at(row[2]);
        if (veq(p0, p1) || veq(p0, p2) || veq(p1, p2)) {
            infos[static_cast<std::size_t>(t)].flag |= kMarkDegenerate;
            ++degen;
        }
    }
    const int nr_tris_in = n_tris - degen;
    if (nr_tris_in > 0)
        degen_prologue(infos, tri_list, nr_tris_in, n_tris);

    std::vector<Space> tspaces(static_cast<std::size_t>(n_tris) * 3);
    // reference defaults (already set above for output; set here explicitly)
    for (Space& s : tspaces) {
        s.v_os = {1, 0, 0}; s.mag_s = 1;
        s.v_ot = {0, 1, 0}; s.mag_t = 1;
        s.counter = 0; s.orient = false;
    }

    if (nr_tris_in > 0) {
        init_tri_info(infos, tri_list, m, nr_tris_in);
        std::vector<Group> groups;
        build_4rule_groups(infos, tri_list, groups, nr_tris_in);
        generate_tspaces(tspaces, infos, groups, tri_list, -1.0f /* cos(180deg) */, m);
        degen_epilogue(tspaces, infos, tri_list, nr_tris_in, n_tris);
    }

    // Reference output walks psTspace in original corner order: with
    // triangles-only faces tspaces_offs == 3*org_face and the corner index
    // is the original slot. Per vertex: first corner's space wins (all
    // corners of a vertex weld identically in cooked data, so they agree).
    std::vector<char> claimed(static_cast<std::size_t>(vc), 0);
    for (int slot = 0; slot < n_tris * 3; ++slot) {
        const std::uint32_t v = tri[static_cast<std::size_t>(slot)];
        if (claimed[v]) continue;
        claimed[v] = 1;
        const Space& s = tspaces[static_cast<std::size_t>(slot)];
        out[v] = baked_tangent{s.v_os.x, s.v_os.y, s.v_os.z, s.orient ? 1.0f : -1.0f};
    }
    return out;
}

} // namespace ve::cook

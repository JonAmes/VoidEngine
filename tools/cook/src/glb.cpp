#include "glb.h"
#include "json.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <vector>

namespace ve::cook {
namespace {

// ---- little-endian writers -------------------------------------------------
void put32(Bytes& p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void put_f32(Bytes& p, float v) {
    std::uint8_t b[4];
    std::memcpy(b, &v, 4);
    p.insert(p.end(), b, b + 4);
}
void put_str(Bytes& p, const std::string& s) {
    put32(p, static_cast<std::uint32_t>(s.size()));
    p.insert(p.end(), s.begin(), s.end());
}

// ---- TRS (doubles for composition; f32 out is rendering precision) --------
struct Trs {
    double t[3] = {0, 0, 0};
    double q[4] = {0, 0, 0, 1}; // x,y,z,w
    double s[3] = {1, 1, 1};
};

void quat_rotate(const double q[4], const double v[3], double out[3]) {
    // v' = v + 2*cross(qv, qv x v + w v)
    const double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    const double cx = qy * v[2] - qz * v[1];
    const double cy = qz * v[0] - qx * v[2];
    const double cz = qx * v[1] - qy * v[0];
    const double tx = 2.0 * (qw * cx + (qy * cz - qz * cy));
    const double ty = 2.0 * (qw * cy + (qz * cx - qx * cz));
    const double tz = 2.0 * (qw * cz + (qx * cy - qy * cx));
    out[0] = v[0] + tx; out[1] = v[1] + ty; out[2] = v[2] + tz;
}

Trs compose(const Trs& parent, const Trs& child) {
    // parent ∘ child: p.s*(p.q*c.s) etc, standard TRS product.
    Trs r;
    double cs[3];
    quat_rotate(child.q, child.s, cs); // rotate child scale axes
    for (int i = 0; i < 3; ++i) r.s[i] = parent.s[i] * cs[i];
    double rot[4];
    const double a[4] = {parent.q[0], parent.q[1], parent.q[2], parent.q[3]};
    const double b[4] = {child.q[0], child.q[1], child.q[2], child.q[3]};
    rot[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    rot[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    rot[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    rot[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    std::memcpy(r.q, rot, sizeof rot);
    double ct[3];
    quat_rotate(child.q, child.t, ct);
    double pt[3];
    quat_rotate(parent.q, ct, pt);
    for (int i = 0; i < 3; ++i)
        r.t[i] = parent.t[i] + parent.s[i] * pt[i];
    return r;
}

void apply_point(const Trs& m, const double in[3], float out[3]) {
    const double sv[3] = {in[0] * m.s[0], in[1] * m.s[1], in[2] * m.s[2]};
    double rv[3];
    quat_rotate(m.q, sv, rv);
    // AXES(user decision, 2026-11-07 — pending): engine world-axis convention
    // not yet chosen; identity (glTF Y-up) applied and marked. When the user
    // picks the convention, switch it HERE (one place) and flip triangle
    // winding if the map is determinant-negative.
    for (int i = 0; i < 3; ++i) out[i] = static_cast<float>(rv[i] + m.t[i]);
}

void apply_dir(const Trs& m, const double in[3], float out[3]) {
    double rv[3];
    quat_rotate(m.q, in, rv); // normals: rotate only (uniform scale in slice art)
    for (int i = 0; i < 3; ++i) out[i] = static_cast<float>(rv[i]);
}

// ---- GLB chunks ------------------------------------------------------------
std::uint32_t rd32(const Bytes& b, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b[at + i]) << (8 * i);
    return v;
}

bool read_chunks(const Bytes& glb, json::Value& doc, Bytes& bin, std::string& err) {
    if (glb.size() < 12 || rd32(glb, 0) != 0x46546C67u || rd32(glb, 4) != 2) {
        err = "not a glTF 2.0 binary (.glb)";
        return false;
    }
    std::size_t at = 12;
    bool have_json = false;
    while (at + 8 <= glb.size()) {
        const std::uint32_t len = rd32(glb, at);
        const std::uint32_t type = rd32(glb, at + 4);
        at += 8;
        if (at + len > glb.size()) { err = "truncated chunk"; return false; }
        if (type == 0x4E4F534Au) { // 'JSON'
            doc = {};
            if (!json::parse(std::string(reinterpret_cast<const char*>(&glb[at]), len), doc, &err)) return false;
            have_json = true;
        } else if (type == 0x004E4942u) { // 'BIN\0'
            bin.assign(glb.begin() + static_cast<std::ptrdiff_t>(at),
                       glb.begin() + static_cast<std::ptrdiff_t>(at + len));
        } // unknown chunks tolerated per spec
        at += len;
    }
    if (!have_json) { err = "no JSON chunk"; return false; }
    return true;
}

// ---- accessor element access ----------------------------------------------
struct Accessor {
    const json::Value* acc = nullptr;
    std::size_t view_off = 0;
    std::size_t stride = 0;
    int comps = 0;
    int elem_size = 0; // bytes per component
};

bool read_accessor(const json::Value& doc, int idx, Accessor& out, std::string& err) {
    const auto* accs = doc.find("accessors");
    if (!accs || !accs->is_arr() || idx < 0 || idx >= static_cast<int>(accs->a().size())) {
        err = "bad accessor index " + std::to_string(idx);
        return false;
    }
    out.acc = &accs->a()[static_cast<std::size_t>(idx)];
    const auto* bv = out.acc->find("bufferView");
    if (!bv) { err = "sparse/absent accessors unsupported"; return false; }
    if (out.acc->find("sparse")) { err = "sparse accessors unsupported"; return false; }
    const int bvi = static_cast<int>(bv->d());
    const auto* bvs = doc.find("bufferViews");
    if (!bvs || bvi < 0 || bvi >= static_cast<int>(bvs->a().size())) { err = "bad bufferView"; return false; }
    const auto& v = bvs->a()[static_cast<std::size_t>(bvi)];
    const std::size_t off = v.find("byteOffset") ? static_cast<std::size_t>(v.find("byteOffset")->d()) : 0;
    const std::size_t stride = v.find("byteStride") ? static_cast<std::size_t>(v.find("byteStride")->d()) : 0;
    const auto& type = out.acc->find("type")->s();
    out.comps = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : 0;
    if (!out.comps) { err = "bad accessor type"; return false; }
    const int ct = static_cast<int>(out.acc->find("componentType")->d());
    out.elem_size = ct == 5126 ? 4 : ct == 5125 ? 4 : ct == 5123 ? 2 : 0; // F32/U32/U16
    if (!out.elem_size) { err = "unsupported componentType"; return false; }
    out.view_off = off;
    out.stride = stride ? stride : static_cast<std::size_t>(out.comps * out.elem_size);
    return true;
}

double accessor_value(const Accessor& a, const Bytes& bin, std::size_t row, int comp) {
    const std::size_t at = a.view_off + row * a.stride + static_cast<std::size_t>(comp) * a.elem_size;
    if (a.elem_size == 4) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &bin[at], 4);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    std::uint16_t h;
    std::memcpy(&h, &bin[at], 2);
    return h;
}

const json::Value* attr_of(const json::Value& prim, const char* key) {
    const auto* attrs = prim.find("attributes");
    return attrs ? attrs->find(key) : nullptr;
}

} // namespace

bool cook_glb_unit(const Bytes& glb, const std::string& id, Unit& out, std::string* err) {
    auto fail = [&](const std::string& msg) {
        if (err) *err = id + ": " + msg;
        return false;
    };

    json::Value doc;
    Bytes bin;
    std::string chunk_err;
    std::string scratch_err; // read_accessor failures here re-checked with a real message below
    if (!read_chunks(glb, doc, bin, chunk_err)) return fail(chunk_err);

    const auto* nodes = doc.find("nodes");
    const auto* meshes = doc.find("meshes");
    const auto* mats = doc.find("materials");
    if (!nodes || !nodes->is_arr() || nodes->a().empty()) return fail("no nodes");

    // world TRS per node (tree; glTF DAG sharing not expected — first parent wins)
    const auto& N = nodes->a();
    std::vector<Trs> world(N.size());
    std::vector<char> seen(N.size(), 0);

    std::function<bool(int, const Trs&)> walk = [&](int ni, const Trs& parent) -> bool {
        if (ni < 0 || ni >= static_cast<int>(N.size())) return false;
        if (seen[static_cast<std::size_t>(ni)]) return true;
        seen[static_cast<std::size_t>(ni)] = 1;
        const auto& node = N[static_cast<std::size_t>(ni)];
        Trs local;
        if (const auto* v = node.find("translation"))
            for (int i = 0; i < 3; ++i) local.t[i] = v->a()[static_cast<std::size_t>(i)].d();
        if (const auto* v = node.find("rotation"))
            for (int i = 0; i < 4; ++i) local.q[i] = v->a()[static_cast<std::size_t>(i)].d();
        if (const auto* v = node.find("scale"))
            for (int i = 0; i < 3; ++i) local.s[i] = v->a()[static_cast<std::size_t>(i)].d();
        world[static_cast<std::size_t>(ni)] = compose(parent, local);
        if (const auto* ch = node.find("children"))
            for (const auto& c : ch->a())
                if (!walk(static_cast<int>(c.d()), world[static_cast<std::size_t>(ni)])) return false;
        return true;
    };
    Trs root{};
    // roots: scene nodes if present, else unreferenced nodes; fall back: node 0.
    std::vector<int> roots;
    if (const auto* scenes = doc.find("scenes"); scenes && scenes->is_arr() && !scenes->a().empty()) {
        if (const auto* nn = scenes->a()[0].find("nodes"))
            for (const auto& c : nn->a()) roots.push_back(static_cast<int>(c.d()));
    }
    if (roots.empty()) {
        std::vector<char> is_child(N.size(), 0);
        for (const auto& node : N)
            if (const auto* ch = node.find("children"))
                for (const auto& c : ch->a()) {
                    const int ci = static_cast<int>(c.d());
                    if (ci >= 0 && ci < static_cast<int>(N.size())) is_child[static_cast<std::size_t>(ci)] = 1;
                }
        for (std::size_t i = 0; i < N.size(); ++i)
            if (!is_child[i]) roots.push_back(static_cast<int>(i));
    }
    for (int r : roots)
        if (!walk(r, root)) return fail("bad node child index");

    // ---- emit --------------------------------------------------------------
    out = {};
    out.type = kUnitMesh;
    Bytes& p = out.payload;

    std::vector<std::pair<std::string, int>> socket_list; // node idx
    std::string root_extras_json;

    std::uint32_t mesh_count = 0;
    Bytes mesh_body;
    for (std::size_t ni = 0; ni < N.size(); ++ni) {
        const auto& node = N[ni];
        const std::string name = node.find("name") ? node.find("name")->s() : "";
        const bool is_socket = name.rfind("hp_", 0) == 0;
        const auto* extras = node.find("extras");
        if (is_socket) {
            socket_list.emplace_back(name, static_cast<int>(ni));
            continue;
        }
        if (extras && extras->is_obj()) {
            if (!root_extras_json.empty()) return fail("multiple hull-extras nodes (one supported)");
            root_extras_json = json::write(*extras);
        }
        const auto* mi = node.find("mesh");
        if (!mi) continue;
        if (!meshes || !meshes->is_arr()) return fail("node references missing meshes[]");
        const auto& mesh = meshes->a()[static_cast<std::size_t>(mi->d())];
        const auto* prims = mesh.find("primitives");
        if (!prims) continue;
        for (std::size_t pi = 0; pi < prims->a().size(); ++pi) {
            const auto& prim = prims->a()[pi];
            const auto* pos = attr_of(prim, "POSITION");
            const auto* idx = prim.find("indices");
            if (!pos || !idx) return fail("mesh " + name + ": POSITION+indices required");
            if (const auto* mode = prim.find("mode"); mode && mode->d() != 4)
                return fail("mesh " + name + ": only TRIANGLES mode supported");

            Accessor pos_a;
            if (!read_accessor(doc, static_cast<int>(pos->d()), pos_a, scratch_err))
                return fail("bad POSITION accessor");
            const std::size_t vc = static_cast<std::size_t>(pos_a.acc->find("count")->d());

            Accessor nrm_a, uv_a;
            bool have_nrm = attr_of(prim, "NORMAL") &&
                            read_accessor(doc, static_cast<int>(attr_of(prim, "NORMAL")->d()), nrm_a, scratch_err);
            bool have_uv = attr_of(prim, "TEXCOORD_0") &&
                           read_accessor(doc, static_cast<int>(attr_of(prim, "TEXCOORD_0")->d()), uv_a, scratch_err);
            Accessor idx_a;
            if (!read_accessor(doc, static_cast<int>(idx->d()), idx_a, scratch_err))
                return fail("bad indices accessor");
            const std::size_t tc = static_cast<std::size_t>(idx_a.acc->find("count")->d()) / 3;

            put_str(mesh_body, prims->a().size() > 1 ? name + "#" + std::to_string(pi) : name);
            const auto* mat = prim.find("material");
            put32(mesh_body, mat ? static_cast<std::uint32_t>(mat->d()) : 0xFFFFFFFFu);
            put32(mesh_body, static_cast<std::uint32_t>(vc));
            const auto& m = world[ni];
            for (std::size_t r = 0; r < vc; ++r) {
                const double v[3] = {accessor_value(pos_a, bin, r, 0),
                                     accessor_value(pos_a, bin, r, 1),
                                     accessor_value(pos_a, bin, r, 2)};
                float o[3];
                apply_point(m, v, o);
                for (float f : o) put_f32(mesh_body, f);
            }
            for (std::size_t r = 0; r < vc; ++r) {
                float o[3];
                if (have_nrm) {
                    const double v[3] = {accessor_value(nrm_a, bin, r, 0),
                                         accessor_value(nrm_a, bin, r, 1),
                                         accessor_value(nrm_a, bin, r, 2)};
                    apply_dir(m, v, o);
                } else {
                    o[0] = 0; o[1] = 1; o[2] = 0;
                }
                for (float f : o) put_f32(mesh_body, f);
            }
            for (std::size_t r = 0; r < vc; ++r) {
                float u = 0, vv = 0;
                if (have_uv) {
                    u = static_cast<float>(accessor_value(uv_a, bin, r, 0));
                    vv = static_cast<float>(accessor_value(uv_a, bin, r, 1));
                }
                put_f32(mesh_body, u);
                put_f32(mesh_body, vv);
            }
            put32(mesh_body, static_cast<std::uint32_t>(tc));
            for (std::size_t r = 0; r < tc * 3; ++r)
                put32(mesh_body, static_cast<std::uint32_t>(accessor_value(idx_a, bin, r, 0)));
            ++mesh_count;
        }
    }

    // materials
    std::uint32_t mat_count = 0;
    Bytes mat_body;
    if (mats && mats->is_arr()) {
        mat_count = static_cast<std::uint32_t>(mats->a().size());
        for (const auto& mat : mats->a()) {
            put_str(mat_body, mat.find("name") ? mat.find("name")->s() : "");
            const auto* pbr = mat.find("pbrMetallicRoughness");
            float bc[4] = {1, 1, 1, 1};
            if (pbr && pbr->find("baseColorFactor"))
                for (int i = 0; i < 4; ++i) bc[i] = static_cast<float>(pbr->find("baseColorFactor")->a()[static_cast<std::size_t>(i)].d());
            const float metal = pbr && pbr->find("metallicFactor") ? static_cast<float>(pbr->find("metallicFactor")->d()) : 1.0f;
            const float rough = pbr && pbr->find("roughnessFactor") ? static_cast<float>(pbr->find("roughnessFactor")->d()) : 1.0f;
            float em[3] = {0, 0, 0};
            if (mat.find("emissiveFactor"))
                for (int i = 0; i < 3; ++i) em[i] = static_cast<float>(mat.find("emissiveFactor")->a()[static_cast<std::size_t>(i)].d());
            // decision #14: fold KHR emissive strength into the factor now.
            if (const auto* ext = mat.find("extensions"); ext) {
                if (const auto* ks = ext->find("KHR_materials_emissive_strength"); ks && ks->find("emissiveStrength")) {
                    const float str = static_cast<float>(ks->find("emissiveStrength")->d());
                    for (int i = 0; i < 3; ++i) em[i] *= str;
                }
            }
            for (float f : bc) put_f32(mat_body, f);
            put_f32(mat_body, metal);
            put_f32(mat_body, rough);
            for (float f : em) put_f32(mat_body, f);
            const std::string am = mat.find("alphaMode") ? mat.find("alphaMode")->s() : "OPAQUE";
            put32(mat_body, am == "BLEND" ? 1u : am == "MASK" ? 2u : 0u);
            // mat.doubleSided intentionally dropped — cooklet owns renderer flags.
        }
    }

    // sockets (names + extras preserved verbatim; TRS-baked translation)
    put32(p, mesh_count);
    p.insert(p.end(), mesh_body.begin(), mesh_body.end());
    put32(p, mat_count);
    p.insert(p.end(), mat_body.begin(), mat_body.end());
    put32(p, static_cast<std::uint32_t>(socket_list.size()));
    for (const auto& [name, ni] : socket_list) {
        put_str(p, name);
        const auto& m = world[static_cast<std::size_t>(ni)];
        const double zero[3] = {0, 0, 0};
        float o[3];
        apply_point(m, zero, o);
        for (float f : o) put_f32(p, f);
        const auto* extras = N[static_cast<std::size_t>(ni)].find("extras");
        const std::string ex = extras ? json::write(*extras) : "";
        put_str(p, ex);
    }
    put_str(p, root_extras_json);

    return true; // glb->texture deps land with material textures (later task)
}

} // namespace ve::cook

#pragma once
// test fixture: author minimal valid GLB bytes (task 3.8 texture-dep tests).
// Hand-built JSON + BIN chunks — no DCC dependency on the test side, same
// doctrine as the cooklet side being hand-rolled. One triangle "Hull"
// (POSITION/NORMAL/TEXCOORD_0, u32 indices) + N materials whose
// baseColor/metallicRoughness slots reference EXTERNAL image URIs.
#include <cstdint>
#include <string>
#include <vector>

#include <ve/cook/cook.h>

namespace ve_test {

struct MatSpec {
    std::string name;
    std::string basecolor_uri; // "" = slot absent
    std::string mr_uri;        // "" = slot absent
};

inline void g32(std::string& s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
inline void gf32(std::string& s, float f) {
    const char* p = reinterpret_cast<const char*>(&f);
    s.append(p, 4);
}

// embedded_images=true emits images as GLB bufferViews (no uri) — the
// cooklet must reject those (3.8: textures are source files, not blobs).
inline ve::cook::Bytes make_glb(const std::vector<MatSpec>& mats,
                                bool embedded_images = false) {
    // ---- BIN: pos@0(36) nrm@36(36) uv@72(24) idx@96(12) = 108 bytes ----
    std::string bin;
    const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const float nrm[9] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    const float uv[6] = {0, 0, 1, 0, 0, 1};
    for (float f : pos) gf32(bin, f);
    for (float f : nrm) gf32(bin, f);
    for (float f : uv) gf32(bin, f);
    g32(bin, 0); g32(bin, 1); g32(bin, 2);

    // ---- materials + texture/image tables --------------------------------
    std::vector<std::string> uris; // one textures[] entry per non-empty slot
    std::string mats_json = "\"materials\":[";
    for (std::size_t i = 0; i < mats.size(); ++i) {
        if (i) mats_json += ',';
        const auto& m = mats[i];
        mats_json += "{\"name\":\"" + m.name + "\"";
        if (!m.basecolor_uri.empty() || !m.mr_uri.empty()) {
            mats_json += ",\"pbrMetallicRoughness\":{";
            bool first = true;
            for (const auto* slot : {&m.basecolor_uri, &m.mr_uri}) {
                if (slot->empty()) continue;
                if (!first) mats_json += ',';
                first = false;
                const std::string key = slot == &m.basecolor_uri ? "baseColorTexture"
                                                                 : "metallicRoughnessTexture";
                mats_json += "\"" + key + "\":{\"index\":" + std::to_string(uris.size()) + "}";
                uris.push_back(*slot);
            }
            mats_json += "}";
        }
        mats_json += "}";
    }
    mats_json += "]";

    std::string tex_json, img_json;
    if (!uris.empty()) {
        tex_json = ",\"textures\":[";
        for (std::size_t i = 0; i < uris.size(); ++i) {
            if (i) tex_json += ',';
            tex_json += "{\"source\":" + std::to_string(i) + "}";
        }
        tex_json += ']';
        if (!embedded_images) {
            img_json = ",\"images\":[";
            for (std::size_t i = 0; i < uris.size(); ++i) {
                if (i) img_json += ',';
                img_json += "{\"uri\":\"" + uris[i] + "\"}";
            }
            img_json += ']';
        } else { // embedded: uri-less image -> cook must fail
            img_json = ",\"images\":[{\"bufferView\":0,\"mimeType\":\"image/png\"}]";
        }
    }

    std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"name\":\"Hull\",\"mesh\":0}],"
        "\"meshes\":[{\"name\":\"Hull\",\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
        "\"buffers\":[{\"byteLength\":108}],\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":12,\"target\":34963}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"}]," +
        mats_json + tex_json + img_json + "}";

    // ---- GLB container: chunks padded to 4 -------------------------------
    while (json.size() % 4) json += ' ';
    while (bin.size() % 4) bin += '\0';
    std::string out;
    g32(out, 0x46546C67u); // glTF
    g32(out, 2);           // version 2
    g32(out, static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + bin.size()));
    g32(out, static_cast<std::uint32_t>(json.size()));
    g32(out, 0x4E4F534Au); // "JSON"
    out += json;
    g32(out, static_cast<std::uint32_t>(bin.size()));
    g32(out, 0x004E4942u); // "BIN\0"
    out += bin;
    return ve::cook::Bytes(out.begin(), out.end());
}

} // namespace ve_test

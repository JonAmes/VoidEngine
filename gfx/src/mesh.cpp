#include <ve/gfx/mesh.h>

#include <algorithm>
#include <cstring>
#include <optional>

#include <ve/core/cooked.h>

namespace ve::gfx {

namespace {
std::optional<std::uint32_t> find_memory_type(const device& dev, std::uint32_t type_bits,
                                              VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(dev.physical(), &mp);
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return std::nullopt;
}

// Bounds-checked payload cursor. Every read checks remaining bytes BEFORE
// touching them; `bad` latches so callers can bail once at the end.
struct Cursor {
    const std::uint8_t* p;
    std::size_t left;
    bool bad = false;

    std::uint32_t u32() {
        if (left < 4) { bad = true; return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
        p += 4; left -= 4;
        return v;
    }
    float f32() {
        if (left < 4) { bad = true; return 0.0f; }
        float f = 0.0f;
        std::memcpy(&f, p, 4);
        p += 4; left -= 4;
        return f;
    }
    bool take(std::size_t n) { // skip/consume n bytes
        if (left < n) { bad = true; return false; }
        p += n; left -= n;
        return true;
    }
};
} // namespace

// Shared walk: parses the mesh list always; additionally fills *mats_out
// when the caller asked for the material list (3.15).
std::expected<std::vector<cooked_mesh>, std::string>
parse_payload_impl(const std::vector<std::uint8_t>& payload,
                   std::vector<cooked_material>* mats_out) {
    Cursor c{payload.data(), payload.size()};
    std::vector<cooked_mesh> meshes;

    const auto mesh_count = c.u32();
    if (c.bad) return std::unexpected("mesh payload: truncated header");
    meshes.reserve(std::min<std::size_t>(mesh_count, 4096u)); // cap the alloc; real
                                                              // truncation still fails on
                                                              // the first missing byte

    for (std::uint32_t m = 0; m < mesh_count && !c.bad; ++m) {
        cooked_mesh mesh;
        const auto name_len = c.u32();
        if (c.left < name_len) { return std::unexpected("mesh payload: truncated name"); }
        mesh.name.assign(reinterpret_cast<const char*>(c.p), name_len);
        c.take(name_len);
        mesh.material_index = c.u32();

        const auto vc = c.u32();
        // Count-then-size: refuse before resizing anything (garbage counts
        // must not ask for terabytes).
        if (vc > c.left / sizeof(mesh_vertex)) {
            return std::unexpected("mesh payload: vertex_count exceeds payload");
        }
        mesh.vertices.resize(vc);
        // Planar payload: f32 pos[vc*3] | f32 nrm[vc*3] | f32 uv[vc*2] |
        // f32 tan[vc*4] — NOT interleaved (cooklet writes component arrays;
        // we deinterleave).
        for (std::uint32_t v = 0; v < vc && !c.bad; ++v)
            for (float& f : mesh.vertices[v].pos) f = c.f32();
        if (c.bad) return std::unexpected("mesh payload: truncated pos data");
        for (std::uint32_t v = 0; v < vc && !c.bad; ++v)
            for (float& f : mesh.vertices[v].nrm) f = c.f32();
        if (c.bad) return std::unexpected("mesh payload: truncated nrm data");
        for (std::uint32_t v = 0; v < vc && !c.bad; ++v)
            for (float& f : mesh.vertices[v].uv) f = c.f32();
        if (c.bad) return std::unexpected("mesh payload: truncated uv data");
        for (std::uint32_t v = 0; v < vc && !c.bad; ++v)
            for (float& f : mesh.vertices[v].tan) f = c.f32();
        if (c.bad) return std::unexpected("mesh payload: truncated tan data");

        const auto tc = c.u32();
        if (tc > c.left / 4u) {
            return std::unexpected("mesh payload: tri_count exceeds payload");
        }
        mesh.indices.resize(tc * 3u);
        for (auto& idx : mesh.indices) idx = c.u32();
        if (c.bad) return std::unexpected("mesh payload: truncated index data");

        meshes.push_back(std::move(mesh));
    }

    // MAT0 material section: CONSUMED since 3.15 (scene harness reads
    // base_color/metallic/roughness + texture content IDs) — lands in
    // *mats_out when requested. Socket/root-extras sections stay walk-and-
    // validate so truncation anywhere still rejects the unit.
    const auto mat_count = c.u32();
    if (mat_count > 65536u) return std::unexpected("mesh payload: implausible material_count");
    if (mats_out) {
        mats_out->clear();
        mats_out->reserve(std::min<std::size_t>(mat_count, 4096u));
    }
    for (std::uint32_t i = 0; i < mat_count && !c.bad; ++i) {
        cooked_material mat;
        const auto n = c.u32();
        if (n > c.left) { c.bad = true; break; } // guard BEFORE any assign
        if (mats_out) mat.name.assign(reinterpret_cast<const char*>(c.p), n);
        c.take(n);                       // name
        for (int k = 0; k < 4; ++k) mat.base_color[k] = c.f32();
        mat.metallic = c.f32(); mat.roughness = c.f32();
        for (int k = 0; k < 3; ++k) mat.emissive[k] = c.f32();
        mat.alpha_mode = c.u32();
        const auto tl1 = c.u32();
        if (tl1 > c.left) { c.bad = true; break; }
        if (mats_out) mat.basecolor_tex.assign(reinterpret_cast<const char*>(c.p), tl1);
        c.take(tl1);                     // baseColor texture content id (3.8)
        const auto tl2 = c.u32();
        if (tl2 > c.left) { c.bad = true; break; }
        if (mats_out) mat.mr_tex.assign(reinterpret_cast<const char*>(c.p), tl2);
        c.take(tl2);                     // metallicRoughness texture id
        if (!c.bad && mats_out) mats_out->push_back(std::move(mat));
    }
    // Walk (and validate) the rest of the payload so truncation in the
    // socket sections still rejects the unit.
    const auto sock_count = c.u32();
    if (sock_count > 65536u) return std::unexpected("mesh payload: implausible socket_count");
    for (std::uint32_t i = 0; i < sock_count && !c.bad; ++i) {
        const auto n = c.u32();
        c.take(n);                       // name
        for (int k = 0; k < 3; ++k) c.f32();
        const auto xl = c.u32();
        c.take(xl);                      // extras JSON
    }
    const auto re_len = c.u32();
    c.take(re_len);                      // root extras
    if (c.bad) return std::unexpected("mesh payload: truncated material/socket sections");
    if (c.left != 0) return std::unexpected("mesh payload: trailing bytes");

    return meshes;
}

std::expected<std::vector<cooked_mesh>, std::string>
load_meshes_by_id(const std::string& cooked_dir, const std::string& content_id) {
    // The manifest is the authority (#13): unknown id = not loadable, even
    // if a matching .ck file happens to sit on disk.
    ve::core::cooked::Manifest manifest;
    if (std::string err; !ve::core::cooked::read_manifest(cooked_dir + "/manifest.bin", manifest, &err)) {
        return std::unexpected("mesh load: " + err);
    }
    if (!manifest.contains(content_id)) {
        return std::unexpected("mesh load: '" + content_id + "' not in manifest");
    }

    ve::core::cooked::Unit unit;
    if (std::string err;
        !ve::core::cooked::read_unit_file(cooked_dir + "/units/" + content_id + ".ck", unit, &err)) {
        return std::unexpected("mesh load: " + err);
    }
    if (unit.type != ve::core::cooked::kUnitMesh) {
        return std::unexpected("mesh load: unit '" + content_id + "' is not a MESH unit");
    }
    return parse_payload_impl(unit.payload, nullptr);
}

std::expected<std::vector<cooked_mesh>, std::string>
parse_mesh_payload(const std::vector<std::uint8_t>& payload) {
    return parse_payload_impl(payload, nullptr);
}

std::expected<std::vector<cooked_material>, std::string>
parse_mesh_materials(const std::vector<std::uint8_t>& payload) {
    std::vector<cooked_material> mats;
    if (auto meshes = parse_payload_impl(payload, &mats); !meshes) {
        return std::unexpected(meshes.error());
    }
    return mats;
}

std::expected<std::vector<cooked_material>, std::string>
load_materials_by_id(const std::string& cooked_dir, const std::string& content_id) {
    // Manifest authority (#13), same pattern as load_meshes_by_id.
    ve::core::cooked::Manifest manifest;
    if (std::string err;
        !ve::core::cooked::read_manifest(cooked_dir + "/manifest.bin", manifest, &err)) {
        return std::unexpected("material load: " + err);
    }
    if (!manifest.contains(content_id)) {
        return std::unexpected("material load: '" + content_id + "' not in manifest");
    }
    ve::core::cooked::Unit unit;
    if (std::string err;
        !ve::core::cooked::read_unit_file(cooked_dir + "/units/" + content_id + ".ck", unit, &err)) {
        return std::unexpected("material load: " + err);
    }
    if (unit.type != ve::core::cooked::kUnitMesh) {
        return std::unexpected("material load: unit '" + content_id + "' is not a MESH unit");
    }
    return parse_mesh_materials(unit.payload);
}

namespace {
struct HostBuffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* map = nullptr;
    void destroy(const device& d) {
        if (map) vkUnmapMemory(d.handle(), mem);
        if (mem) vkFreeMemory(d.handle(), mem, nullptr);
        if (buf) vkDestroyBuffer(d.handle(), buf, nullptr);
        *this = {};
    }
};

std::expected<HostBuffer, std::string> make_host_buffer(const device& dev, VkBufferUsageFlags usage,
                                                        VkDeviceSize size) {
    const VkBufferCreateInfo bci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    HostBuffer hb;
    if (const VkResult r = vkCreateBuffer(dev.handle(), &bci, nullptr, &hb.buf); r != VK_SUCCESS) {
        return std::unexpected("mesh upload: vkCreateBuffer VkResult=" + std::to_string(r));
    }
    const VkMemoryRequirements reqs = [&] {
        VkMemoryRequirements m{};
        vkGetBufferMemoryRequirements(dev.handle(), hb.buf, &m);
        return m;
    }();
    const auto mt = find_memory_type(dev, reqs.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mt) {
        vkDestroyBuffer(dev.handle(), hb.buf, nullptr);
        return std::unexpected("mesh upload: no host-visible memory type");
    }
    const VkMemoryAllocateInfo mai{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .allocationSize = reqs.size,
        .memoryTypeIndex = *mt,
    };
    if (const VkResult r = vkAllocateMemory(dev.handle(), &mai, nullptr, &hb.mem); r != VK_SUCCESS) {
        vkDestroyBuffer(dev.handle(), hb.buf, nullptr);
        hb.buf = VK_NULL_HANDLE;
        return std::unexpected("mesh upload: vkAllocateMemory VkResult=" + std::to_string(r));
    }
    if (const VkResult r = vkBindBufferMemory(dev.handle(), hb.buf, hb.mem, 0); r != VK_SUCCESS) {
        hb.destroy(dev);
        return std::unexpected("mesh upload: vkBindBufferMemory VkResult=" + std::to_string(r));
    }
    if (const VkResult r = vkMapMemory(dev.handle(), hb.mem, 0, VK_WHOLE_SIZE, 0, &hb.map); r != VK_SUCCESS) {
        hb.destroy(dev);
        return std::unexpected("mesh upload: vkMapMemory VkResult=" + std::to_string(r));
    }
    return hb;
}
} // namespace

std::expected<gpu_mesh, std::string> gpu_mesh::upload(const device& dev, const cooked_mesh& mesh) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return std::unexpected("mesh upload: empty mesh '" + mesh.name + "'");
    }
    gpu_mesh out;
    out.device_ = &dev;
    out.vertex_count_ = static_cast<std::uint32_t>(mesh.vertices.size());
    out.index_count_ = static_cast<std::uint32_t>(mesh.indices.size());

    // TRANSFER_SRC now so these host-visible buffers double as the staging
    // source when device-local meshes arrive in a perf phase — no API churn.
    auto vb = make_host_buffer(dev, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               mesh.vertices.size() * sizeof(mesh_vertex));
    if (!vb) return std::unexpected(vb.error());
    auto ib = make_host_buffer(dev, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               mesh.indices.size() * sizeof(std::uint32_t));
    if (!ib) {
        vb->destroy(dev);
        return std::unexpected(ib.error());
    }
    std::memcpy(vb->map, mesh.vertices.data(), mesh.vertices.size() * sizeof(mesh_vertex));
    std::memcpy(ib->map, mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t));

    out.vertex_buf_ = vb->buf; out.vertex_mem_ = vb->mem; out.vertex_map_ = vb->map;
    out.index_buf_ = ib->buf; out.index_mem_ = ib->mem; out.index_map_ = ib->map;
    return out;
}

gpu_mesh::~gpu_mesh() {
    if (!device_) return;
    if (vertex_map_) vkUnmapMemory(device_->handle(), vertex_mem_);
    if (index_map_) vkUnmapMemory(device_->handle(), index_mem_);
    if (vertex_mem_) vkFreeMemory(device_->handle(), vertex_mem_, nullptr);
    if (index_mem_) vkFreeMemory(device_->handle(), index_mem_, nullptr);
    if (vertex_buf_) vkDestroyBuffer(device_->handle(), vertex_buf_, nullptr);
    if (index_buf_) vkDestroyBuffer(device_->handle(), index_buf_, nullptr);
}

gpu_mesh::gpu_mesh(gpu_mesh&& o) noexcept { *this = std::move(o); }

gpu_mesh& gpu_mesh::operator=(gpu_mesh&& o) noexcept {
    if (this != &o) {
        this->~gpu_mesh();
        device_ = o.device_;
        vertex_buf_ = o.vertex_buf_; vertex_mem_ = o.vertex_mem_; vertex_map_ = o.vertex_map_;
        index_buf_ = o.index_buf_; index_mem_ = o.index_mem_; index_map_ = o.index_map_;
        vertex_count_ = o.vertex_count_; index_count_ = o.index_count_;
        o.device_ = nullptr;
        o.vertex_buf_ = VK_NULL_HANDLE; o.vertex_mem_ = VK_NULL_HANDLE; o.vertex_map_ = nullptr;
        o.index_buf_ = VK_NULL_HANDLE; o.index_mem_ = VK_NULL_HANDLE; o.index_map_ = nullptr;
        o.vertex_count_ = o.index_count_ = 0;
    }
    return *this;
}

} // namespace ve::gfx

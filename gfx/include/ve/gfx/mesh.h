#pragma once
#include <ve/gfx/device.h>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace ve::gfx {

// 3.5 cooked mesh -> GPU. Two separable halves:
//   (1) parse: bounds-checked reading of a MESH payload (layout documented
//       in core/cooked.h). Corrupt input is a load error, never a crash.
//   (2) upload: host-visible vertex/index VkBuffers (perf phases will move
//       to device-local + staging; the API won't change, only the memory
//       flags — consumers hold gpu_mesh, not memory details).
// Loading is by CONTENT ID THROUGH THE MANIFEST (decision #13): the
// manifest is the authority on what exists; a unit file the manifest does
// not know about is not loadable.

// One vertex, payload order pos/nrm/uv; byte-identical to the payload
// region so readback can byte-compare (static_assert pins the layout).
struct mesh_vertex {
    float pos[3];
    float nrm[3];
    float uv[2];
};
static_assert(sizeof(mesh_vertex) == 32, "payload vertex is 8 f32, no padding");

struct cooked_mesh {
    std::string name;                  // Blender node name (#14)
    std::uint32_t material_index = 0xFFFFFFFFu; // into the payload material list
    std::vector<mesh_vertex> vertices;
    std::vector<std::uint32_t> indices;         // triangle list, 3 per tri
};

// Parse the mesh list out of a full MESH payload. Validates every section
// header (materials/sockets/root extras are walked for integrity even
// though only meshes are returned) so truncation/garbage anywhere fails.
std::expected<std::vector<cooked_mesh>, std::string>
parse_mesh_payload(const std::vector<std::uint8_t>& payload);

// Content-ID load through the manifest: <cooked_dir>/manifest.bin must list
// the id; the unit comes from <cooked_dir>/units/<id>.ck and must be a
// MESH unit with a valid payload.
std::expected<std::vector<cooked_mesh>, std::string>
load_meshes_by_id(const std::string& cooked_dir, const std::string& content_id);

// Host-visible GPU buffers for one mesh. Mapped for the object's lifetime
// (host-coherent; the mapped pointer is a pin-test surface AND the eventual
// staging source). Move-only.
class gpu_mesh {
public:
    gpu_mesh() = default; // empty; dtor is a no-op until upload() fills it
    static std::expected<gpu_mesh, std::string> upload(const device& dev,
                                                       const cooked_mesh& mesh);
    ~gpu_mesh();
    gpu_mesh(gpu_mesh&& o) noexcept;
    gpu_mesh& operator=(gpu_mesh&& o) noexcept;
    gpu_mesh(const gpu_mesh&) = delete;
    gpu_mesh& operator=(const gpu_mesh&) = delete;

    [[nodiscard]] VkBuffer vertex_buffer() const { return vertex_buf_; }
    [[nodiscard]] VkBuffer index_buffer() const { return index_buf_; }
    [[nodiscard]] std::uint32_t vertex_count() const { return vertex_count_; }
    [[nodiscard]] std::uint32_t index_count() const { return index_count_; }
    // Mapped host views (null if upload failed — never is, on success).
    [[nodiscard]] const void* mapped_vertices() const { return vertex_map_; }
    [[nodiscard]] const void* mapped_indices() const { return index_map_; }

private:
    const device* device_ = nullptr;
    VkBuffer vertex_buf_ = VK_NULL_HANDLE, index_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory vertex_mem_ = VK_NULL_HANDLE, index_mem_ = VK_NULL_HANDLE;
    void* vertex_map_ = nullptr;
    void* index_map_ = nullptr;
    std::uint32_t vertex_count_ = 0, index_count_ = 0;
};

} // namespace ve::gfx

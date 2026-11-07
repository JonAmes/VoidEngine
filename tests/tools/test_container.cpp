// test: container_roundtrip + manifest_ids_stable.
#include <doctest/doctest.h>

#include <unistd.h> // getpid

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include <ve/cook/cook.h>

namespace {
std::string tmp_root(const char* name) {
    auto p = std::filesystem::temp_directory_path() /
             ("ve_cook_" + std::string(name) + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
std::vector<std::uint8_t> file_bytes(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    std::size_t n = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<std::uint8_t> b(n);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
    return b;
}
} // namespace

TEST_CASE("hash_known_vectors — FIPS/crc32 constants (self-consistency is NOT enough)") {
    auto hex_of = [](const std::array<std::uint8_t, 32>& a) {
        static const char* d = "0123456789abcdef";
        std::string s;
        for (auto b : a) { s += d[b >> 4]; s += d[b & 0xF]; }
        return s;
    };
    // FIPS 180-4 examples
    CHECK(hex_of(ve::cook::sha256({'a', 'b', 'c'})) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex_of(ve::cook::sha256({})) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // crc32 of the classic pangram
    const std::string fox = "The quick brown fox jumps over the lazy dog";
    ve::cook::Bytes b(fox.begin(), fox.end());
    CHECK(ve::cook::crc32(b) == 0x414FA339u);
}

TEST_CASE("container_roundtrip — units survive write/read; corruption is caught") {
    auto dir = tmp_root("container");
    ve::cook::Unit u;
    u.id = "models/ship/hull";
    u.type = ve::cook::kUnitMesh;
    u.payload = {1, 2, 3, 250, 0, 7, 99};

    std::string err;
    auto path = (std::filesystem::path(dir) / "hull.ck").string();
    REQUIRE(ve::cook::write_unit_file(path, u, &err));

    ve::cook::Unit back;
    REQUIRE(ve::cook::read_unit_file(path, back, &err));
    CHECK(back.id == u.id);
    CHECK(back.type == u.type);
    CHECK(back.payload == u.payload);

    // flip a payload byte -> crc must reject
    auto bytes = file_bytes(path);
    bytes[bytes.size() - 6] ^= 0xFF; // inside payload, not the trailing crc
    std::ofstream wf(path, std::ios::binary | std::ios::trunc);
    wf.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
    wf.close();
    ve::cook::Unit broken;
    CHECK_FALSE(ve::cook::read_unit_file(path, broken, &err));
    CHECK(err.find("crc") != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("manifest_ids_stable — content IDs are sorted; bytes are reproducible") {
    auto dir = tmp_root("manifest");
    ve::cook::Manifest m;
    for (const char* id : {"defs/z.toml", "a.png", "defs/mid.toml"}) {
        ve::cook::ManifestEntry e;
        e.id = id;
        e.source_sha = ve::cook::sha256({static_cast<std::uint8_t>(id[0])});
        if (std::string(id) == "defs/z.toml") e.deps = {"a.png", "defs/mid.toml"};
        m[id] = e;
    }

    std::string err;
    auto p1 = (std::filesystem::path(dir) / "m1.bin").string();
    auto p2 = (std::filesystem::path(dir) / "m2.bin").string();
    REQUIRE(ve::cook::write_manifest(p1, m, &err));
    REQUIRE(ve::cook::write_manifest(p2, m, &err));
    CHECK(file_bytes(p1) == file_bytes(p2)); // deterministic bytes

    ve::cook::Manifest back;
    REQUIRE(ve::cook::read_manifest(p1, back, &err));
    REQUIRE(back.size() == 3);
    // entries come back ordered by content ID
    std::vector<std::string> order;
    for (auto& [id, e] : back) order.push_back(id);
    CHECK(order == std::vector<std::string>{"a.png", "defs/mid.toml", "defs/z.toml"});
    CHECK(back["defs/z.toml"].deps ==
          std::vector<std::string>({"a.png", "defs/mid.toml"}));
    CHECK(back["a.png"].source_sha == ve::cook::sha256({static_cast<std::uint8_t>('a')}));

    // content IDs come from relative paths, so they repeat across runs
    CHECK(m.count("defs/z.toml") == 1);

    std::filesystem::remove_all(dir);
}

#include "ve/core/cooked.h"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace ve::core::cooked {
namespace detail {

void put_u32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}
void put_u64(Bytes& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}
void put_str(Bytes& out, const std::string& s) {
    put_u32(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}
void put_magic(Bytes& out, const char m[5]) {
    out.insert(out.end(), m, m + 4);
}

struct Reader {
    const Bytes& b;
    std::size_t p = 0;
    bool bad = false;

    std::uint32_t u32() {
        if (p + 4 > b.size()) { bad = true; return 0; }
        std::uint32_t v = b[p] | (std::uint32_t(b[p + 1]) << 8) |
                          (std::uint32_t(b[p + 2]) << 16) | (std::uint32_t(b[p + 3]) << 24);
        p += 4;
        return v;
    }
    std::uint64_t u64() {
        if (p + 8 > b.size()) { bad = true; return 0; }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= std::uint64_t(b[p + i]) << (8 * i);
        p += 8;
        return v;
    }
    Bytes raw(std::size_t n) {
        if (p + n > b.size()) { bad = true; return {}; }
        Bytes r(b.begin() + static_cast<std::ptrdiff_t>(p),
                b.begin() + static_cast<std::ptrdiff_t>(p + n));
        p += n;
        return r;
    }
    std::string str() {
        auto bytes = raw(u32());
        return std::string(bytes.begin(), bytes.end());
    }
    bool magic(const char m[5]) {
        auto bytes = raw(4);
        return !bad && std::memcmp(bytes.data(), m, 4) == 0;
    }
};

bool write_bytes(const std::string& path, const Bytes& data, std::string* err) {
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (err) *err = "cannot open for write: " + path;
        return false;
    }
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    if (!f) {
        if (err) *err = "write failed: " + path;
        return false;
    }
    return true;
}

bool read_bytes(const std::string& path, Bytes& out, std::string* err) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        if (err) *err = "cannot open for read: " + path;
        return false;
    }
    auto size = f.tellg();
    f.seekg(0);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0 &&
        !f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size))) {
        if (err) *err = "read failed: " + path;
        return false;
    }
    return true;
}
} // namespace detail

// container IO helpers visible to the rest of this TU
using namespace detail;

// Public whole-file IO (header-declared; cooklet + runtime loaders use these).
bool write_file_bytes(const std::string& path, const Bytes& data, std::string* err) {
    return write_bytes(path, data, err);
}
bool read_file_bytes(const std::string& path, Bytes& out, std::string* err) {
    return read_bytes(path, out, err);
}

// ---- crc32 (IEEE 802.3, reflected) -----------------------------------------
std::uint32_t crc32(const Bytes& data) {
    static std::uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        built = true;
    }
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::uint8_t b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ---- sha256 (hand-rolled; tools-side only) ---------------------------------
std::array<std::uint8_t, 32> sha256(const Bytes& data) {
    auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    static constexpr std::uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    Bytes msg = data;
    std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0x00);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF));

    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(msg[off + 4 * i]) << 24) |
                   (std::uint32_t(msg[off + 4 * i + 1]) << 16) |
                   (std::uint32_t(msg[off + 4 * i + 2]) << 8) |
                   std::uint32_t(msg[off + 4 * i + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            std::uint32_t ch = (e & f) ^ ((~e) & g);
            std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[4 * i] = static_cast<std::uint8_t>(h[i] >> 24);
        out[4 * i + 1] = static_cast<std::uint8_t>(h[i] >> 16);
        out[4 * i + 2] = static_cast<std::uint8_t>(h[i] >> 8);
        out[4 * i + 3] = static_cast<std::uint8_t>(h[i]);
    }
    return out;
}

// ---- unit container ---------------------------------------------------------
bool write_unit_file(const std::string& path, const Unit& unit, std::string* err) {
    Bytes out;
    put_magic(out, "VECK");
    put_u32(out, kCookFormat);
    put_u32(out, unit.type);
    put_str(out, unit.id);
    put_u64(out, unit.payload.size());
    out.insert(out.end(), unit.payload.begin(), unit.payload.end());
    put_u32(out, crc32(unit.payload));
    return write_bytes(path, out, err);
}

bool read_unit_file(const std::string& path, Unit& out, std::string* err) {
    Bytes b;
    if (!read_bytes(path, b, err)) return false;
    Reader r{b};
    if (!r.magic("VECK")) { if (err) *err = "not a VECK unit: " + path; return false; }
    if (r.u32() != kCookFormat) { if (err) *err = "unit format mismatch: " + path; return false; }
    out.type = r.u32();
    out.id = r.str();
    auto len = r.u64();
    out.payload = r.raw(static_cast<std::size_t>(len));
    auto want = r.u32();
    if (r.bad) { if (err) *err = "truncated unit: " + path; return false; }
    if (crc32(out.payload) != want) { if (err) *err = "payload crc mismatch: " + path; return false; }
    return true;
}

// ---- manifest ---------------------------------------------------------------
bool write_manifest(const std::string& path, const Manifest& m, std::string* err) {
    Bytes out;
    put_magic(out, "VEMF");
    put_u32(out, kCookFormat);
    put_u32(out, static_cast<std::uint32_t>(m.size()));
    for (const auto& [id, e] : m) {
        put_str(out, e.id);
        put_u32(out, static_cast<std::uint32_t>(e.deps.size()));
        for (const auto& d : e.deps) put_str(out, d);
        out.insert(out.end(), e.source_sha.begin(), e.source_sha.end());
    }
    return write_bytes(path, out, err);
}

bool read_manifest(const std::string& path, Manifest& out, std::string* err) {
    Bytes b;
    if (!read_bytes(path, b, err)) return false;
    Reader r{b};
    if (!r.magic("VEMF")) { if (err) *err = "not a VEMF manifest: " + path; return false; }
    if (r.u32() != kCookFormat) { if (err) *err = "manifest format mismatch: " + path; return false; }
    auto n = r.u32();
    for (std::uint32_t i = 0; i < n && !r.bad; ++i) {
        ManifestEntry e;
        e.id = r.str();
        auto nd = r.u32();
        for (std::uint32_t d = 0; d < nd && !r.bad; ++d) e.deps.push_back(r.str());
        auto sha = r.raw(32);
        std::copy(sha.begin(), sha.end(), e.source_sha.begin());
        out[e.id] = std::move(e);
    }
    if (r.bad) { if (err) *err = "truncated manifest: " + path; return false; }
    return true;
}

} // namespace ve::core::cooked

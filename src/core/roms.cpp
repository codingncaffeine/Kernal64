#include "roms.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace k64 {

namespace {

uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

} // namespace

// SHA-1 (FIPS 180-4).
std::string sha1Hex(const uint8_t* data, size_t size) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<uint8_t> msg(data, data + size);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    const uint64_t bits = static_cast<uint64_t>(size) * 8;
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>(bits >> (i * 8)));
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t{msg[off + i * 4]} << 24) | (uint32_t{msg[off + i * 4 + 1]} << 16) |
                   (uint32_t{msg[off + i * 4 + 2]} << 8) | msg[off + i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    char out[41];
    for (int i = 0; i < 5; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return out;
}

const char* romName(RomRole r) {
    for (const KnownRom& k : kKnownRoms) if (k.role == r) return k.name;
    return "?";
}

RomSet findRoms(const std::string& folder) {
    RomSet set;
    namespace fs = std::filesystem;
    auto consider = [&](const std::vector<uint8_t>& data, const std::string& from) {
        const std::string hash = sha1Hex(data.data(), data.size());
        for (const KnownRom& k : kKnownRoms) {
            const size_t i = static_cast<size_t>(k.role);
            if (k.size == data.size() && hash == k.sha1 && set.image[i].empty()) {
                set.image[i] = data;
                set.from[i] = from;
            }
        }
    };
    for (const fs::path& dir : {fs::path(folder), fs::path(folder) / "kernal64"}) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const auto size = entry.file_size(ec);
            if (ec || (size != 4096 && size != 8192 && size != 16384)) continue;
            std::ifstream f(entry.path(), std::ios::binary);
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
            if (data.size() != size) continue;
            const std::string from = entry.path().filename().string();
            if (size == 16384) {
                // A combined image: each half is tried as a ROM of its own.
                consider(std::vector<uint8_t>(data.begin(), data.begin() + 8192), from + " (first half)");
                consider(std::vector<uint8_t>(data.begin() + 8192, data.end()), from + " (second half)");
            } else {
                consider(data, from);
            }
        }
    }
    return set;
}

} // namespace k64

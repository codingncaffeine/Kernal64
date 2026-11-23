#include "crt.h"

#include <cstring>

namespace k64 {

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

} // namespace

bool parseCrt(const uint8_t* data, size_t size, CartImage& out, std::string& error) {
    out = CartImage{};
    if (size < 0x40 || std::memcmp(data, "C64 CARTRIDGE   ", 16) != 0) {
        const bool other = size >= 16 && std::memcmp(data + 4, " CARTRIDGE", 10) == 0;
        error = other ? "a cartridge file for another machine" : "not a cartridge file";
        return false;
    }
    // Some files give $20 as the header length; the header is never shorter than $40.
    size_t pos = be32(data + 0x10);
    if (pos < 0x40) pos = 0x40;
    out.type = be16(data + 0x16);
    out.exrom = data[0x18] ? 1 : 0;
    out.game = data[0x19] ? 1 : 0;
    out.subtype = data[0x1A];
    for (size_t i = 0x20; i < 0x40 && data[i]; ++i) out.name += static_cast<char>(data[i]);
    while (pos + 0x10 <= size) {
        const uint8_t* p = data + pos;
        if (std::memcmp(p, "CHIP", 4) != 0) { error = "damaged cartridge file: no chip packet where one should start"; return false; }
        const uint32_t packet = be32(p + 4);
        CartImage::Chip chip;
        chip.kind = be16(p + 8);
        chip.bank = be16(p + 10);
        chip.load = be16(p + 12);
        const size_t length = be16(p + 14);
        // A RAM packet may come without data; any other must hold what it announces.
        const size_t present = packet >= 0x10 ? packet - 0x10 : 0;
        const size_t have = length < present ? length : present;
        if (pos + 0x10 + have > size) { error = "damaged cartridge file: a chip packet is cut short"; return false; }
        if (have < length && chip.kind != 1) { error = "damaged cartridge file: a chip packet holds less than its size"; return false; }
        chip.data.assign(p + 0x10, p + 0x10 + have);
        chip.data.resize(length, 0);
        out.chips.push_back(std::move(chip));
        if (packet < 0x10) break;
        pos += packet;
    }
    if (out.chips.empty()) { error = "cartridge file without a chip packet"; return false; }
    return true;
}

} // namespace k64

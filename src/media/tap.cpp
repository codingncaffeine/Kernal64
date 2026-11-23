#include "tap.h"

#include <cstring>

namespace k64 {

// T1 (the TAP format description, the reference manual's "TAP" section): a 20-byte header,
// "C64-TAPE-RAW", version at $0C, platform at $0D, video standard at $0E, data size at $10.
// T2: version 0: a byte = the pulse length / 8; $00 = longer than 255 * 8 (taken as 256 * 8).
// T3: version 1: $00 is followed by the exact length in cycles, three bytes low first.
bool parseTap(const uint8_t* data, size_t size, Tape& out, std::string& error) {
    if (size < 20 || std::memcmp(data, "C64-TAPE-RAW", 12) != 0) { error = "not a TAP image"; return false; }
    const int version = data[12];
    if (version > 1) { error = "TAP version " + std::to_string(version) + " is not for the C64"; return false; }
    size_t len = data[16] | (data[17] << 8) | (data[18] << 16) | (static_cast<size_t>(data[19]) << 24);
    if (len > size - 20) len = size - 20;   // a header that claims more than the file holds
    out = Tape{};
    out.pulses.reserve(len);
    const uint8_t* p = data + 20;
    const uint8_t* end = p + len;
    while (p < end) {
        const uint8_t b = *p++;
        if (b) { out.pulses.push_back(8u * b); continue; }
        if (version == 0) { out.pulses.push_back(256u * 8u); continue; }
        if (end - p < 3) break;
        out.pulses.push_back(p[0] | (p[1] << 8) | (static_cast<uint32_t>(p[2]) << 16));
        p += 3;
    }
    return true;
}

// T4: version 1; lengths that are a multiple of 8 up to 255 * 8 as one byte, every other as
// $00 + three bytes (so nothing is rounded).
std::vector<uint8_t> writeTap(const Tape& tape, int videoStandard) {
    std::vector<uint8_t> out(20, 0);
    std::memcpy(out.data(), "C64-TAPE-RAW", 12);
    out[12] = 1;
    out[13] = 0;   // C64
    out[14] = static_cast<uint8_t>(videoStandard);
    for (uint32_t c : tape.pulses) {
        if (c % 8 == 0 && c / 8 >= 1 && c / 8 <= 255) {
            out.push_back(static_cast<uint8_t>(c / 8));
        } else {
            const uint32_t v = c > 0xFFFFFF ? 0xFFFFFF : c;
            out.push_back(0);
            out.push_back(static_cast<uint8_t>(v));
            out.push_back(static_cast<uint8_t>(v >> 8));
            out.push_back(static_cast<uint8_t>(v >> 16));
        }
    }
    const size_t n = out.size() - 20;
    for (int k = 0; k < 4; ++k) out[16 + static_cast<size_t>(k)] = static_cast<uint8_t>(n >> (8 * k));
    return out;
}

} // namespace k64

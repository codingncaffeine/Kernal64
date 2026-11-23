// System ROMs: identified by content (SHA-1), never by file name, and never shipped.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace k64 {

enum class RomRole : uint8_t { Basic, Kernal, Chargen, Drive1541Low, Drive1541High, Count };

struct KnownRom {
    RomRole role;
    size_t size;
    const char* sha1;  // lowercase hex
    const char* name;  // the conventional file name, for messages
};

// Only hashes measured from real dumps are listed. A combined 16 KB BASIC+KERNAL image is
// recognized through its halves.
inline constexpr KnownRom kKnownRoms[] = {
    {RomRole::Basic, 8192, "79015323128650c742a3694c9429aa91f355905e", "basic.901226-01.bin"},
    {RomRole::Kernal, 8192, "1d503e56df85a62fee696e7618dc5b4e781df1bb", "kernal.901227-03.bin"},
    {RomRole::Chargen, 4096, "adc7c31e18c7c7413d54802ef2f4193da14711aa", "characters.901225-01.bin"},
    {RomRole::Drive1541Low, 8192, "8e0547430135ba462525c224e76356bd3d430f11", "1541-c000.325302-01.bin"},
    {RomRole::Drive1541High, 8192, "f5d60777440829e46dc91285e662ba072acd2d8b", "1541-e000.901229-05.bin"},
};

std::string sha1Hex(const uint8_t* data, size_t size);

// The ROM set found in a folder (not recursive into unrelated folders: the folder itself and
// a "kernal64" folder inside it).
struct RomSet {
    std::array<std::vector<uint8_t>, static_cast<size_t>(RomRole::Count)> image;
    std::array<std::string, static_cast<size_t>(RomRole::Count)> from;
    bool has(RomRole r) const { return !image[static_cast<size_t>(r)].empty(); }
    const std::vector<uint8_t>& get(RomRole r) const { return image[static_cast<size_t>(r)]; }
};

RomSet findRoms(const std::string& folder);

// The conventional file name of a role's preferred ROM, for "missing" messages.
const char* romName(RomRole r);

} // namespace k64

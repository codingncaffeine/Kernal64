#include "disk.h"

#include <algorithm>
#include <cstring>

namespace k64 {

namespace {

// G1: each 4-bit group is recorded as 5 bits with no more than two zeros in a row.
constexpr uint8_t kGcr[16] = {0x0A, 0x0B, 0x12, 0x13, 0x0E, 0x0F, 0x16, 0x17,
                              0x09, 0x19, 0x1A, 0x1B, 0x0D, 0x1D, 0x1E, 0x15};

// G2: bytes per revolution per zone at 300 rpm (the zone's bit clock, D1, over 200 ms)
// and the gap the DOS leaves after each sector's data block.
constexpr int kTrackBytes[4] = {6250, 6666, 7142, 7692};
// G3 (PRIMARY: measured on a disk the DOS ROM formatted in our drive, PLAN D24): the gap after
// each data block, by zone.
constexpr int kTailGap[4] = {9, 11, 17, 8};
// G5 (same measurement): where sector 0 starts, as a fraction of a revolution: 0.152 on track 1,
// then each track this much further on than the one before, by its zone (the DOS's format
// timing: write, verify, step).
constexpr double kFirstStart = 0.152;
constexpr double kSkew[4] = {0.293, 0.0897, 0.8883, 0.6869};
constexpr int kHeaderGap = 9;
constexpr int kSyncBytes = 5;

// G4: the D64 error byte's codes that change what is recorded.
enum : uint8_t { kErrHeader = 2, kErrSync = 3, kErrNoData = 4, kErrDataSum = 5, kErrHeaderSum = 9, kErrId = 11 };

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

} // namespace

GcrDisk blankDisk() {
    GcrDisk d;
    for (int i = 0; i < GcrDisk::kHalfTracks; ++i) d.zone[static_cast<size_t>(i)] = static_cast<uint8_t>(trackZone(std::min(42, (i + 2) / 2)));
    return d;
}

int zoneTrackBytes(int zone) { return kTrackBytes[zone & 3]; }

int trackZone(int track) { return (track < 31) + (track < 25) + (track < 18); }

int sectorsOnTrack(int track) {
    static constexpr int kSectors[4] = {17, 18, 19, 21};
    return kSectors[trackZone(track)];
}

void gcrEncode4(const uint8_t in[4], uint8_t out[5]) {
    uint64_t bits = 0;
    for (int i = 0; i < 4; ++i) bits = (bits << 10) | (static_cast<uint64_t>(kGcr[in[i] >> 4]) << 5) | kGcr[in[i] & 15];
    for (int i = 0; i < 5; ++i) out[i] = static_cast<uint8_t>(bits >> (32 - 8 * i));
}

// G3: a sector as the DOS records it: SYNC, header block (8 bytes -> 10), header gap, SYNC,
// data block ($07, 256 bytes, checksum, $00 $00 = 260 bytes -> 325), tail gap.
static void encodeSector(const uint8_t* data, int track, int sector, uint8_t id1, uint8_t id2, uint8_t err,
                         std::vector<uint8_t>& out) {
    auto put4 = [&out](uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        const uint8_t in[4] = {a, b, c, d};
        uint8_t g[5];
        gcrEncode4(in, g);
        out.insert(out.end(), g, g + 5);
    };
    const uint8_t sync = err == kErrSync ? 0x55 : 0xFF;
    const uint8_t idMix = err == kErrId ? 0xFF : 0x00;
    out.insert(out.end(), kSyncBytes, sync);
    uint8_t hsum = static_cast<uint8_t>(sector ^ track ^ id2 ^ id1 ^ idMix);
    if (err == kErrHeaderSum) hsum ^= 0xFF;
    put4(err == kErrHeader ? 0xFF : 0x08, hsum, static_cast<uint8_t>(sector), static_cast<uint8_t>(track));
    put4(id2, static_cast<uint8_t>(id1 ^ idMix), 0x0F, 0x0F);
    out.insert(out.end(), kHeaderGap, 0x55);
    out.insert(out.end(), kSyncBytes, sync);
    uint8_t block[260];
    // A missing data block keeps its bytes but loses its mark; $00 begins with a 0 bit in GCR,
    // so it does not run into the SYNC before it.
    block[0] = err == kErrNoData ? 0x00 : 0x07;
    std::memcpy(block + 1, data, 256);
    uint8_t dsum = 0;
    for (int i = 0; i < 256; ++i) dsum ^= data[i];
    block[257] = err == kErrDataSum ? static_cast<uint8_t>(dsum ^ 0xFF) : dsum;
    block[258] = block[259] = 0;
    for (int i = 0; i < 260; i += 4) put4(block[i], block[i + 1], block[i + 2], block[i + 3]);
}

bool parseD64(const uint8_t* data, size_t size, GcrDisk& out, std::string& error) {
    int tracks = 0;
    bool errors = false;
    switch (size) {
    case 174848: tracks = 35; break;
    case 175531: tracks = 35; errors = true; break;
    case 196608: tracks = 40; break;
    case 197376: tracks = 40; errors = true; break;
    case 205312: tracks = 42; break;
    case 206114: tracks = 42; errors = true; break;
    default: error = "not a D64 image (size " + std::to_string(size) + ")"; return false;
    }
    int totalSectors = 0;
    for (int t = 1; t <= tracks; ++t) totalSectors += sectorsOnTrack(t);
    // The disk ID is the one in the directory's first sector (track 18, sector 0, $A2-$A3).
    size_t bam = 0;
    for (int t = 1; t < 18; ++t) bam += static_cast<size_t>(sectorsOnTrack(t)) * 256;
    const uint8_t id1 = data[bam + 0xA2], id2 = data[bam + 0xA3];

    out = GcrDisk{};
    size_t offset = 0;
    int sectorIndex = 0;
    double start = kFirstStart;
    for (int t = 1; t <= tracks; ++t) {
        if (t > 1) start = start + kSkew[trackZone(t)] - static_cast<int>(start + kSkew[trackZone(t)]);
        const int z = trackZone(t);
        std::vector<uint8_t> track;
        track.reserve(static_cast<size_t>(kTrackBytes[z]));
        for (int s = 0; s < sectorsOnTrack(t); ++s, ++sectorIndex, offset += 256) {
            const uint8_t err = errors ? data[static_cast<size_t>(totalSectors) * 256 + static_cast<size_t>(sectorIndex)] : 1;
            encodeSector(data + offset, t, s, id1, id2, err, track);
            track.insert(track.end(), static_cast<size_t>(kTailGap[z]), 0x55);
        }
        track.resize(static_cast<size_t>(kTrackBytes[z]), 0x55);
        std::rotate(track.begin(), track.end() - static_cast<long>(start * static_cast<double>(track.size())), track.end());
        const int i = GcrDisk::index(2 * t);
        out.tracks[static_cast<size_t>(i)] = std::move(track);
        out.zone[static_cast<size_t>(i)] = static_cast<uint8_t>(z);
    }
    // Half-tracks between and beyond: nothing recorded, the zone of the track below.
    for (int i = 0; i < GcrDisk::kHalfTracks; ++i)
        if (out.tracks[static_cast<size_t>(i)].empty())
            out.zone[static_cast<size_t>(i)] = static_cast<uint8_t>(trackZone(std::min(42, (i + 2) / 2)));
    return true;
}

bool parseG64(const uint8_t* data, size_t size, GcrDisk& out, std::string& error) {
    if (size < 12 || std::memcmp(data, "GCR-1541", 8) != 0) { error = "not a G64 image"; return false; }
    const int halfTracks = data[9];
    const size_t tables = 12 + static_cast<size_t>(halfTracks) * 8;
    if (data[8] != 0 || halfTracks < 1 || halfTracks > GcrDisk::kHalfTracks || size < tables) {
        error = "G64 header not understood";
        return false;
    }
    out = GcrDisk{};
    for (int i = 0; i < halfTracks; ++i) {
        const uint32_t at = le32(data + 12 + 4 * i);
        const uint32_t speed = le32(data + 12 + 4 * halfTracks + 4 * i);
        // A speed entry above 3 points at a per-byte speed map: not supported yet; the track's
        // standard zone is used.
        out.zone[static_cast<size_t>(i)] = static_cast<uint8_t>(speed <= 3 ? speed : trackZone(std::min(42, (i + 2) / 2)));
        if (!at) continue;
        if (at + 2 > size || at + 2 + le16(data + at) > size) { error = "G64 track outside the file"; return false; }
        const uint16_t len = le16(data + at);
        out.tracks[static_cast<size_t>(i)].assign(data + at + 2, data + at + 2 + len);
    }
    return true;
}

std::vector<uint8_t> writeG64(const GcrDisk& disk) {
    size_t slot = 7928;
    for (const auto& t : disk.tracks) slot = std::max(slot, t.size());
    const int n = GcrDisk::kHalfTracks;
    std::vector<uint8_t> out(12 + static_cast<size_t>(n) * 8, 0);
    std::memcpy(out.data(), "GCR-1541", 8);
    out[8] = 0;
    out[9] = static_cast<uint8_t>(n);
    out[10] = static_cast<uint8_t>(slot);
    out[11] = static_cast<uint8_t>(slot >> 8);
    auto put32 = [&out](size_t at, uint32_t v) { for (int k = 0; k < 4; ++k) out[at + static_cast<size_t>(k)] = static_cast<uint8_t>(v >> (8 * k)); };
    for (int i = 0; i < n; ++i) {
        const auto& t = disk.tracks[static_cast<size_t>(i)];
        put32(12 + 4 * static_cast<size_t>(n) + 4 * static_cast<size_t>(i), disk.zone[static_cast<size_t>(i)]);
        if (t.empty()) continue;
        put32(12 + 4 * static_cast<size_t>(i), static_cast<uint32_t>(out.size()));
        out.push_back(static_cast<uint8_t>(t.size()));
        out.push_back(static_cast<uint8_t>(t.size() >> 8));
        out.insert(out.end(), t.begin(), t.end());
        out.insert(out.end(), slot - t.size(), 0);
    }
    return out;
}

bool parseDisk(const uint8_t* data, size_t size, GcrDisk& out, std::string& error) {
    if (size >= 8 && std::memcmp(data, "GCR-1541", 8) == 0) return parseG64(data, size, out, error);
    return parseD64(data, size, out, error);
}

} // namespace k64

#include "disk.h"

#include <algorithm>
#include <cstdio>
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

namespace {

// What one revolution holds, read as the DOS would: after each SYNC (G1: ten or more 1 bits;
// the track is byte-framed after a SYNC, as the drive frames it), a sector header ($08, checksum,
// sector, track, ID; checksum = XOR of the four) or a data block ($07, 256 bytes, XOR checksum).
struct TrackScan {
    int syncs = 0, headersGood = 0, headersBad = 0, dataGood = 0, dataBad = 0;
    int otherTrack = -1;            // a track number other than the expected one, from a good header
    uint64_t sectors = 0;           // the sector numbers of good headers (bit per sector, up to 63)
};

// 5 GCR bytes -> 4 bytes; false if a 5-bit group is no GCR code.
bool gcrDecode5(const uint8_t* in, uint8_t out[4]) {
    static int8_t inv[32];
    static bool ready = false;
    if (!ready) { std::fill(std::begin(inv), std::end(inv), int8_t(-1)); for (int i = 0; i < 16; ++i) inv[kGcr[i]] = int8_t(i); ready = true; }
    uint64_t bits = 0;
    for (int i = 0; i < 5; ++i) bits = (bits << 8) | in[i];
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
        const int hi = inv[(bits >> (35 - 10 * i)) & 31], lo = inv[(bits >> (30 - 10 * i)) & 31];
        ok = ok && hi >= 0 && lo >= 0;
        out[i] = static_cast<uint8_t>(((hi & 15) << 4) | (lo & 15));
    }
    return ok;
}

// The kind of block that starts at b (just after a SYNC): 8 = sector header, 7 = data block, 0 = other.
int blockKind(const uint8_t* b) {
    uint8_t first[4];
    gcrDecode5(b, first);
    return first[0] == 0x08 ? 8 : first[0] == 0x07 ? 7 : 0;
}
// Whether the block of that kind at b has valid GCR codes and a matching checksum.
bool blockGood(const uint8_t* b, int kind) {
    if (kind == 8) {
        uint8_t h[8];
        const bool codes = gcrDecode5(b, h) && gcrDecode5(b + 5, h + 4);
        return codes && h[1] == static_cast<uint8_t>(h[2] ^ h[3] ^ h[4] ^ h[5]);
    }
    uint8_t d[260];
    bool codes = true;
    for (int g = 0; g < 65; ++g) codes = gcrDecode5(b + 5 * g, d + 4 * g) && codes;
    uint8_t x = 0;
    for (int k = 1; k <= 256; ++k) x ^= d[k];
    return codes && x == d[257];
}

TrackScan scanTrack(const std::vector<uint8_t>& t, int track) {
    TrackScan r;
    const size_t n = t.size();
    if (n < 16) return r;
    auto at = [&](size_t i) { return t[i % n]; };
    for (size_t i = 0; i < n; ++i) {
        // A SYNC ends at i: the byte before is all 1s with at least two more 1s before it.
        if (at(i) == 0xFF || at(i + n - 1) != 0xFF || (at(i + n - 2) & 3) != 3) continue;
        ++r.syncs;
        uint8_t raw[325];
        for (size_t k = 0; k < 325; ++k) raw[k] = at(i + k);
        uint8_t first[4];
        gcrDecode5(raw, first);
        if (first[0] == 0x08) {
            uint8_t h[8];
            const bool codes = gcrDecode5(raw, h) && gcrDecode5(raw + 5, h + 4);
            if (codes && h[1] == static_cast<uint8_t>(h[2] ^ h[3] ^ h[4] ^ h[5])) {
                ++r.headersGood;
                if (h[2] < 64) r.sectors |= uint64_t(1) << h[2];
                if (h[3] != track) r.otherTrack = h[3];
            } else {
                ++r.headersBad;
            }
        } else if (first[0] == 0x07) {
            uint8_t d[260];
            bool codes = true;
            for (int g = 0; g < 65; ++g) codes = gcrDecode5(raw + 5 * g, d + 4 * g) && codes;
            uint8_t x = 0;
            for (int k = 1; k <= 256; ++k) x ^= d[k];
            if (codes && x == d[257]) ++r.dataGood; else ++r.dataBad;
        }
    }
    return r;
}

std::string sectorList(uint64_t bits) {
    std::string s;
    for (int a = 0; a < 64; ++a) {
        if (!(bits >> a & 1)) continue;
        int b = a;
        while (b + 1 < 64 && (bits >> (b + 1) & 1)) ++b;
        if (!s.empty()) s += ',';
        s += std::to_string(a);
        if (b > a) s += '-' + std::to_string(b);
        a = b;
    }
    return s.empty() ? "none" : s;
}

} // namespace

// G9: one revolution out of a raw read. The read is byte-framed by the drive (a SYNC realigns
// the framing), so what follows a SYNC comes back byte for byte one revolution later. The
// revolution is the distance, near the zone's nominal length, at which the 16 bytes after the
// first SYNC that is followed by a sector header ($52 = GCR of $08's first five bits) recur;
// failing that, after any SYNC; failing that, any 64-byte window; failing that, the nominal
// length from the start.
enum class RevFound { Header, Sync, Window, Nominal };
struct Revolution { size_t start = 0, length = 0; RevFound how = RevFound::Nominal; };
static Revolution findRevolution(const uint8_t* raw, size_t len, int zone) {
    const size_t nominal = static_cast<size_t>(kTrackBytes[zone & 3]);
    const size_t lo = nominal * 95 / 100, hi = std::min(len - 16, nominal * 105 / 100);
    auto syncEnd = [&](size_t from, bool header) -> size_t {
        for (size_t i = from; i + 1 < len; ++i)
            if (raw[i] == 0xFF && raw[i + 1] != 0xFF && i > 0 && raw[i - 1] == 0xFF && (!header || raw[i + 1] == 0x52)) return i + 1;
        return len;
    };
    for (bool header : {true, false}) {
        for (size_t s = syncEnd(0, header); s < len && s + lo + 16 < len; s = syncEnd(s + 1, header)) {
            size_t best = 0, bestDist = SIZE_MAX;
            for (size_t p = lo; p <= hi && s + p + 16 <= len; ++p) {
                if (std::memcmp(raw + s, raw + s + p, 16) != 0) continue;
                const size_t dist = p > nominal ? p - nominal : nominal - p;
                if (dist < bestDist) { best = p; bestDist = dist; }
            }
            if (best) {
                // Start the revolution at the SYNC before s, so the track begins with a whole SYNC.
                size_t start = s;
                while (start > 0 && raw[start - 1] == 0xFF) --start;
                if (start + best > len) start = s;
                return {start, best, header ? RevFound::Header : RevFound::Sync};
            }
        }
    }
    // A long track can leave too little of the read after its first SYNC for the repeat to
    // start there. Between SYNCs the framing is the same a revolution later too: a longer
    // window (64 bytes), starting anywhere the read still holds one revolution after it.
    for (size_t x = 0; x + lo + 64 <= len; ++x) {
        size_t best = 0, bestDist = SIZE_MAX;
        for (size_t p = lo; p <= hi + 16 && x + p + 64 <= len; ++p) {
            if (std::memcmp(raw + x, raw + x + p, 64) != 0) continue;
            const size_t dist = p > nominal ? p - nominal : nominal - p;
            if (dist < bestDist) { best = p; bestDist = dist; }
        }
        if (best) return {x, best, RevFound::Window};
    }
    return {0, std::min(len, nominal), RevFound::Nominal};
}

// A raw read holds more than one revolution, so the part read twice gives a second copy of the
// blocks in it, exactly one revolution away. A sector header or data block that is bad in the
// revolution kept but good in its other copy was misread by the dumping drive on one pass: it is
// taken from the good copy (PLAN D30). A block bad in every copy is left as it is (a damaged
// disk, or protection that wants it bad). Only where the revolution's length was found, so the
// copies line up: a revolution measured from a SYNC. Returns the number of blocks taken from the
// other copy.
static int repairFromOtherPass(std::vector<uint8_t>& track, const uint8_t* raw, size_t len, size_t start) {
    const size_t n = track.size();
    int repaired = 0;
    for (size_t i = 0; i < n; ++i) {
        if (track[i] == 0xFF || track[(i + n - 1) % n] != 0xFF || (track[(i + n - 2) % n] & 3) != 3) continue;
        uint8_t blk[325];
        for (size_t k = 0; k < 325; ++k) blk[k] = track[(i + k) % n];
        const int kind = blockKind(blk);
        if (!kind || blockGood(blk, kind)) continue;
        const size_t blen = kind == 8 ? 10 : 325;
        // The same block in the raw read: this pass (unwrapped), and one or two revolutions on or back.
        for (long k = -2; k <= 2; ++k) {
            const long r = static_cast<long>(start + i) + k * static_cast<long>(n);
            if (r < 0 || static_cast<size_t>(r) + blen > len) continue;
            if (!blockGood(raw + r, kind)) continue;
            for (size_t b = 0; b < blen; ++b) track[(i + b) % n] = raw[static_cast<size_t>(r) + b];
            ++repaired;
            break;
        }
    }
    return repaired;
}

// One revolution of a NIB track, with misread blocks taken from the read's other pass.
static std::vector<uint8_t> nibTrack(const uint8_t* raw, size_t len, int zone, RevFound* how = nullptr, int* repaired = nullptr) {
    const Revolution r = findRevolution(raw, len, zone);
    std::vector<uint8_t> track(raw + r.start, raw + r.start + r.length);
    const bool anchored = r.how == RevFound::Header || r.how == RevFound::Sync;
    const int fixed = anchored ? repairFromOtherPass(track, raw, len, r.start) : 0;
    if (how) *how = r.how;
    if (repaired) *repaired = fixed;
    return track;
}

bool parseNib(const uint8_t* data, size_t size, GcrDisk& out, std::string& error) {
    if (size < 0x100 || std::memcmp(data, "MNIB-1541-RAW", 13) != 0) { error = "not a NIB image"; return false; }
    out = blankDisk();
    size_t at = 0x100;
    for (size_t e = 0x10; e + 1 < 0x100 && data[e]; e += 2, at += 8192) {
        const int halfTrack = data[e];
        const int zone = data[e + 1] & 3;
        if (at + 8192 > size) { error = "NIB track data outside the file"; return false; }
        if (halfTrack < 2 || halfTrack >= 2 + GcrDisk::kHalfTracks) continue;
        const size_t i = static_cast<size_t>(GcrDisk::index(halfTrack));
        out.tracks[i] = nibTrack(data + at, 8192, zone);
        out.zone[i] = static_cast<uint8_t>(zone);
    }
    return true;
}

std::vector<std::string> describeNib(const uint8_t* data, size_t size) {
    std::vector<std::string> lines;
    if (size < 0x100 || std::memcmp(data, "MNIB-1541-RAW", 13) != 0) { lines.push_back("not a NIB image"); return lines; }
    int tracks = 0, halfTracks = 0;
    std::vector<std::string> badBlocks, notFound, noSync, repairedTracks;
    size_t at = 0x100;
    char buf[320];
    for (size_t e = 0x10; e + 1 < 0x100 && data[e]; e += 2, at += 8192) {
        const int halfTrack = data[e];
        const uint8_t density = data[e + 1];
        if (at + 8192 > size) { lines.push_back("track data outside the file: the image is cut short"); break; }
        const std::string name = std::to_string(halfTrack / 2) + (halfTrack & 1 ? ".5" : "");
        if (halfTrack < 2 || halfTrack >= 2 + GcrDisk::kHalfTracks) {
            lines.push_back("track " + name + ": outside the drive's range, not used");
            continue;
        }
        (halfTrack & 1 ? halfTracks : tracks)++;
        RevFound how = RevFound::Nominal;
        int repaired = 0;
        const std::vector<uint8_t> rev = nibTrack(data + at, 8192, density & 3, &how, &repaired);
        const TrackScan sc = scanTrack(rev, halfTrack / 2);
        const char* found = how == RevFound::Header ? "repeat after a sector header"
                          : how == RevFound::Sync ? "repeat after a SYNC"
                          : how == RevFound::Window ? "repeat of a 64-byte window"
                          : "NOT FOUND, cut at the nominal length";
        std::snprintf(buf, sizeof buf,
                      "track %-4s density $%02X: %zu bytes (nominal %d), revolution: %s; SYNCs %d; headers %d good %d bad (sectors %s); data %d good %d bad",
                      name.c_str(), density, rev.size(), kTrackBytes[density & 3], found, sc.syncs, sc.headersGood, sc.headersBad,
                      sectorList(sc.sectors).c_str(), sc.dataGood, sc.dataBad);
        std::string line = buf;
        if (density & 0x80) line += "; dumper flag $80 (track of all 1s)";
        if (density & 0x40) line += "; dumper flag $40 (no SYNC)";
        if (sc.otherTrack >= 0) line += "; a header names track " + std::to_string(sc.otherTrack);
        if (repaired) line += "; " + std::to_string(repaired) + " misread block(s) taken from the read's other pass";
        if (repaired) repairedTracks.push_back(name);
        lines.push_back(line);
        if (sc.headersBad || sc.dataBad) badBlocks.push_back(name);
        if (how == RevFound::Nominal) notFound.push_back(name);
        if (!sc.syncs) noSync.push_back(name);
    }
    auto join = [](const std::vector<std::string>& v) { std::string s; for (const auto& x : v) s += (s.empty() ? "" : " ") + x; return s.empty() ? std::string("none") : s; };
    std::snprintf(buf, sizeof buf, "summary: %d tracks, %d half-tracks", tracks, halfTracks);
    lines.push_back(buf);
    lines.push_back("  revolution not found (our conversion is unsure): " + join(notFound));
    lines.push_back("  bad header or data checksums (damage in the dump, or protection): " + join(badBlocks));
    lines.push_back("  no SYNC at all (unformatted, or protection): " + join(noSync));
    lines.push_back("  blocks misread on one pass of the dump, taken from the other: " + join(repairedTracks));
    return lines;
}

bool parseDisk(const uint8_t* data, size_t size, GcrDisk& out, std::string& error) {
    if (size >= 8 && std::memcmp(data, "GCR-1541", 8) == 0) return parseG64(data, size, out, error);
    if (size >= 13 && std::memcmp(data, "MNIB-1541-RAW", 13) == 0) return parseNib(data, size, out, error);
    return parseD64(data, size, out, error);
}

} // namespace k64

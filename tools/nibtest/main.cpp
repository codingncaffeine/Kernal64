// nibtest: the NIB conversion on synthetic raw reads (no disk images needed). A blank disk is
// formatted in memory (D64 -> GCR); its track 1 is laid out as a raw read of more than one
// revolution, as a NIB holds it, and blocks are spoiled in one pass or in every pass.
//   - an untouched read converts with every block good
//   - a block misread in one pass is taken from the other (PLAN D30)
//   - a block misread in every pass stays bad
//   - a block read only once and misread stays bad
#include "disk.h"

#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// A NIB with one entry: half-track 2 (track 1) at density 3, its raw read `raw` (8192 bytes).
std::vector<uint8_t> makeNib(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> nib(0x100 + 8192, 0);
    std::memcpy(nib.data(), "MNIB-1541-RAW", 13);
    nib[13] = 3;
    nib[0x10] = 2;
    nib[0x11] = 3;
    std::memcpy(nib.data() + 0x100, raw.data(), 8192);
    return nib;
}

// "data G good B bad" of track 1 in the report.
std::pair<int, int> dataBlocks(const std::vector<uint8_t>& nib, std::string* line = nullptr) {
    for (const auto& l : k64::describeNib(nib.data(), nib.size())) {
        std::smatch m;
        if (l.rfind("track 1 ", 0) == 0 && std::regex_search(l, m, std::regex("data (\\d+) good (\\d+) bad"))) {
            if (line) *line = l;
            return {std::stoi(m[1]), std::stoi(m[2])};
        }
    }
    return {-1, -1};
}

// The offset just after the SYNC of the k-th data block ($07, GCR $55) of the track.
size_t dataBlockAt(const std::vector<uint8_t>& t, int k) {
    for (size_t i = 2; i < t.size(); ++i)
        if (t[i] == 0x55 && t[i - 1] == 0xFF && t[i - 2] == 0xFF && k-- == 0) return i;
    return 0;
}

} // namespace

int main() {
    k64::GcrDisk disk;
    std::string error;
    const std::vector<uint8_t> d64(174848, 0);
    if (!k64::parseD64(d64.data(), d64.size(), disk, error)) { std::printf("FAIL D64: %s\n", error.c_str()); return 1; }
    const std::vector<uint8_t>& track = disk.tracks[static_cast<size_t>(k64::GcrDisk::index(2))];
    const size_t n = track.size();
    // The raw read: 8192 bytes (more than one revolution), from shortly before the track's
    // first data block, so that block falls in the part read twice.
    const size_t from = dataBlockAt(track, 0) - 20;
    std::vector<uint8_t> raw(8192);
    for (size_t i = 0; i < raw.size(); ++i) raw[i] = track[(from + i) % n];

    auto [good, bad] = dataBlocks(makeNib(raw));
    check(good == 21 && bad == 0, "an untouched read: 21 data blocks, all good");

    // A block in the part read twice: raw offsets r and r + n both hold it.
    const size_t first = dataBlockAt(raw, 0);
    check(first + n + 325 <= raw.size(), "the first data block is read twice in the raw read");
    // The revolution kept starts at the first sector header in the read, after this block, so it
    // holds the block's second copy: that copy is the one spoiled.
    std::vector<uint8_t> once = raw;
    once[first + n + 40] ^= 0x10;   // one bit cell flipped in the second pass only
    std::string line;
    auto r1 = dataBlocks(makeNib(once), &line);
    check(r1.first == 21 && r1.second == 0, "a block misread in one pass is taken from the other");
    check(line.find("taken from the read's other pass") != std::string::npos, "and the report says so");

    std::vector<uint8_t> both = once;
    both[first + 40] ^= 0x10;   // the same cell in the first pass too
    auto r2 = dataBlocks(makeNib(both));
    check(r2.first == 20 && r2.second == 1, "a block misread in every pass stays bad");

    // A block in the middle of the read: only one copy of it.
    const size_t middle = dataBlockAt(raw, 10);
    check(middle > 8192 - n && middle + 325 < n, "the 11th data block is read only once");
    std::vector<uint8_t> single = raw;
    single[middle + 40] ^= 0x10;
    auto r3 = dataBlocks(makeNib(single));
    check(r3.first == 20 && r3.second == 1, "a block read once and misread stays bad");

    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

// A floppy disk as the 1541's head sees it: per half-track, the bit stream (GCR) that passes
// under the head in one revolution. Made from D64 images (sectors, encoded here) or read
// from G64 images (the bit stream itself). Rules and sources: local-notes/hw/drive.md G.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace k64 {

struct GcrDisk {
    static constexpr int kHalfTracks = 84;   // half-tracks 2..85 (track 1 = half-track 2) at index 0..83
    // One revolution of each half-track, a byte = 8 bit cells, the first bit in bit 7. Empty:
    // nothing recorded there (no flux changes).
    std::array<std::vector<uint8_t>, kHalfTracks> tracks;
    // The speed zone each half-track was recorded at (0 = tracks 31+, 3 = tracks 1-17).
    std::array<uint8_t, kHalfTracks> zone{};
    bool writeProtected = false;
    // Half-tracks the drive has written since the disk was made or last saved.
    std::array<bool, kHalfTracks> written{};
    bool changed = false;

    static int index(int halfTrack) { return halfTrack - 2; }   // half-track number -> array index
};

// The standard speed zone of a track (1..42): 3 for 1-17, 2 for 18-24, 1 for 25-30, 0 above.
int trackZone(int track);
int sectorsOnTrack(int track);

// D64: 35, 40 or 42 tracks, optionally with one error byte per sector. The sectors are laid out
// as the DOS formats a disk. False, with the reason in `error`, for anything else.
bool parseD64(const uint8_t* data, size_t size, GcrDisk& out, std::string& error);
// G64 ("GCR-1541" header, half-track table, speed table).
bool parseG64(const uint8_t* data, size_t size, GcrDisk& out, std::string& error);
// NIB ("MNIB-1541-RAW", G8): per half-track a raw read of more than one revolution; each
// track's revolution is found where its sector headers repeat (G9).
bool parseNib(const uint8_t* data, size_t size, GcrDisk& out, std::string& error);
// A report on a NIB image, a line per half-track it holds and a summary: how the revolution was
// found, its length, the SYNCs and the sector headers and data blocks decoded with good or bad
// checksums. For telling a damaged or protected dump from a fault in the conversion.
std::vector<std::string> describeNib(const uint8_t* data, size_t size);
// By content: G64 and NIB by their signatures, D64 by its size.
bool parseDisk(const uint8_t* data, size_t size, GcrDisk& out, std::string& error);

// G64 of the disk as it is (G6's layout; each track slot as long as the longest track, at
// least 7928 bytes; half-tracks with nothing recorded have no entry).
std::vector<uint8_t> writeG64(const GcrDisk& disk);

// A blank disk, as out of its box: nothing recorded on any half-track (G7).
GcrDisk blankDisk();

// The bytes per revolution of a track recorded at zone 0-3 (G2).
int zoneTrackBytes(int zone);

// The 4-to-5 bit code (hw/drive.md G1), exposed for tests.
void gcrEncode4(const uint8_t in[4], uint8_t out[5]);

} // namespace k64

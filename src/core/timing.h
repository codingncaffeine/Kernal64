// Machine timing per video chip model.
//
// Lines, cycles per line and clock rates are from the VIC-II chip table and the
// 6510 clock figures (985.248 kHz PAL, 1022.7 kHz NTSC). The dot clock is eight
// pixels per system cycle.
#pragma once

#include <cstdint>

namespace k64 {

enum class VicModel : uint8_t {
    Mos6569,     // PAL-B (6569R3)
    Mos6567R8,   // NTSC-M, 65 cycles per line
    Mos6567R56A, // NTSC-M, 64 cycles per line (early machines)
    Mos8565,     // PAL-B, the later chip generation (later boards)
    Mos8562,     // NTSC-M, the later chip generation
    Mos6569R1,   // PAL-B, first revision (early machines)
};

struct Timing {
    VicModel model;
    int cyclesPerLine;
    int linesPerFrame;
    double clockHz;         // system (φ2) clock
    double squarePixelHz;   // sampling rate at which a pixel is square on this TV system
    const char* name;
    // The VIC-II's X coordinate at the start of cycle 1 (article §3.4 "first X coo. of a line").
    int firstX;
    // The visible picture (article §3.4 table): the first line after vertical blanking, the
    // first vertical blanking line, the visible lines, and in pixels counted from the start
    // of cycle 1, the first visible pixel and the visible width.
    int firstVisibleLine, firstVblankLine, visibleLines, firstVisiblePixel, visibleWidth;
    int mainsHz;            // power-line frequency, fed to the CIAs' TOD inputs
    // Chip generation (hw/vic2.md N1-N6). The 8565/8562 take mode and color register
    // changes without the older chips' delays and show a gray dot on a color write.
    bool newChip = false;
    // The first revisions raise the light pen interrupt only for a pen line that is
    // already low when the frame starts, and have five luma levels instead of nine.
    bool firstRevision = false;
    bool pal = false;       // TV system, for the frontend's region

    constexpr int cyclesPerFrame() const { return cyclesPerLine * linesPerFrame; }
    constexpr double framesPerSecond() const { return clockHz / cyclesPerFrame(); }
    constexpr double dotClockHz() const { return clockHz * 8.0; }
    // Width of one emitted pixel relative to its height on a 4:3 TV.
    constexpr double pixelAspect() const { return squarePixelHz / dotClockHz(); }
};

// PAL: 17.734475 MHz crystal / 18 = 985,248.6 Hz. Square pixels on 625-line PAL
// are sampled at 14.75 MHz for 768 pixels per active line; the VIC-II's 7.88 MHz
// dot clock against half of that gives its pixel aspect.
// NTSC: 14.31818 MHz crystal / 14 = 1,022,727.1 Hz. Square pixels on 525-line NTSC
// are sampled at 12.2727 MHz; same reasoning.
// Visible pixel: first visible X ($1E0 / $1E9 / $1E8) minus the first X of a line.
constexpr Timing kTimingPal      { VicModel::Mos6569,     63, 312, 17734475.0 / 18.0, 14750000.0 / 2.0, "6569 (PAL)",
                                   0x194, 16, 300, 284, 0x1E0 - 0x194, 403, 50, false, false, true };
constexpr Timing kTimingNtsc     { VicModel::Mos6567R8,   65, 263, 14318181.8 / 14.0, 12272727.3 / 2.0, "6567R8 (NTSC)",
                                   0x19C, 41, 13, 235, 0x1E9 - 0x19C, 418, 60, false, false, false };
constexpr Timing kTimingNtscOld  { VicModel::Mos6567R56A, 64, 262, 14318181.8 / 14.0, 12272727.3 / 2.0, "6567R56A (old NTSC)",
                                   0x19C, 41, 13, 234, 0x1E8 - 0x19C, 411, 60, false, true, false };
// The later generation and the first PAL revision keep the raster of the chip they replace.
constexpr Timing kTimingPalNew   { VicModel::Mos8565,     63, 312, 17734475.0 / 18.0, 14750000.0 / 2.0, "8565 (PAL)",
                                   0x194, 16, 300, 284, 0x1E0 - 0x194, 403, 50, true, false, true };
constexpr Timing kTimingNtscNew  { VicModel::Mos8562,     65, 263, 14318181.8 / 14.0, 12272727.3 / 2.0, "8562 (NTSC)",
                                   0x19C, 41, 13, 235, 0x1E9 - 0x19C, 418, 60, true, false, false };
constexpr Timing kTimingPalOld   { VicModel::Mos6569R1,   63, 312, 17734475.0 / 18.0, 14750000.0 / 2.0, "6569R1 (old PAL)",
                                   0x194, 16, 300, 284, 0x1E0 - 0x194, 403, 50, false, true, true };

constexpr const Timing& timingFor(VicModel m) {
    switch (m) {
    case VicModel::Mos6567R8: return kTimingNtsc;
    case VicModel::Mos6567R56A: return kTimingNtscOld;
    case VicModel::Mos8565: return kTimingPalNew;
    case VicModel::Mos8562: return kTimingNtscNew;
    case VicModel::Mos6569R1: return kTimingPalOld;
    case VicModel::Mos6569: break;
    }
    return kTimingPal;
}

// The model for a name as the core option and the tools spell it; false if unknown.
constexpr bool vicModelFromName(const char* name, VicModel& out) {
    constexpr struct { const char* name; VicModel model; } kNames[] = {
        {"pal", VicModel::Mos6569},      {"pal-new", VicModel::Mos8565},   {"pal-old", VicModel::Mos6569R1},
        {"ntsc", VicModel::Mos6567R8},   {"ntsc-new", VicModel::Mos8562},  {"ntsc-old", VicModel::Mos6567R56A},
    };
    for (const auto& n : kNames) {
        const char* a = n.name;
        const char* b = name;
        while (*a && *a == *b) { ++a; ++b; }
        if (!*a && !*b) { out = n.model; return true; }
    }
    return false;
}
constexpr const char* kVicModelNames = "pal|pal-new|pal-old|ntsc|ntsc-new|ntsc-old";

// Largest raster any model produces, in pixels: 65 cycles x 8, 312 lines.
constexpr int kMaxRasterWidth = 65 * 8;
constexpr int kMaxRasterHeight = 312;

} // namespace k64

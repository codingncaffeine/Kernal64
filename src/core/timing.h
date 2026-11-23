// Machine timing per video chip model.
//
// Lines, cycles per line and clock rates are from the VIC-II chip table and the
// 6510 clock figures (985.248 kHz PAL, 1022.7 kHz NTSC). The dot clock is eight
// pixels per system cycle.
#pragma once

#include <cstdint>

namespace k64 {

enum class VicModel : uint8_t {
    Mos6569,     // PAL-B
    Mos6567R8,   // NTSC-M, 65 cycles per line
    Mos6567R56A, // NTSC-M, 64 cycles per line (early machines)
};

struct Timing {
    VicModel model;
    int cyclesPerLine;
    int linesPerFrame;
    double clockHz;         // system (φ2) clock
    double squarePixelHz;   // sampling rate at which a pixel is square on this TV system
    const char* name;

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
constexpr Timing kTimingPal      { VicModel::Mos6569,     63, 312, 17734475.0 / 18.0, 14750000.0 / 2.0, "6569 (PAL)" };
constexpr Timing kTimingNtsc     { VicModel::Mos6567R8,   65, 263, 14318181.8 / 14.0, 12272727.3 / 2.0, "6567R8 (NTSC)" };
constexpr Timing kTimingNtscOld  { VicModel::Mos6567R56A, 64, 262, 14318181.8 / 14.0, 12272727.3 / 2.0, "6567R56A (old NTSC)" };

constexpr const Timing& timingFor(VicModel m) {
    return m == VicModel::Mos6569 ? kTimingPal : m == VicModel::Mos6567R8 ? kTimingNtsc : kTimingNtscOld;
}

// Largest raster any model produces, in pixels: 65 cycles x 8, 312 lines.
constexpr int kMaxRasterWidth = 65 * 8;
constexpr int kMaxRasterHeight = 312;

} // namespace k64

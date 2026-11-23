// MOS 6567/6569 VIC-II: raster timing, bus accesses, text/bitmap display, border,
// interrupts and DRAM refresh.
//
// The model follows Bauer, "The MOS 6567/6569 video controller (VIC-II) and its
// application in the Commodore 64" (1996); section numbers in comments are that
// article's. Every rule and its source: local-notes/hw/vic2.md.
//
// One call sequence per system cycle, driven by the machine:
//   phase1()        the VIC-II's first-phase access and its internal state for the cycle;
//                   decides BA and whether AEC stays low in the second phase
//   (the 6510's second-phase access, when AEC is high)
//   phase2(cpuData) the c-access that a bad line makes in the second phase
//   render()        the cycle's eight pixels into the raster buffer
//   endCycle()      advances the raster position
#pragma once

#include "timing.h"

#include <array>
#include <cstdint>
#include <vector>

namespace k64 {

// What the VIC-II sees of the machine.
class VicHost {
public:
    virtual uint8_t vicRead(uint16_t addr14) = 0;   // through the PLA, in the selected bank
    virtual uint8_t colorRead(uint16_t addr10) = 0; // 4-bit color RAM
protected:
    ~VicHost() = default;
};

class Vic2 {
public:
    Vic2(VicHost& host, const Timing& t);

    void setTiming(const Timing& t);
    void reset();

    void phase1();
    void phase2(uint8_t cpuData);
    void render();
    void endCycle();   // returns to cycle 1 of the next line after the last cycle

    bool ba() const { return s_.baLow == 0; }       // true = bus available to the CPU
    bool aecLow() const { return s_.aecLow != 0; }  // the VIC-II keeps the second phase
    bool irq() const { return (s_.irqLatch & s_.irqEnable & 0x0F) != 0; }
    uint8_t lastByte() const { return s_.lastByte; } // what the first-phase access read

    uint8_t read(unsigned reg);           // reg = address & $3F; side effects ($D01E/$D01F)
    uint8_t peek(unsigned reg) const;     // without side effects
    void write(unsigned reg, uint8_t v);

    int line() const { return s_.line; }
    int cycleInLine() const { return s_.cycle; }   // 1-based, as in the article

    // Pixel buffer: kMaxRasterWidth x kMaxRasterHeight palette indices, row = raster line,
    // column = pixel counted from the start of cycle 1.
    const uint8_t* pixels() const { return pixels_.data(); }

    // The X coordinate (sprite coordinate system) of a pixel counted from cycle 1.
    int xAt(int pixel) const;

#pragma pack(push, 1)
    struct State {
        int32_t line = 0, cycle = 1;     // raster position; cycle is 1-based
        uint8_t reg[0x40] = {};          // register file as written ($D019/$D01A kept apart)
        uint16_t rasterCompare = 0;
        uint8_t irqLatch = 0, irqEnable = 0;
        uint8_t collSprite = 0, collData = 0, lpx = 0, lpy = 0;
        uint8_t lastByte = 0;            // byte read in the last first phase (open bus)
        uint8_t baLow = 0, aecLow = 0, baRun = 0;
        uint8_t denSeen = 0;             // DEN was set in some cycle of line $30 (§3.5)
        uint8_t badLine = 0;             // Bad Line Condition in this cycle
        uint8_t display = 0;             // display state (else idle state), §3.7.1
        uint16_t vc = 0, vcBase = 0;     // §3.7.2
        uint8_t rc = 0, vmli = 0;
        uint8_t ref = 0xFF;              // refresh counter, §3.13
        uint16_t matrix[40] = {};        // video matrix/color line: color << 8 | pointer
        uint8_t spritePtr[8] = {};       // last p-access per sprite
        // Graphics sequencer, §3.7.3: queued g-access results, each loaded into the
        // shift register at its pixel.
        struct Pending { int16_t at; uint8_t data; uint16_t cdata; uint8_t valid; } pend[2] = {};
        uint8_t shData = 0;              // the byte in the shift register
        uint16_t shC = 0;                // its video matrix data
        int8_t shPixel = 8;              // pixel index within it; 8 = run empty
        uint8_t mainBorder = 1, vertBorder = 1;   // §3.9
    };
#pragma pack(pop)
    const State& state() const { return s_; }
    void setState(const State& s) { s_ = s; }

private:
    uint16_t gAddress() const;
    void pixel(int px, int x, uint8_t* out);
    int spritePCycle(int n) const;

    VicHost& host_;
    const Timing* t_;
    State s_;
    std::vector<uint8_t> pixels_;
};

// The 16 colors as XRGB8888, computed from the luma/chroma model of the chip.
const std::array<uint32_t, 16>& vicPalette();

} // namespace k64

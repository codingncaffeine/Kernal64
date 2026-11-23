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
//   render()        eight pixels through the graphics data path, the border unit and the
//                   output stage, with the registers as the CPU's access left them
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
    virtual bool vicIsCharRom(uint16_t addr14) const = 0;   // the address selects the character ROM
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

    // The light pen input (CIA 1 port B bit 4 on the C64), called when the line may have
    // changed: while it is low and nothing was latched in this frame, the position is
    // latched in the next cycle (§3.11; hw/vic2.md rule 16).
    void setLightPen(bool low) {
        s_.lpLow = low ? 1 : 0;
        if (low) s_.lpPending = 1;
    }

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
        // Sprites (hw/vic2.md S1-S4). One bit per sprite in the masks.
        uint8_t mc[8] = {}, mcBase[8] = {};      // data counters, §3.8.1
        uint8_t spriteDma = 0;           // DMA on
        uint8_t spriteDisplay = 0;       // display on
        uint8_t expFlop = 0xFF;          // Y expansion flip-flops
        uint32_t spriteData[8] = {};     // the three bytes fetched for the line
        uint8_t busPhi2 = 0xFF;          // the chip's own data bus in the second phase ($FF unless the CPU used a register)
        // Sprite drawing: shift registers and the per-pixel state of S3.
        uint32_t sShift[8] = {};
        uint8_t sPixel[8] = {};
        uint16_t sXPipe[8] = {};         // X coordinates as the comparators see them
        uint8_t sPending = 0, sActive = 0, sHalt = 0;
        uint8_t sExpFlops = 0, sMcFlops = 0;      // X expansion / multicolor flip-flops
        uint8_t sMcBits = 0, sPriBits = 0, sExpXBits = 0;   // $D01C, $D01B, $D01D as piped
        uint8_t clearColl = 0;           // $1E / $1F: that collision register was read
        // Graphics data path (hw/vic2.md P4-P7): the g-access byte and its matrix data on
        // their way to the shift register, the shift register, and the mode bits as the
        // sequencer sees them.
        uint8_t gbuf = 0;                // byte of the last g-access
        uint8_t gPipe = 0;               // what the shift register takes at its next load
        uint8_t gPipeValid = 0;          // the coming g-access enters the path
        uint16_t cPipe0 = 0, cPipe1 = 0; // matrix data: color << 8 | pointer
        uint8_t xscrollPipe = 0;         // the pixel of a render step at which the load happens
        uint8_t dmli = 0;                // column of the matrix line entering the path
        uint8_t shData = 0;              // the byte in the shift register
        uint16_t shC = 0;                // its video matrix data
        uint8_t mcFlop = 0;              // multicolor flip-flop: set = take a 2-bit pixel now
        uint8_t pixelReg = 0;            // the pixel value formed last (0-3)
        uint8_t mode11 = 0;              // ECM/BMM ($D011 bits 6, 5) as piped
        uint8_t mode16 = 0, mode16b = 0; // MCM ($D016 bit 4) for the color decode / pixel forming
        uint8_t reg11Prev = 0;           // $D011 one cycle ago (g-access address, P10)
        // Output stage (P8): rendered pixels wait 8 pixels; a value >= $20 is a reference
        // to color register $D0xx, resolved just before output.
        uint8_t ring[8] = {};
        uint8_t creg[16] = {};           // $D020-$D02E as the output stage sees them
        uint8_t colorLatchReg = 0xFF, colorLatchVal = 0;   // write reaching creg at the next step
        uint8_t colorPendReg = 0xFF, colorPendVal = 0;     // write of this cycle
        uint8_t mainBorder = 1, vertBorder = 1;   // §3.9
        uint8_t vertPending = 1;         // a bottom match waiting for cycle 1 / the left comparator
        uint8_t rasterMatched = 0;       // the raster compare matched in the last cycle
        uint8_t lpLow = 0, lpPending = 0, lpTriggered = 0;   // light pen: line, latch due, latched this frame
    };
#pragma pack(pop)
    const State& state() const { return s_; }
    void setState(const State& s) { s_ = s; }

private:
    uint16_t gAddress(uint8_t r11) const;
    uint8_t graphicsPixel(int i);
    void spritePixel(int i, int x, uint8_t* px);
    uint8_t spriteBaMask(int cycle) const;
    void buildSlots();
    void latchLightPen(bool held);
    int spritePCycle(int n) const;

    VicHost& host_;
    const Timing* t_;
    State s_;
    uint8_t pri_[8] = {};            // foreground flags of the step's graphics pixels
    // Per cycle of the line (1-based), from the model's schedule: the sprite whose pointer
    // and first data byte are fetched in the cycle, the sprite whose second and third
    // bytes are (-1: none), and the sprites whose DMA holds BA low in it.
    int8_t ptrSprite_[66] = {}, dataSprite_[66] = {};
    uint8_t baMask_[66] = {};
    std::vector<uint8_t> pixels_;
};

// The 16 colors as XRGB8888, computed from the luma/chroma model of the chip.
// firstRevision: the five luma levels of the first chip revisions instead of nine.
const std::array<uint32_t, 16>& vicPalette(bool firstRevision = false);

} // namespace k64

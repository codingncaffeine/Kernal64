// 8255 programmable peripheral interface, mode 0 (the only mode the CMD HD's ROM sets).
// Rules from the 82C55A data sheet (gitignore/refs/cmdhd/82c55a.pdf; Intel's 8255A scan
// agrees): after RESET every port is an input and the 24 lines are held high; a mode word
// (bit 7 set) sets the directions and clears every output register; a word with bit 7 clear
// sets or clears one bit of port C (bits 3-1 = which, bit 0 = the value); in mode 0 outputs
// are latched and a read of an output port returns its latch. Notes: local-notes/hw/cmdhd.md.
//
// Modes 1 and 2 (strobed and bidirectional transfers) are not built: a mode word asking for
// them is kept as written and its ports act as in mode 0.
#pragma once

#include <cstdint>

namespace k64 {

class Ppi8255 {
public:
    // The levels on the pins of the input ports (the host's side). Unused bits: 1.
    struct Pins { uint8_t a = 0xFF, b = 0xFF, c = 0xFF; };

#pragma pack(push, 1)
    struct State {
        uint8_t control = 0x9B;   // after RESET: mode 0, every port an input
        uint8_t a = 0, b = 0, c = 0;   // the output latches
    };
#pragma pack(pop)

    void reset() { s_ = State{}; }

    uint8_t read(unsigned reg, const Pins& pins) const {
        switch (reg & 3) {
        case 0: return inA() ? pins.a : s_.a;
        case 1: return inB() ? pins.b : s_.b;
        case 2: return static_cast<uint8_t>((pins.c & inCMask()) | (s_.c & ~inCMask()));
        default: return 0xFF;   // the control register cannot be read
        }
    }
    void write(unsigned reg, uint8_t v) {
        switch (reg & 3) {
        case 0: s_.a = v; break;
        case 1: s_.b = v; break;
        case 2: s_.c = v; break;
        default:
            if (v & 0x80) { s_.control = v; s_.a = s_.b = s_.c = 0; }
            else {
                const uint8_t bit = static_cast<uint8_t>(1u << ((v >> 1) & 7));
                s_.c = (v & 1) ? static_cast<uint8_t>(s_.c | bit) : static_cast<uint8_t>(s_.c & ~bit);
            }
            break;
        }
    }

    // What the chip's pins show: a driven output's latch, an input held high.
    uint8_t pinsA() const { return inA() ? 0xFF : s_.a; }
    uint8_t pinsB() const { return inB() ? 0xFF : s_.b; }
    uint8_t pinsC() const { return static_cast<uint8_t>(inCMask() | (s_.c & ~inCMask())); }

    const State& state() const { return s_; }
    void setState(const State& s) { s_ = s; }

private:
    bool inA() const { return s_.control & 0x10; }
    bool inB() const { return s_.control & 0x02; }
    uint8_t inCMask() const {
        return static_cast<uint8_t>(((s_.control & 0x08) ? 0xF0 : 0) | ((s_.control & 0x01) ? 0x0F : 0));
    }
    State s_;
};

} // namespace k64

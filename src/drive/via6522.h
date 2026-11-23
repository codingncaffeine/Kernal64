// 6522 VIA: two ports with their four control lines, two timers, the interrupt flags.
// The 1541 has two of them. Rules and sources: local-notes/hw/drive.md, V (the chip's
// data sheet).
//
// Per cycle of the drive's clock the board makes the CPU's register access (read/write)
// first, then calls clock() once. The shift register can be read and written but does
// not shift yet (the 1541 leaves it alone).
#pragma once

#include <cstdint>

namespace k64 {

class ViaHost {
public:
    // The levels on a port's pins, given what the chip drives: `out` the output register,
    // `ddr` the direction bits (1 = the chip drives the pin).
    virtual uint8_t viaPortA(int via, uint8_t out, uint8_t ddr) = 0;
    virtual uint8_t viaPortB(int via, uint8_t out, uint8_t ddr) = 0;
    // Something the chip drives has changed (a port, CA2 or CB2).
    virtual void viaOutputs(int via) = 0;
protected:
    ~ViaHost() = default;
};

class Via6522 {
public:
    Via6522(ViaHost& host, int id) : host_(host), id_(id) {}

    void reset();               // the RES pin: everything but the timers and the shift register
    uint8_t read(unsigned reg);
    uint8_t peek(unsigned reg) const;
    void write(unsigned reg, uint8_t v);
    void clock();               // one cycle

    bool irq() const { return (s_.ifr & s_.ier & 0x7F) != 0; }   // the IRQ output is low

    // The control lines as inputs. CA1 and CB1 always are; CA2 and CB2 when the
    // peripheral control register says so.
    void setCa1(bool level);
    void setCa2(bool level);
    void setCb1(bool level);
    void setCb2(bool level);
    // CA2 and CB2 as the chip drives them (high when they are inputs).
    bool ca2Out() const { return lineOut(s_.pcr >> 1, s_.ca2Low); }
    bool cb2Out() const { return lineOut(s_.pcr >> 5, s_.cb2Low); }
    // The ports as the chip drives them.
    uint8_t outA() const { return s_.ora; }
    uint8_t ddrA() const { return s_.ddra; }
    uint8_t outB() const;       // with timer 1's output on PB7 when that is switched on
    uint8_t ddrB() const { return s_.ddrb; }

#pragma pack(push, 1)
    struct State {
        uint8_t ora = 0, orb = 0, ddra = 0, ddrb = 0;
        uint8_t ira = 0, irb = 0;         // the ports as latched by CA1 / CB1
        uint16_t t1c = 0xFFFF, t1l = 0xFFFF, t2c = 0xFFFF;
        uint8_t t2ll = 0xFF;              // timer 2's low latch
        uint8_t sr = 0, acr = 0, pcr = 0, ifr = 0, ier = 0;
        uint8_t t1Loaded = 0, t2Loaded = 0;   // written in this cycle: counts from the next
        uint8_t t1Reload = 0;             // free-running: the latch goes in after the cycle at $FFFF
        uint8_t t1Armed = 0, t2Armed = 0; // the next time-out sets the flag
        uint8_t pb7 = 1;                  // timer 1's output
        uint8_t pb6 = 1;                  // PB6 as last seen (timer 2 counts its falling edges)
        uint8_t ca1 = 1, ca2 = 1, cb1 = 1, cb2 = 1;   // the lines' levels as inputs
        uint8_t ca2Low = 0, cb2Low = 0;   // handshake and pulse output: the line is low
        uint8_t ca2Pulse = 0, cb2Pulse = 0;   // pulse output: back high after this cycle
    };
#pragma pack(pop)
    const State& state() const { return s_; }
    void setState(const State& s) { s_ = s; }

private:
    static bool lineOut(unsigned mode, uint8_t low) {
        mode &= 7;
        if (mode < 4) return true;        // an input
        if (mode == 6) return false;      // held low
        if (mode == 7) return true;       // held high
        return !low;                      // handshake (4) or pulse (5)
    }
    void setIfr(uint8_t bits) { s_.ifr |= bits; }
    void portAccess(bool portA, bool write);

    ViaHost& host_;
    int id_;
    State s_;
};

} // namespace k64

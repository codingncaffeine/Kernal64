// MOS 6526 CIA: two timers with their cycle pipeline, interrupt control, time of day,
// serial data register and the two ports, as the original 6526 or as the later 8521 /
// 6526A, whose interrupt logic is a clock faster. Rules and sources: local-notes/hw/cia.md.
//
// Per system cycle the machine makes the CPU's register access (read/write) first, then
// calls clock() once.
#pragma once

#include <cstdint>

namespace k64 {

class CiaHost {
public:
    // Pin levels of a port, given what this CIA drives (1 = driven high or released).
    virtual uint8_t ciaPortA(int cia, uint8_t driven) = 0;
    virtual uint8_t ciaPortB(int cia, uint8_t driven) = 0;
protected:
    ~CiaHost() = default;
};

enum class CiaModel : uint8_t {
    Mos6526,   // the original chip
    Mos8521,   // the later chip (8521, 6526A)
};

class Cia6526 {
public:
    Cia6526(CiaHost& host, int id) : host_(host), id_(id) { reset(); }

    void setModel(CiaModel m) { new_ = m == CiaModel::Mos8521; }
    CiaModel model() const { return new_ ? CiaModel::Mos8521 : CiaModel::Mos6526; }

    void reset();
    uint8_t read(unsigned reg);
    uint8_t peek(unsigned reg) const;
    void write(unsigned reg, uint8_t v);
    void clock();               // one phi2 cycle
    void todTick();             // one cycle of the mains-frequency input on the TOD pin
    void setFlag(bool level);   // the FLAG input; a falling edge sets ICR bit 4

    bool irq() const { return s_.irqLow != 0; }   // interrupt output active (low)
    // The output went low in the last clock without the usual clock of delay to the CPU
    // (8521, flags of the serial port, the alarm and the FLAG pin; rule 20).
    bool irqEarly() const { return s_.irqEarly != 0; }
    uint8_t portAOut() const;   // what the port A pins are driven to (released = 1)
    uint8_t portBOut() const;

#pragma pack(push, 1)
    struct State {
        uint8_t pra = 0, prb = 0, ddra = 0, ddrb = 0;
        uint16_t counter[2] = {0xFFFF, 0xFFFF}, latch[2] = {0xFFFF, 0xFFFF};   // all ones after RES
        uint8_t cr[2] = {};
        uint8_t icr = 0, imr = 0, irqLow = 0;
        uint8_t irHold = 0;             // IR bit still visible after a read (clocks)
        uint8_t readAge = 2;            // clocks since the last ICR read (saturates at 3)
        uint8_t tbBug = 0;              // the next ICR read hides timer B's flag
        uint8_t ir = 0;                 // 8521: the IR bit (the 6526 derives it, see peek)
        uint8_t ack = 0;                // 8521: flags (and bit 7) an ICR read will clear a clock later
        uint8_t irqEarly = 0;           // see irqEarly()
        uint32_t delay = 0, feed = 0;   // the cycle pipeline, see cia6526.cpp
        uint8_t reloadHeld = 0;         // per timer: a load happened in the last clock
        uint8_t pbTimerOut = 0;         // PB6/PB7 as the timers drive them
        uint8_t pbToggle = 0;           // the toggle flip-flops (bits 6, 7)
        uint8_t tod[4] = {0, 0, 0, 0x01};   // 10ths, sec, min, hr (BCD, bit 7 = PM)
        uint8_t todLatch[4] = {};
        uint8_t alarm[4] = {};
        uint8_t todStopped = 1, todLatched = 0, todDivider = 0;
        uint8_t sdr = 0;
        uint32_t serial = 0;            // serial port pipeline, see cia6526.cpp
        uint16_t shifter = 0;
        uint8_t srBits = 0, sdrValid = 0, forceFinish = 0, cntOut = 1;
        uint8_t flag = 1;
    };
#pragma pack(pop)
    const State& state() const { return s_; }
    void setState(const State& s) { s_ = s; }

private:
    void underflow(int t);
    void serialClock();
    void serialDirection(uint8_t newCra);
    void setIcr(uint8_t bits);
    void setIcrLate(uint8_t bits);
    void todIncrement();
    void checkAlarm();

    CiaHost& host_;
    int id_;
    bool new_ = false;
    State s_;
};

} // namespace k64

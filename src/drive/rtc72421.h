// Epson RTC-72421 real-time clock: 16 four-bit registers, BCD time and date, control
// registers D, E and F. Rules from Epson's application manual (gitignore/refs/cmdhd/
// rtc7242x.pdf): register table p.9, bit functions p.10-15. Notes: local-notes/hw/cmdhd.md.
//
// The clock runs from the board's cycles (tick() once per cycle at cyclesPerSecond), so a
// save state and a replay keep the same time. The host's local time is only where it starts:
// setTime() at power on.
//
// Fixed-period output: the STD.P pin's level is kept (stdp()), its wiring is the board's.
#pragma once

#include <cstdint>

namespace k64 {

class Rtc72421 {
public:
    struct Time { int year, month, day, weekday, hour, minute, second; };   // year 0-99, hour 0-23

    explicit Rtc72421(uint32_t cyclesPerSecond) : cps_(cyclesPerSecond) {}

    void setTime(const Time& t);
    uint8_t read(unsigned reg) const;     // the low four bits are the chip's
    void write(unsigned reg, uint8_t v);
    void tick();                          // one board cycle
    bool stdp() const { return s_.stdpLow == 0; }   // STD.P: false = pulled low (open drain)

#pragma pack(push, 1)
    struct State {
        uint8_t r[13] = {};       // S1..W (BCD digits as the registers hold them)
        uint8_t cd = 0, ce = 0, cf = 0;
        uint8_t heldIncrement = 0;   // an increment HOLD put off (done when HOLD clears)
        uint8_t stdpLow = 0;
        uint32_t sub = 0;         // board cycles into the current second
        uint32_t periodSub = 0;   // board cycles into the current fixed period
    };
#pragma pack(pop)
    const State& state() const { return s_; }
    void setState(const State& s) { s_ = s; }

private:
    void incrementSecond();
    uint32_t periodCycles() const;
    bool twentyFour() const { return s_.cf & 0x04; }

    uint32_t cps_;
    State s_;
};

} // namespace k64

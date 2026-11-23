// The 1541's board: a 6502, 2K of RAM, the DOS ROM, a 6522 for the serial bus and one
// for the disk mechanism. Rules and sources: local-notes/hw/drive.md.
//
// So far the board alone: it boots its ROM and answers the serial bus's lines. The disk
// mechanism (rotation, the head, SYNC, byte ready) is not built yet: the second 6522
// sees no disk.
#pragma once

#include "cpu6502.h"
#include "roms.h"
#include "via6522.h"

#include <array>
#include <cstdint>

namespace k64 {

class C1541 final : private ViaHost {
public:
    C1541();

    // The DOS ROM's two halves; the drive does not run without both.
    void setRoms(const RomSet& roms);
    bool hasRoms() const { return romsOk_; }
    // The two jumpers that set the device number: 8 to 11.
    void setDeviceNumber(int number) { jumpers_ = static_cast<uint8_t>((number - 8) & 3); }

    void reset();       // power on
    void tick();        // one cycle of the drive's 1 MHz clock

    // The serial bus at the drive's connector. In: whether anything else on the bus
    // pulls a line low. Out: whether the drive pulls it.
    void setBus(bool atnLow, bool clkLow, bool dataLow);
    bool pullsClk() const;
    bool pullsData() const;   // its DATA output, or the answer its hardware gives to ATN

    bool led() const;         // the activity light
    bool motor() const;       // the spindle motor

    // For tests and tools.
    const CpuRegs& cpuRegs() const { return cpu_.regs(); }
    bool cpuAtInstructionStart() const { return cpu_.atInstructionStart(); }
    uint64_t cycle() const { return cycle_; }
    const uint8_t* ram() const { return ram_.data(); }
    const Via6522& via1() const { return via1_; }
    const Via6522& via2() const { return via2_; }

private:
    struct Bus {
        C1541* d;
        uint8_t read(uint16_t a) { return d->read(a); }
        void write(uint16_t a, uint8_t v) { d->write(a, v); }
    };
    uint8_t read(uint16_t a);
    void write(uint16_t a, uint8_t v);
    uint8_t viaPortA(int via, uint8_t out, uint8_t ddr) override;
    uint8_t viaPortB(int via, uint8_t out, uint8_t ddr) override;
    void viaOutputs(int) override {}

    std::array<uint8_t, 2048> ram_{};
    std::array<uint8_t, 16384> rom_{};
    bool romsOk_ = false;
    uint8_t jumpers_ = 0;
    bool atnLow_ = false, clkLow_ = false, dataLow_ = false;   // pulled by others
    uint64_t cycle_ = 0;
    Bus bus_{this};
    Cpu6502<Bus> cpu_{bus_};
    Via6522 via1_{*this, 0};   // the serial bus
    Via6522 via2_{*this, 1};   // the disk mechanism
};

} // namespace k64

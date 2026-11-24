// The CMD HD's board: a 65C02 at 2 MHz, 64K of RAM (16K of it banked), the 16K boot ROM,
// a 6522 for the serial bus, a 6522 for SCSI, an 8255 for memory control, the front panel
// and SCSI control lines, an RTC-72421, and the latch that drives the panel lights and the
// RAM write protection. The disk is a SCSI target (ScsiDisk) on a host file (.dhd).
// Rules and sources: local-notes/hw/cmdhd.md (no schematic: the boot ROM's own code first).
#pragma once

#include "cpu65c02.h"
#include "ppi8255.h"
#include "rtc72421.h"
#include "scsidisk.h"
#include "via6522.h"

#include <array>
#include <vector>
#include <cstdint>

namespace k64 {

class CmdHd final : private ViaHost {
public:
    static constexpr uint32_t kClockHz = 2'000'000;

    CmdHd();

    // The boot ROM: 16K, or a 32K chip image holding it twice.
    bool setRom(const uint8_t* data, size_t size);
    bool hasRom() const { return romOk_; }
    void attachDisk(BlockFile* disk) { scsi_.attach(disk); }
    void setTime(const Rtc72421::Time& t) { rtc_.setTime(t); }

    void reset();   // power on (RAM cleared: the signature at $90F0 is gone, so a cold start)
    void tick();    // one cycle of the 2 MHz clock

    // Front panel. Buttons are held while true; RESET is a press (the board restarts, RAM kept).
    void setButtons(bool swap8, bool swap9, bool writeProtect) { swap8_ = swap8; swap9_ = swap9; wp_ = writeProtect; }
    void pressReset();
    uint8_t lights() const { return latch_; }   // the latch U20 (active-low lamps, hw/cmdhd.md)

    // The serial bus, as on the 1541 (C1541::setBus).
    void setBus(bool atnLow, bool clkLow, bool dataLow);
    bool pullsClk() const;
    bool pullsData() const;

    // For tests and tools.
    const CpuRegs& cpuRegs() const { return cpu_.regs(); }
    bool cpuAtInstructionStart() const { return cpu_.atInstructionStart(); }
    uint64_t cycle() const { return cycle_; }
    uint8_t peek(uint16_t a) const { return ram_[a]; }
    ScsiDisk& scsi() { return scsi_; }
    // Debugging: addresses whose execution is printed to stderr (K64_HD_WATCH=hex,hex,...),
    // at most `watchLeft` times in all.
    std::vector<uint16_t> watch;
    int watchLeft = 200;

private:
    struct Bus {
        CmdHd* d;
        uint8_t read(uint16_t a) { return d->read(a); }
        void write(uint16_t a, uint8_t v) { d->write(a, v); }
    };
    uint8_t read(uint16_t a);
    void write(uint16_t a, uint8_t v);
    uint8_t viaPortA(int via, uint8_t out, uint8_t ddr) override;
    uint8_t viaPortB(int via, uint8_t out, uint8_t ddr) override;
    void viaOutputs(int via) override;
    Ppi8255::Pins ppiPins() const;
    void scsiLines();
    uint8_t portC() const { return ppi_.pinsC(); }

    std::array<uint8_t, 65536> ram_{};
    std::array<uint8_t, 16384> rom_{};
    bool romOk_ = false;
    uint8_t latch_ = 0xFF;
    uint8_t busLast_ = 0;
    bool swap8_ = false, swap9_ = false, wp_ = false;
    bool atnLow_ = false, clkLow_ = false, dataLow_ = false;
    bool ackLow_ = false;   // CA2 of the SCSI 6522 as last seen
    uint64_t cycle_ = 0;
    Bus bus_{this};
    Cpu65C02<Bus> cpu_{bus_, C02Variant::Rockwell};
    Via6522 via1_{*this, 0};   // U10: the serial bus
    Via6522 via2_{*this, 1};   // U9: SCSI
    Ppi8255 ppi_;              // U11
    Rtc72421 rtc_{kClockHz};
    ScsiDisk scsi_{0};
};

} // namespace k64

// The 1541's board: a 6502, 2K of RAM, the DOS ROM, a 6522 for the serial bus and one
// for the disk mechanism. Rules and sources: local-notes/hw/drive.md.
//
// The disk mechanism: the spindle, the head on its stepper, and the read circuit (zone
// clock, bit-cell counter, shift register, SYNC and BYTE READY), hw/drive.md R, and the
// write path, W.
#pragma once

#include "cpu6502.h"
#include "roms.h"
#include "via6522.h"
#include "disk.h"

#include <memory>

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

    // The disk in the drive (none: empty). Shared with whoever made it, who can save what the
    // drive wrote (GcrDisk::changed).
    void insertDisk(std::shared_ptr<GcrDisk> disk) { disk_ = std::move(disk); }
    void ejectDisk() { disk_.reset(); }
    bool hasDisk() const { return disk_ != nullptr; }
    int halfTrack() const { return m_.halfTrack; }

    // Save states: everything the board holds, constant size. The ROMs and the jumpers are
    // settings, not state.
    static size_t stateSize();
    void saveState(uint8_t* out) const;
    void loadState(const uint8_t* in);

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
    void viaOutputs(int via) override;
    void mechanism();         // one drive cycle of the spindle and the read circuit
    void writeBit(bool one, uint8_t zone);
    bool readMode() const { return via2_.cb2Out(); }
    bool soe() const { return via2_.ca2Out(); }

#pragma pack(push, 1)
    struct Mech {
        uint8_t halfTrack = 36;     // 2 = track 1 (the head starts over the directory track)
        uint8_t phase = 0;          // the stepper's phase as last driven (PB0-PB1)
        uint32_t angle = 0;         // the disk's position: 16 MHz ticks into the revolution
        uint8_t ue7 = 0, uf4 = 0;   // zone clock divider and bit-cell counter
        uint16_t shift = 0;         // the last ten bits read
        uint8_t bits = 0;           // bits since the last byte or SYNC
        uint8_t latch = 0;          // the byte port A reads
        uint8_t flux = 0;           // a flux change reaches the counters in the next tick
        uint8_t byteDone = 0;       // the bit counter stands at 7: a byte is complete (R4)
        uint8_t readyLow = 0;       // BYTE READY was low in this drive cycle
        uint8_t wbuf = 0;           // the write shift register (W1)
        uint8_t writing = 0;        // the last bit cell was written (W2)
        uint32_t wpos = 0;          // the bit the next written cell goes to (W2)
    };
#pragma pack(pop)

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
    Mech m_;
    uint8_t bus_last_ = 0;     // the data bus's last value (B1)
    std::shared_ptr<GcrDisk> disk_;
};

} // namespace k64

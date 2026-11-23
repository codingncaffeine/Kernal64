// The whole machine, advanced one system cycle at a time.
//
// Per cycle (VIC-II article §2.4.3): in the first clock phase the VIC-II makes its
// access; in the second, the 6510 makes its access (when the VIC-II has not taken
// the bus). Every access goes through the PLA (pla.h).
#pragma once

#include "cpu6502.h"
#include "roms.h"
#include "timing.h"
#include "vic2.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace k64 {

class C64 final : private VicHost {
public:
    explicit C64(VicModel model = VicModel::Mos6569);

    // The system ROMs; the machine does not run without BASIC, KERNAL and characters.
    void setRoms(const RomSet& roms);
    bool hasSystemRoms() const { return romsOk_; }

    void reset();       // power on
    void setModel(VicModel model);
    const Timing& timing() const { return *timing_; }

    void runFrame();    // one video frame of cycles, ending where vertical blanking starts
    void tick();        // one system cycle

    // The raster as emitted: one palette index per pixel, kMaxRasterWidth wide, row = raster
    // line, column = pixel counted from the start of cycle 1 (Vic2::pixels).
    const uint8_t* raster() const { return vic_.pixels(); }
    const Vic2& vic() const { return vic_; }
    // The visible picture as XRGB8888, rows from the first visible line (in raster order
    // across the frame boundary), timing().visibleWidth x timing().visibleLines.
    void visibleFrame(uint32_t* out) const;

    uint64_t cycle() const { return s_.cycle; }
    uint64_t frame() const { return s_.frame; }

    // Read-only views for tests and tools.
    const uint8_t* ram() const { return ram_.data(); }
    const CpuRegs& cpuRegs() const { return cpu_.regs(); }
    uint8_t peekCpu(uint16_t addr) const;  // what the CPU would read, without side effects

    // Machine state as a flat byte image of constant size for the model.
    size_t stateSize() const;
    void saveState(uint8_t* out) const;
    bool loadState(const uint8_t* in, size_t size);

private:
    struct CpuBus {
        C64* m;
        uint8_t read(uint16_t a) { return m->cpuRead(a); }
        void write(uint16_t a, uint8_t v) { m->cpuWrite(a, v); }
    };

    uint8_t cpuRead(uint16_t a);
    uint8_t cpuReadBus(uint16_t a);
    void cpuWrite(uint16_t a, uint8_t v);
    uint8_t ioRead(uint16_t a);
    void ioWrite(uint16_t a, uint8_t v);
    uint8_t vicFetch(uint16_t vaddr14) const;  // a VIC-II access in its 16 KB view
    uint8_t vicRead(uint16_t a) override { return vicFetch(a); }
    uint8_t colorRead(uint16_t a) override { return colorRam_[a & 0x3FF]; }

    uint16_t plaInputsCpu(uint16_t a, bool read) const;
    uint8_t portPins() const;    // the 6510 port lines as the PLA sees them
    uint8_t portRead() const;    // reading $01
    unsigned vicBank() const;    // 0..3, from CIA 2 port A (inverted)

    // ---- state (everything here is saved; packed so it holds no padding) -------
#pragma pack(push, 1)
    struct State {
        uint64_t cycle = 0, frame = 0;
        uint8_t portDdr = 0, portData = 0;
        uint8_t portFloat = 0;           // bits 6-7 as last driven (they float when inputs)
        uint8_t vicLastByte = 0;         // what the VIC-II read in the last first phase (open bus)
        uint8_t cpuData = 0;             // the CPU's data bus in the last second phase
        uint8_t aecLow = 0;              // the VIC-II holds the second phase of this cycle
        uint8_t cia1[16] = {}, cia2[16] = {};
        uint8_t sid[32] = {};
    };
#pragma pack(pop)

    const Timing* timing_;
    State s_;
    std::array<uint8_t, 65536> ram_{};
    std::array<uint8_t, 1024> colorRam_{};
    std::array<uint8_t, 8192> basic_{}, kernal_{};
    std::array<uint8_t, 4096> chargen_{};
    bool romsOk_ = false;
    CpuBus bus_{this};
    Cpu6502<CpuBus> cpu_{bus_};
    Vic2 vic_{*this, kTimingPal};
};

} // namespace k64

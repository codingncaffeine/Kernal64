// The whole machine, advanced one system cycle at a time.
//
// Per cycle (VIC-II article §2.4.3): in the first clock phase the VIC-II makes its
// access; in the second, the 6510 makes its access (when the VIC-II has not taken
// the bus). Every access goes through the PLA (pla.h).
#pragma once

#include "cpu6502.h"
#include "roms.h"
#include "timing.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace k64 {

class C64 {
public:
    explicit C64(VicModel model = VicModel::Mos6569);

    // The system ROMs; the machine does not run without BASIC, KERNAL and characters.
    void setRoms(const RomSet& roms);
    bool hasSystemRoms() const { return romsOk_; }

    void reset();       // power on
    void setModel(VicModel model);
    const Timing& timing() const { return *timing_; }

    void runFrame();    // exactly one video frame of cycles
    void tick();        // one system cycle

    // The raster as emitted, one 32-bit XRGB pixel per dot, kMaxRasterWidth wide.
    const uint32_t* raster() const { return raster_.data(); }
    int rasterWidth() const { return timing_->cyclesPerLine * 8; }
    int rasterHeight() const { return timing_->linesPerFrame; }

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
    void cpuWrite(uint16_t a, uint8_t v);
    uint8_t ioRead(uint16_t a);
    void ioWrite(uint16_t a, uint8_t v);
    uint8_t vicFetch(uint16_t vaddr14) const;  // a VIC-II access in its 16 KB view

    uint16_t plaInputsCpu(uint16_t a, bool read) const;
    uint8_t portPins() const;    // the 6510 port lines as the PLA sees them
    uint8_t portRead() const;    // reading $01
    unsigned vicBank() const;    // 0..3, from CIA 2 port A (inverted)

    void vicPhase1();            // stub: raster counter, raster interrupt, idle access

    // ---- state (everything here is saved; packed so it holds no padding) -------
#pragma pack(push, 1)
    struct State {
        uint64_t cycle = 0, frame = 0;
        uint8_t portDdr = 0, portData = 0;
        uint8_t portFloat = 0;           // bits 6-7 as last driven (they float when inputs)
        uint8_t vicLastByte = 0;         // what the VIC-II read in the last first phase (open bus)
        int32_t rasterCycle = 0;         // 0-based cycle within the line
        int32_t rasterLine = 0;
        uint8_t vicReg[64] = {};
        uint16_t rasterCompare = 0;
        uint8_t vicIrqLatch = 0, vicIrqEnable = 0;
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
    std::vector<uint32_t> raster_;
};

} // namespace k64

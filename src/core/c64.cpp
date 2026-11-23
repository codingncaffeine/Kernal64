#include "c64.h"

#include "pla.h"

#include <algorithm>
#include <cstring>

namespace k64 {

namespace {
constexpr uint32_t kStateMagic = 0x3436'4B32; // "K64" + format generation
constexpr uint32_t kStateVersion = 2;
}

C64::C64(VicModel model) : timing_(&timingFor(model)), raster_(kMaxRasterWidth * kMaxRasterHeight, 0) {
    reset();
}

void C64::setRoms(const RomSet& roms) {
    romsOk_ = roms.has(RomRole::Basic) && roms.has(RomRole::Kernal) && roms.has(RomRole::Chargen);
    if (roms.has(RomRole::Basic)) std::copy_n(roms.get(RomRole::Basic).begin(), basic_.size(), basic_.begin());
    if (roms.has(RomRole::Kernal)) std::copy_n(roms.get(RomRole::Kernal).begin(), kernal_.size(), kernal_.begin());
    if (roms.has(RomRole::Chargen)) std::copy_n(roms.get(RomRole::Chargen).begin(), chargen_.size(), chargen_.begin());
}

void C64::setModel(VicModel model) {
    timing_ = &timingFor(model);
    reset();
}

void C64::reset() {
    s_ = State{};
    // DRAM at power on: the common pattern of the C64's RAM chips, $00 and $FF alternating
    // in blocks of 64 bytes. (Software must not depend on it; a fixed pattern keeps runs
    // reproducible.)
    for (size_t i = 0; i < ram_.size(); ++i) ram_[i] = (i & 0x40) ? 0xFF : 0x00;
    colorRam_.fill(0);
    std::fill(raster_.begin(), raster_.end(), 0u);
    cpu_.reset();
}

// ---- the 6510 port -----------------------------------------------------------
//
// $00 is the data direction register, $01 the data register (64doc, "Memory
// Management"). Lines set as inputs read their pin: LORAM/HIRAM/CHAREN (bits 0-2)
// have pull-ups, so the ROMs are mapped in after reset; bit 4 is the cassette sense
// (pulled up: no button pressed). Bits 3 and 5 read 0 as inputs, and bits 6-7, which
// have no pins, keep the last value driven. (Provisional for bits 3, 5, 6, 7: checked
// in Phase 4 against the CPU-port test program.)

uint8_t C64::portPins() const {
    const uint8_t driven = s_.portData & s_.portDdr;
    const uint8_t inputs = static_cast<uint8_t>(~s_.portDdr);
    return static_cast<uint8_t>(driven | (inputs & 0x17));
}

uint8_t C64::portRead() const {
    const uint8_t pins = portPins();
    const uint8_t floating = static_cast<uint8_t>(~s_.portDdr & 0xC0 & s_.portFloat);
    return static_cast<uint8_t>((pins & 0x3F) | (s_.portDdr & s_.portData & 0xC0) | floating);
}

// ---- the PLA's view ------------------------------------------------------------

uint16_t C64::plaInputsCpu(uint16_t a, bool read) const {
    const uint8_t pins = portPins();
    uint16_t in = 0;
    if (pins & 0x01) in |= pla::nLORAM;
    if (pins & 0x02) in |= pla::nHIRAM;
    if (pins & 0x04) in |= pla::nCHAREN;
    if (a & 0x8000) in |= pla::A15;
    if (a & 0x4000) in |= pla::A14;
    if (a & 0x2000) in |= pla::A13;
    if (a & 0x1000) in |= pla::A12;
    if (read) in |= pla::RnW;
    in |= pla::BA;                     // VIC-II stub never takes the bus yet
    in |= pla::nEXROM | pla::nGAME;    // no cartridge
    // #AEC low (CPU cycle), #CAS low (access in progress), VA lines irrelevant here.
    return in;
}

unsigned C64::vicBank() const {
    // CIA 2 port A bits 0-1, inverted; inputs are pulled up, which selects bank 0.
    const uint8_t pra = s_.cia2[0], ddra = s_.cia2[2];
    const uint8_t lines = static_cast<uint8_t>((pra & ddra) | ~ddra);
    return 3u - (lines & 3u);
}

uint8_t C64::vicFetch(uint16_t vaddr) const {
    const unsigned bank = vicBank();
    uint16_t in = pla::nAEC | pla::RnW | pla::nLORAM | pla::nHIRAM | pla::nCHAREN | pla::nEXROM | pla::nGAME |
                  pla::BA | pla::A15 | pla::A14 | pla::A13 | pla::A12; // address bus pulled up in VIC cycles
    if (!(bank & 1)) in |= pla::nVA14;
    if (vaddr & 0x2000) in |= pla::VA13;
    if (vaddr & 0x1000) in |= pla::VA12;
    const uint8_t out = pla::table()[in];
    if (!(out & pla::nCHARROM)) return chargen_[vaddr & 0x0FFF];
    return ram_[(bank << 14) | (vaddr & 0x3FFF)];
}

// ---- CPU accesses ---------------------------------------------------------------

uint8_t C64::peekCpu(uint16_t a) const {
    if (a == 0) return s_.portDdr;
    if (a == 1) return portRead();
    const uint8_t out = pla::table()[plaInputsCpu(a, true)];
    if (!(out & pla::nBASIC)) return basic_[a & 0x1FFF];
    if (!(out & pla::nKERNAL)) return kernal_[a & 0x1FFF];
    if (!(out & pla::nCHARROM)) return chargen_[a & 0x0FFF];
    if (!(out & pla::nCASRAM)) return ram_[a];
    return s_.vicLastByte;
}

uint8_t C64::cpuRead(uint16_t a) {
    if (a == 0) return s_.portDdr;
    if (a == 1) return portRead();
    const uint8_t out = pla::table()[plaInputsCpu(a, true)];
    if (!(out & pla::nBASIC)) return basic_[a & 0x1FFF];
    if (!(out & pla::nKERNAL)) return kernal_[a & 0x1FFF];
    if (!(out & pla::nCHARROM)) return chargen_[a & 0x0FFF];
    if (!(out & pla::nIO)) return ioRead(a);
    if (!(out & pla::nCASRAM)) return ram_[a];
    return s_.vicLastByte; // nothing selected: the data bus still holds the VIC-II's byte
}

void C64::cpuWrite(uint16_t a, uint8_t v) {
    if (a <= 1) {
        if (a == 0) s_.portDdr = v;
        else s_.portData = v;
        s_.portFloat = static_cast<uint8_t>((s_.portFloat & ~(s_.portDdr & 0xC0)) | (s_.portData & s_.portDdr & 0xC0));
        // The CPU drives no data onto the bus for its own port, but R/W is low: the RAM
        // underneath stores what the VIC-II read in the first phase (article §4).
        ram_[a] = s_.vicLastByte;
        return;
    }
    const uint16_t in = plaInputsCpu(a, false);
    const uint8_t out = pla::table()[in];
    if (!(out & pla::nIO)) {
        if ((a & 0x0C00) == 0x0800) {
            if (!(out & pla::nGRW)) colorRam_[a & 0x03FF] = v & 0x0F;
        } else {
            ioWrite(a, v);
        }
        return;
    }
    if (!(out & pla::nCASRAM)) ram_[a] = v;
}

// $D000-$DFFF with I/O selected: VIC-II, SID, color RAM, CIA 1, CIA 2, I/O 1, I/O 2.
uint8_t C64::ioRead(uint16_t a) {
    switch ((a >> 8) & 0x0F) {
    case 0x0: case 0x1: case 0x2: case 0x3: {
        const unsigned r = a & 0x3F;
        // Stub VIC-II: the raster registers and the interrupt registers work; unused bits read 1.
        switch (r) {
        case 0x11: return static_cast<uint8_t>((s_.vicReg[0x11] & 0x7F) | ((s_.rasterLine & 0x100) ? 0x80 : 0));
        case 0x12: return static_cast<uint8_t>(s_.rasterLine & 0xFF);
        case 0x16: return static_cast<uint8_t>(s_.vicReg[0x16] | 0xC0);
        case 0x18: return static_cast<uint8_t>(s_.vicReg[0x18] | 0x01);
        case 0x19: {
            const bool irq = (s_.vicIrqLatch & s_.vicIrqEnable & 0x0F) != 0;
            return static_cast<uint8_t>(0x70 | (s_.vicIrqLatch & 0x0F) | (irq ? 0x80 : 0));
        }
        case 0x1A: return static_cast<uint8_t>(0xF0 | s_.vicIrqEnable);
        case 0x1E: case 0x1F: { const uint8_t v = s_.vicReg[r]; s_.vicReg[r] = 0; return v; }
        default:
            if (r >= 0x2F) return 0xFF;
            if (r >= 0x20) return static_cast<uint8_t>(0xF0 | s_.vicReg[r]);
            return s_.vicReg[r];
        }
    }
    case 0x4: case 0x5: case 0x6: case 0x7:
        return 0; // SID stub
    case 0x8: case 0x9: case 0xA: case 0xB:
        // Color RAM is 4 bits wide; the upper bits are whatever is left on the bus.
        return static_cast<uint8_t>((s_.vicLastByte & 0xF0) | colorRam_[a & 0x03FF]);
    case 0xC: {
        const unsigned r = a & 0x0F;
        if (r == 0x00 || r == 0x01) return 0xFF; // stub CIA 1: no keys, no joystick
        return s_.cia1[r];
    }
    case 0xD:
        return s_.cia2[a & 0x0F];
    default:
        return s_.vicLastByte; // I/O 1 and I/O 2: nothing attached
    }
}

void C64::ioWrite(uint16_t a, uint8_t v) {
    switch ((a >> 8) & 0x0F) {
    case 0x0: case 0x1: case 0x2: case 0x3: {
        const unsigned r = a & 0x3F;
        if (r == 0x11) s_.rasterCompare = static_cast<uint16_t>((s_.rasterCompare & 0xFF) | ((v & 0x80) << 1));
        if (r == 0x12) s_.rasterCompare = static_cast<uint16_t>((s_.rasterCompare & 0x100) | v);
        if (r == 0x19) { s_.vicIrqLatch = static_cast<uint8_t>(s_.vicIrqLatch & ~v & 0x0F); return; }
        if (r == 0x1A) { s_.vicIrqEnable = v & 0x0F; return; }
        if (r == 0x1E || r == 0x1F) return; // collision registers cannot be written
        if (r < 0x2F) s_.vicReg[r] = v;
        return;
    }
    case 0x4: case 0x5: case 0x6: case 0x7:
        s_.sid[a & 0x1F] = v;
        return;
    case 0xC: s_.cia1[a & 0x0F] = v; return;
    case 0xD: s_.cia2[a & 0x0F] = v; return;
    default: return;
    }
}

// ---- the cycle ------------------------------------------------------------------

void C64::vicPhase1() {
    // Stub VIC-II until Phase 3: the raster position, the raster interrupt (compared at
    // the start of each line; line 0 one cycle later, article §3.12) and the idle access
    // ($3FFF) that keeps the open-bus value right.
    s_.vicLastByte = vicFetch(0x3FFF);
    const bool compareNow = s_.rasterLine == 0 ? s_.rasterCycle == 1 : s_.rasterCycle == 0;
    if (compareNow && s_.rasterLine == s_.rasterCompare) s_.vicIrqLatch |= 0x01;
}

void C64::tick() {
    vicPhase1();
    const bool vicIrq = (s_.vicIrqLatch & s_.vicIrqEnable & 0x0F) != 0;
    cpu_.setIrq(vicIrq);
    cpu_.setNmi(false);
    cpu_.setRdy(true);
    cpu_.tick();

    ++s_.cycle;
    if (++s_.rasterCycle == timing_->cyclesPerLine) {
        s_.rasterCycle = 0;
        if (++s_.rasterLine == timing_->linesPerFrame) {
            s_.rasterLine = 0;
            ++s_.frame;
        }
    }
}

void C64::runFrame() {
    const uint64_t start = s_.frame;
    while (s_.frame == start) tick();
}

// ---- save states ----------------------------------------------------------------

size_t C64::stateSize() const {
    return 4 + 4 + 1 + sizeof(State) + ram_.size() + colorRam_.size() + sizeof(Cpu6502<CpuBus>::Snapshot);
}

void C64::saveState(uint8_t* out) const {
    auto put = [&out](const void* p, size_t n) { std::memcpy(out, p, n); out += n; };
    put(&kStateMagic, 4);
    put(&kStateVersion, 4);
    const uint8_t model = static_cast<uint8_t>(timing_->model);
    put(&model, 1);
    put(&s_, sizeof s_);
    put(ram_.data(), ram_.size());
    put(colorRam_.data(), colorRam_.size());
    const auto snap = cpu_.snapshot();
    put(&snap, sizeof snap);
}

bool C64::loadState(const uint8_t* in, size_t size) {
    if (size < stateSize()) return false;
    auto get = [&in](void* p, size_t n) { std::memcpy(p, in, n); in += n; };
    uint32_t magic = 0, version = 0;
    uint8_t model = 0;
    get(&magic, 4);
    get(&version, 4);
    get(&model, 1);
    if (magic != kStateMagic || version != kStateVersion || model != static_cast<uint8_t>(timing_->model)) return false;
    State s;
    get(&s, sizeof s);
    if (s.rasterCycle < 0 || s.rasterCycle >= timing_->cyclesPerLine || s.rasterLine < 0 ||
        s.rasterLine >= timing_->linesPerFrame)
        return false;
    s_ = s;
    get(ram_.data(), ram_.size());
    get(colorRam_.data(), colorRam_.size());
    Cpu6502<CpuBus>::Snapshot snap;
    get(&snap, sizeof snap);
    cpu_.restore(snap);
    return true;
}

} // namespace k64

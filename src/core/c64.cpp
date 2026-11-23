#include "c64.h"

#include "pla.h"

#include <algorithm>
#include <cstring>

namespace k64 {

namespace {
constexpr uint32_t kStateMagic = 0x3436'4B32; // "K64" + format generation
constexpr uint32_t kStateVersion = 3;
}

C64::C64(VicModel model) : timing_(&timingFor(model)) {
    vic_.setTiming(*timing_);
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
    vic_.setTiming(*timing_);
    reset();
}

void C64::reset() {
    s_ = State{};
    // DRAM at power on: the common pattern of the C64's RAM chips, $00 and $FF alternating
    // in blocks of 64 bytes. (Software must not depend on it; a fixed pattern keeps runs
    // reproducible.)
    for (size_t i = 0; i < ram_.size(); ++i) ram_[i] = (i & 0x40) ? 0xFF : 0x00;
    colorRam_.fill(0);
    vic_.reset();
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
    if (vic_.ba()) in |= pla::BA;      // with BA low, I/O is not selected (memory.md rule 4)
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
    const uint8_t v = cpuReadBus(a);
    s_.cpuData = v;
    return v;
}

uint8_t C64::cpuReadBus(uint16_t a) {
    // A halted read while the VIC-II holds the second phase never reaches the bus.
    if (s_.aecLow) return s_.vicLastByte;
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
    s_.cpuData = v;
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
    case 0x0: case 0x1: case 0x2: case 0x3:
        return vic_.read(a);
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
    case 0x0: case 0x1: case 0x2: case 0x3:
        vic_.write(a, v);
        return;
    case 0x4: case 0x5: case 0x6: case 0x7:
        s_.sid[a & 0x1F] = v;
        return;
    case 0xC: s_.cia1[a & 0x0F] = v; return;
    case 0xD: s_.cia2[a & 0x0F] = v; return;
    default: return;
    }
}

// ---- the cycle ------------------------------------------------------------------

void C64::tick() {
    vic_.phase1();
    s_.vicLastByte = vic_.lastByte();
    s_.aecLow = vic_.aecLow();
    cpu_.setIrq(vic_.irq());
    cpu_.setNmi(false);
    cpu_.setRdy(vic_.ba());
    cpu_.tick();
    vic_.phase2(s_.cpuData);
    vic_.render();
    vic_.endCycle();

    ++s_.cycle;
    if (vic_.line() == 0 && vic_.cycleInLine() == 1) ++s_.frame;
}

void C64::runFrame() {
    // Ends where vertical blanking starts, so the visible picture of one call is one frame.
    const int stop = timing_->firstVblankLine;
    do tick();
    while (!(vic_.line() == stop && vic_.cycleInLine() == 1));
}

void C64::visibleFrame(uint32_t* out) const {
    const auto& pal = vicPalette();
    const Timing& t = *timing_;
    const uint8_t* px = vic_.pixels();
    for (int r = 0; r < t.visibleLines; ++r) {
        const int line = (t.firstVisibleLine + r) % t.linesPerFrame;
        const uint8_t* src = px + static_cast<size_t>(line) * kMaxRasterWidth + t.firstVisiblePixel;
        for (int x = 0; x < t.visibleWidth; ++x) *out++ = pal[src[x] & 0x0F];
    }
}

// ---- save states ----------------------------------------------------------------

size_t C64::stateSize() const {
    return 4 + 4 + 1 + sizeof(State) + sizeof(Vic2::State) + ram_.size() + colorRam_.size() + sizeof(Cpu6502<CpuBus>::Snapshot);
}

void C64::saveState(uint8_t* out) const {
    auto put = [&out](const void* p, size_t n) { std::memcpy(out, p, n); out += n; };
    put(&kStateMagic, 4);
    put(&kStateVersion, 4);
    const uint8_t model = static_cast<uint8_t>(timing_->model);
    put(&model, 1);
    put(&s_, sizeof s_);
    const Vic2::State vs = vic_.state();
    put(&vs, sizeof vs);
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
    Vic2::State vs;
    get(&vs, sizeof vs);
    if (vs.cycle < 1 || vs.cycle > timing_->cyclesPerLine || vs.line < 0 || vs.line >= timing_->linesPerFrame ||
        vs.vmli >= 40 || vs.shPixel < 0 || vs.shPixel > 8)
        return false;
    s_ = s;
    vic_.setState(vs);
    get(ram_.data(), ram_.size());
    get(colorRam_.data(), colorRam_.size());
    Cpu6502<CpuBus>::Snapshot snap;
    get(&snap, sizeof snap);
    cpu_.restore(snap);
    return true;
}

} // namespace k64

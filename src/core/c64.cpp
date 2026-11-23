#include "c64.h"

#include "pla.h"

#include <algorithm>
#include <cstring>

namespace k64 {

namespace {
constexpr uint32_t kStateMagic = 0x3436'4B32; // "K64" + format generation
constexpr uint32_t kStateVersion = 7;
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
    cia1_.reset();
    cia2_.reset();
    cpu_.reset();
}

// ---- the 6510 port -----------------------------------------------------------
//
// $00 is the data direction register, $01 the data register (64doc, "Memory
// Management"). Lines set as inputs read their pin: LORAM/HIRAM/CHAREN (bits 0-2)
// have pull-ups, so the ROMs are mapped in after reset; bit 4 is the cassette sense
// (pulled up: no button pressed). Bit 5 (cassette motor) reads 0 as an input. Bits 3
// (cassette write) and 6-7 (no pins) keep the last value driven while they are inputs
// (Lorenz's CPUPORT program, measured on a C64; real chips let that charge fade after a
// while, not modeled yet).

uint8_t C64::portPins() const {
    const uint8_t driven = s_.portData & s_.portDdr;
    const uint8_t inputs = static_cast<uint8_t>(~s_.portDdr);
    return static_cast<uint8_t>(driven | (inputs & 0x17));
}

uint8_t C64::portRead() const {
    const uint8_t pins = portPins();
    const uint8_t floating = static_cast<uint8_t>(~s_.portDdr & 0xC8 & s_.portFloat);
    return static_cast<uint8_t>((pins & 0x37) | (s_.portDdr & s_.portData & 0xC8) | floating);
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
    return 3u - (cia2_.portAOut() & 3u);
}

bool C64::vicIsCharRom(uint16_t vaddr) const {
    const unsigned bank = vicBank();
    uint16_t in = pla::nAEC | pla::RnW | pla::nLORAM | pla::nHIRAM | pla::nCHAREN | pla::nEXROM | pla::nGAME |
                  pla::BA | pla::A15 | pla::A14 | pla::A13 | pla::A12; // address bus pulled up in VIC cycles
    if (!(bank & 1)) in |= pla::nVA14;
    if (vaddr & 0x2000) in |= pla::VA13;
    if (vaddr & 0x1000) in |= pla::VA12;
    return !(pla::table()[in] & pla::nCHARROM);
}

uint8_t C64::vicFetch(uint16_t vaddr) const {
    if (vicIsCharRom(vaddr)) return chargen_[vaddr & 0x0FFF];
    return ram_[(vicBank() << 14) | (vaddr & 0x3FFF)];
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
        s_.portFloat = static_cast<uint8_t>((s_.portFloat & ~(s_.portDdr & 0xC8)) | (s_.portData & s_.portDdr & 0xC8));
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
    case 0xC: return cia1_.read(a);
    case 0xD: return cia2_.read(a);
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
        if (debugCart_ && a == 0xD7FF) debugExit_ = v;
        s_.sid[a & 0x1F] = v;
        return;
    case 0xC:
        cia1_.write(a, v);
        if ((a & 0x0F) < 4) updateLightPen();   // ports and directions
        return;
    case 0xD: cia2_.write(a, v); return;
    default: return;
    }
}

// The light pen line is CIA 1's port B bit 4 (also the fire button of control port 1 and
// a keyboard row), so software can pull it low through the port (hw/cia.md rule 16).
void C64::updateLightPen() {
    vic_.setLightPen(!(ciaPortB(0, cia1_.portBOut()) & 0x10));
}

// ---- input and the CIA ports (hw/cia.md rules 16-17) -----------------------------

void C64::setKey(int column, int row, bool down) {
    if (column < 0 || column > 7 || row < 0 || row > 7) return;
    const uint8_t bit = static_cast<uint8_t>(1 << row);
    s_.keys[column] = static_cast<uint8_t>(down ? (s_.keys[column] | bit) : (s_.keys[column] & ~bit));
    updateLightPen();
}

void C64::releaseAllKeys() {
    for (auto& k : s_.keys) k = 0;
    updateLightPen();
}

void C64::setJoystick(int port, uint8_t lines) {
    if (port == 1 || port == 2) s_.joy[port - 1] = lines & 0x1F;
    updateLightPen();
}

void C64::setRestore(bool down) { s_.restore = down ? 1 : 0; }

// The keyboard connects port A lines (columns) to port B lines (rows) wherever a key is
// down; a line pulled low on either side pulls the other low too, through every closed
// key, so ghost keys appear as on the real matrix. Control port 2 shares port A, port 1
// port B.
void C64::keyboardLines(uint8_t paDriven, uint8_t pbDriven, uint8_t& pa, uint8_t& pb) const {
    pa = static_cast<uint8_t>(paDriven & ~s_.joy[1]);
    pb = static_cast<uint8_t>(pbDriven & ~s_.joy[0]);
    for (bool changed = true; changed;) {
        changed = false;
        for (int c = 0; c < 8; ++c) {
            const uint8_t k = s_.keys[c];
            if (!k) continue;
            if (!(pa & (1 << c)) && (pb & k)) { pb = static_cast<uint8_t>(pb & ~k); changed = true; }
            if ((pa & (1 << c)) && (~pb & k)) { pa = static_cast<uint8_t>(pa & ~(1 << c)); changed = true; }
        }
    }
}

uint8_t C64::ciaPortA(int cia, uint8_t driven) {
    if (cia == 0) {
        uint8_t pa, pb;
        keyboardLines(driven, cia1_.portBOut(), pa, pb);
        return pa;
    }
    // CIA 2: serial bus. The outputs (bits 3-5) drive the lines through inverters; the
    // inputs read CLK (bit 6) and DATA (bit 7). No device is attached yet.
    const bool clk = !(driven & 0x10), data = !(driven & 0x20);
    return static_cast<uint8_t>((driven & 0x3F) | (clk ? 0x40 : 0) | (data ? 0x80 : 0)) &
           static_cast<uint8_t>(driven | 0x3F);
}

uint8_t C64::ciaPortB(int cia, uint8_t driven) {
    if (cia == 0) {
        uint8_t pa, pb;
        keyboardLines(cia1_.portAOut(), driven, pa, pb);
        return pb;
    }
    return driven;   // user port: pulled up, nothing attached
}

// ---- the cycle ------------------------------------------------------------------

void C64::tick() {
    vic_.phase1();
    s_.vicLastByte = vic_.lastByte();
    s_.aecLow = vic_.aecLow();
    // The CIAs' interrupt outputs reach the 6510 one cycle after they change (Lorenz's
    // IMR, IRQ and NMI programs; hw/cia.md rule 19).
    // The VIC-II's interrupt output reaches the 6510 one cycle late: the CPU sees it as it
    // was after the VIC's first phase of the last cycle. A raster interrupt raised in cycle 1
    // is seen from cycle 2; an acknowledge written in cycle N still shows low in N+1 (the
    // open-collector line is slow to rise; hw/vic2.md rule 15).
    // (irqEarly: hw/cia.md rule 20.)
    cpu_.setIrq(s_.vicIrqSeen || s_.cia1IrqSeen || cia1_.irqEarly());
    s_.vicIrqSeen = vic_.irq();
    cpu_.setNmi(s_.cia2IrqSeen || cia2_.irqEarly() || s_.restore);
    // RDY is sampled in the second phase: a read in the first cycle of BA low is the one
    // that does not complete, and the cycle after BA rises is the one that does. In the
    // CPU core's terms (a halt is decided before a cycle), that is BA of the last cycle
    // (hw/cpu.md rule 14a).
    cpu_.setRdy(!s_.baLowSeen);
    s_.baLowSeen = !vic_.ba();
    s_.cia1IrqSeen = cia1_.irq();
    s_.cia2IrqSeen = cia2_.irq();
    cpu_.tick();
    cia1_.clock();
    cia2_.clock();
    // The TOD pins see the mains frequency (hw/cia.md rule 18): exactly mainsHz ticks per
    // second of machine cycles, spread evenly.
    s_.todCount += static_cast<uint32_t>(timing_->mainsHz);
    if (s_.todCount >= static_cast<uint32_t>(timing_->clockHz + 0.5)) {
        s_.todCount -= static_cast<uint32_t>(timing_->clockHz + 0.5);
        cia1_.todTick();
        cia2_.todTick();
    }
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
    return 4 + 4 + 2 + sizeof(State) + sizeof(Vic2::State) + 2 * sizeof(Cia6526::State) + ram_.size() + colorRam_.size() + sizeof(Cpu6502<CpuBus>::Snapshot);
}

void C64::saveState(uint8_t* out) const {
    auto put = [&out](const void* p, size_t n) { std::memcpy(out, p, n); out += n; };
    put(&kStateMagic, 4);
    put(&kStateVersion, 4);
    const uint8_t model = static_cast<uint8_t>(timing_->model);
    put(&model, 1);
    const uint8_t cia = static_cast<uint8_t>(ciaModel());
    put(&cia, 1);
    put(&s_, sizeof s_);
    const Vic2::State vs = vic_.state();
    put(&vs, sizeof vs);
    const Cia6526::State c1 = cia1_.state(), c2 = cia2_.state();
    put(&c1, sizeof c1);
    put(&c2, sizeof c2);
    put(ram_.data(), ram_.size());
    put(colorRam_.data(), colorRam_.size());
    const auto snap = cpu_.snapshot();
    put(&snap, sizeof snap);
}

bool C64::loadState(const uint8_t* in, size_t size) {
    if (size < stateSize()) return false;
    auto get = [&in](void* p, size_t n) { std::memcpy(p, in, n); in += n; };
    uint32_t magic = 0, version = 0;
    uint8_t model = 0, cia = 0;
    get(&magic, 4);
    get(&version, 4);
    get(&model, 1);
    get(&cia, 1);
    if (magic != kStateMagic || version != kStateVersion || model != static_cast<uint8_t>(timing_->model) ||
        cia != static_cast<uint8_t>(ciaModel()))
        return false;
    State s;
    get(&s, sizeof s);
    Vic2::State vs;
    get(&vs, sizeof vs);
    Cia6526::State c1, c2;
    get(&c1, sizeof c1);
    get(&c2, sizeof c2);
    if (vs.cycle < 1 || vs.cycle > timing_->cyclesPerLine || vs.line < 0 || vs.line >= timing_->linesPerFrame ||
        vs.vmli >= 40 || vs.dmli > 40 || vs.xscrollPipe > 7)
        return false;
    s_ = s;
    vic_.setState(vs);
    cia1_.setState(c1);
    cia2_.setState(c2);
    get(ram_.data(), ram_.size());
    get(colorRam_.data(), colorRam_.size());
    Cpu6502<CpuBus>::Snapshot snap;
    get(&snap, sizeof snap);
    cpu_.restore(snap);
    return true;
}

} // namespace k64

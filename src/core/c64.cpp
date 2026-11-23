#include "c64.h"

#include "pla.h"

#include <algorithm>
#include <cstring>

namespace k64 {

namespace {
constexpr uint32_t kStateMagic = 0x3436'4B32; // "K64" + format generation
constexpr uint32_t kStateVersion = 16;

// The 1541 runs on its own 16 MHz crystal divided by 16 (hw/drive.md D1): 1 MHz. Against the
// machine's clock that is exactly 18,000,000 / 17,734,475 drive cycles per machine cycle on
// PAL (17.734475 MHz / 18) and 308 / 315 = 44 / 45 on NTSC (315/22 MHz / 14).
void driveRatio(const Timing& t, uint32_t& num, uint32_t& den) {
    if (t.clockHz < 1.0e6) { num = 18'000'000; den = 17'734'475; }
    else { num = 44; den = 45; }
}
}

C64::C64(VicModel model) : timing_(&timingFor(model)) {
    vic_.setTiming(*timing_);
    driveRatio(*timing_, driveNum_, driveDen_);
    reset();
}

void C64::setRoms(const RomSet& roms) {
    romsOk_ = roms.has(RomRole::Basic) && roms.has(RomRole::Kernal) && roms.has(RomRole::Chargen);
    if (roms.has(RomRole::Basic)) std::copy_n(roms.get(RomRole::Basic).begin(), basic_.size(), basic_.begin());
    if (roms.has(RomRole::Kernal)) std::copy_n(roms.get(RomRole::Kernal).begin(), kernal_.size(), kernal_.begin());
    if (roms.has(RomRole::Chargen)) std::copy_n(roms.get(RomRole::Chargen).begin(), chargen_.size(), chargen_.begin());
    drive_.setRoms(roms);
}

void C64::setModel(VicModel model) {
    timing_ = &timingFor(model);
    vic_.setTiming(*timing_);
    driveRatio(*timing_, driveNum_, driveDen_);
    setAudioRate(audioRate_);   // the clock changed
    reset();
}

void C64::setAudioRate(double sampleRate) {
    audioRate_ = sampleRate;
    if (audioRate_ <= 0.0) return;
    boardFilter_.configure(timing_->clockHz);
    resampler_.configure(timing_->clockHz, audioRate_);
}

void C64::reset() {
    s_ = State{};
    // DRAM at power on: the common pattern of the C64's RAM chips, $00 and $FF alternating
    // in blocks of 64 bytes. (Software must not depend on it; a fixed pattern keeps runs
    // reproducible.)
    for (size_t i = 0; i < ram_.size(); ++i) ram_[i] = (i & 0x40) ? 0xFF : 0x00;
    colorRam_.fill(0);
    vic_.reset();
    sid_.powerOn();   // the oscillators start from their power-on value
    sid_.setClockHz(timing_->clockHz);
    boardFilter_.reset();
    cia1_.reset();
    cia2_.reset();
    cpu_.reset();
    cart_.reset();
    drive_.setBus(false, false, false);
    drive_.reset();   // the serial bus's RESET line is the machine's
    s_.autostartPending = autostart_.data.empty() ? 0 : 1;
    updatePots();
}

void C64::setAutostart(const ProgramFile& program, uint64_t delayCycles) {
    autostart_ = program;
    autostartDelay_ = delayCycles;
    s_.autostartPending = autostart_.data.empty() ? 0 : 1;
    s_.autostartReadyAt = 0;
}

// What BASIC's LOAD leaves behind, without a drive: the bytes in memory, the end address
// as the start of variables and as the KERNAL's end of load; then RUN and RETURN in the
// keyboard buffer. (RUN sets the start of arrays and of free space from the start of
// variables itself, as LOAD does.)
void C64::injectProgram() {
    const uint16_t load = autostart_.load;
    const auto end = static_cast<uint16_t>(load + autostart_.data.size());
    for (size_t i = 0; i < autostart_.data.size(); ++i) ram_[static_cast<uint16_t>(load + i)] = autostart_.data[i];
    for (int p : {0x2D, 0xAE}) {
        ram_[static_cast<size_t>(p)] = static_cast<uint8_t>(end);
        ram_[static_cast<size_t>(p) + 1] = static_cast<uint8_t>(end >> 8);
    }
    const uint8_t run[] = {'R', 'U', 'N', 13};
    for (size_t i = 0; i < 4; ++i) ram_[0x0277 + i] = run[i];
    ram_[0xC6] = 4;
    s_.autostartPending = 0;
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
    if (cart_.exrom()) in |= pla::nEXROM;
    if (cart_.game()) in |= pla::nGAME;
    // #AEC low (CPU cycle), #CAS low (access in progress), VA lines irrelevant here.
    return in;
}

unsigned C64::vicBank() const {
    // CIA 2 port A bits 0-1, inverted; inputs are pulled up, which selects bank 0.
    return 3u - (cia2_.portAOut() & 3u);
}

uint16_t C64::plaInputsVic(uint16_t vaddr, bool cpuHalf) const {
    const unsigned bank = vicBank();
    uint16_t in = pla::nAEC | pla::RnW | pla::nLORAM | pla::nHIRAM | pla::nCHAREN |
                  pla::BA | pla::A15 | pla::A14 | pla::A13 | pla::A12; // address bus pulled up in VIC cycles
    // (Some cartridges pull the two lines in the CPU's half of the cycle only.)
    if (cpuHalf ? cart_.exrom() : cart_.exromVic()) in |= pla::nEXROM;
    if (cpuHalf ? cart_.game() : cart_.gameVic()) in |= pla::nGAME;
    if (!(bank & 1)) in |= pla::nVA14;
    if (vaddr & 0x2000) in |= pla::VA13;
    if (vaddr & 0x1000) in |= pla::VA12;
    return in;
}

bool C64::vicIsCharRom(uint16_t vaddr) const { return !(pla::table()[plaInputsVic(vaddr)] & pla::nCHARROM); }

uint8_t C64::vicFetch(uint16_t vaddr, bool cpuHalf) const {
    const uint8_t out = pla::table()[plaInputsVic(vaddr, cpuHalf)];
    if (!(out & pla::nCHARROM)) return chargen_[vaddr & 0x0FFF];
    // With GAME low and EXROM high the VIC-II's upper 4K are the cartridge's ROMH; the
    // cartridge sees the pulled-up address lines, $F000-$FFFF (hw/cart.md P3).
    if (!(out & pla::nROMH)) return cart_.peekRomH(static_cast<uint16_t>(0xF000 | (vaddr & 0x0FFF)));
    return ram_[(vicBank() << 14) | (vaddr & 0x3FFF)];
}

// ---- CPU accesses ---------------------------------------------------------------

uint8_t C64::peekCpu(uint16_t a) const {
    if (a == 0) return s_.portDdr;
    if (a == 1) return portRead();
    if (cart_.claims(a)) return cart_.readClaimed(a);
    const uint8_t out = pla::table()[plaInputsCpu(a, true)];
    if (!(out & pla::nBASIC)) return basic_[a & 0x1FFF];
    if (!(out & pla::nKERNAL)) return kernal_[a & 0x1FFF];
    if (!(out & pla::nCHARROM)) return chargen_[a & 0x0FFF];
    if (!(out & pla::nROML)) return cart_.peekRomL(a, s_.vicLastByte, ram_[a]);
    if (!(out & pla::nROMH)) return cart_.peekRomH(a);
    if (!(out & pla::nCASRAM)) return ram_[a];
    return s_.vicLastByte;
}

uint8_t C64::cpuRead(uint16_t a) {
    const uint8_t v = cpuReadBus(a);
    s_.cpuData = v;
    if (cart_.watchesCycles()) cart_.cpuCycle(false);
    return v;
}

uint8_t C64::cpuReadBus(uint16_t a) {
    // A halted read while the VIC-II holds the second phase never reaches the bus.
    if (s_.aecLow) return s_.vicLastByte;
    if (a == 0) return s_.portDdr;
    if (a == 1) return portRead();
    if (cart_.claims(a)) return cart_.readClaimed(a);   // a cartridge that decodes the address itself
    const uint8_t out = pla::table()[plaInputsCpu(a, true)];
    if (!(out & pla::nBASIC)) return basic_[a & 0x1FFF];
    if (!(out & pla::nKERNAL)) return kernal_[a & 0x1FFF];
    if (!(out & pla::nCHARROM)) return chargen_[a & 0x0FFF];
    if (!(out & pla::nIO)) return ioRead(a);
    if (!(out & pla::nROML)) return cart_.readRomL(a, s_.vicLastByte, ram_[a]);
    if (!(out & pla::nROMH)) return cart_.readRomH(a, s_.vicLastByte);
    if (!(out & pla::nCASRAM)) return ram_[a];
    return s_.vicLastByte; // nothing selected: the data bus still holds the VIC-II's byte
}

void C64::cpuWrite(uint16_t a, uint8_t v) {
    cpuWriteBus(a, v);
    // A cartridge waiting for the CPU to take an interrupt counts its write cycles.
    if (cart_.watchesCycles()) cart_.cpuCycle(true);
}

void C64::cpuWriteBus(uint16_t a, uint8_t v) {
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
    if (cart_.claims(a)) {
        cart_.writeClaimed(a, v);
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
    // In the Ultimax configuration the PLA selects the cartridge's chips for writes too.
    if (!(out & pla::nROML)) cart_.writeRomL(a, v);
    else if (!(out & pla::nROMH)) cart_.writeRomH(a, v);
    else if ((a & 0xC000) == 0x8000 && cart_.seesBus()) {
        // A cartridge with RAM of its own decodes $8000-$BFFF itself; RAM where ROMH is
        // read from keeps the write from the computer's RAM.
        const bool romhForReads = !(pla::table()[plaInputsCpu(a, true)] & pla::nROMH);
        if (cart_.writeMem(a, v, romhForReads)) return;
    }
    if (!(out & pla::nCASRAM)) ram_[a] = v;
}

// $D000-$DFFF with I/O selected: VIC-II, SID, color RAM, CIA 1, CIA 2, I/O 1, I/O 2.
uint8_t C64::ioRead(uint16_t a) {
    switch ((a >> 8) & 0x0F) {
    case 0x0: case 0x1: case 0x2: case 0x3:
        return vic_.read(a);
    case 0x4: case 0x5: case 0x6: case 0x7:
        return sid_.read(a);   // 32 registers, repeated through $D400-$D7FF
    case 0x8: case 0x9: case 0xA: case 0xB:
        // Color RAM is 4 bits wide; the upper bits are whatever is left on the bus.
        return static_cast<uint8_t>((s_.vicLastByte & 0xF0) | colorRam_[a & 0x03FF]);
    case 0xC: return cia1_.read(a);
    case 0xD: return cia2_.read(a);
    default: {
        // I/O 1 and I/O 2: the cartridge's; what it does not answer leaves the bus as it is.
        uint8_t v = s_.vicLastByte;
        return cart_.readIo(a, v) ? v : s_.vicLastByte;
    }
    }
}

void C64::ioWrite(uint16_t a, uint8_t v) {
    switch ((a >> 8) & 0x0F) {
    case 0x0: case 0x1: case 0x2: case 0x3:
        vic_.write(a, v);
        return;
    case 0x4: case 0x5: case 0x6: case 0x7:
        if (debugCart_ && a == 0xD7FF) debugExit_ = v;
        sid_.write(a, v);
        return;
    case 0xC:
        cia1_.write(a, v);
        if ((a & 0x0F) < 4) updateLightPen();   // ports and directions
        return;
    case 0xD: cia2_.write(a, v); return;
    default: cart_.writeIo(a, v); return;
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

void C64::setPaddles(int port, int x, int y) {
    if (port != 1 && port != 2) return;
    s_.pot[port - 1][0] = static_cast<int16_t>(x < 0 ? -1 : x > 255 ? 255 : x);
    s_.pot[port - 1][1] = static_cast<int16_t>(y < 0 ? -1 : y > 255 ? 255 : y);
    s_.potAny = 0;
    for (const auto& p : s_.pot)
        if (p[0] >= 0 || p[1] >= 0) s_.potAny = 1;
    updatePots();
}

// The SID's two paddle inputs reach the control ports through an analog switch: CIA 1
// port A line 6 high connects port 1's pair, line 7 high port 2's (the levels of the
// pins, so a line that is an input, or pulled low through the keyboard, counts as it
// stands). With both on, the two ports' resistances are in parallel.
void C64::updatePots() {
    uint32_t charge[2] = {0, 0};
    if (s_.potAny) {
        const uint8_t pa = ciaPortA(0, cia1_.portAOut());
        for (int port = 0; port < 2; ++port) {
            if (!(pa & (0x40 << port))) continue;
            for (int axis = 0; axis < 2; ++axis)
                if (s_.pot[port][axis] >= 0) charge[axis] += Sid::potChargeFor(static_cast<unsigned>(s_.pot[port][axis]));
        }
    }
    sid_.setPotCharge(0, charge[0]);
    sid_.setPotCharge(1, charge[1]);
}

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
    // CIA 2: serial bus. The outputs (bits 3-5: ATN, CLK, DATA) pull the lines through
    // inverters; the inputs read CLK (bit 6) and DATA (bit 7) as they are: low when any
    // device pulls them.
    const bool drive = driveAttached();
    const bool clk = !(driven & 0x10) && !(drive && drive_.pullsClk());
    const bool data = !(driven & 0x20) && !(drive && drive_.pullsData());
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
    cpu_.setIrq(s_.vicIrqSeen || s_.cia1IrqSeen || cia1_.irqEarly() || cart_.irq());
    s_.vicIrqSeen = vic_.irq();
    cpu_.setNmi(s_.cia2IrqSeen || cia2_.irqEarly() || s_.restore || cart_.nmi());
    // RDY is sampled in the second phase: a read in the first cycle of BA low is the one
    // that does not complete, and the cycle after BA rises is the one that does. In the
    // CPU core's terms (a halt is decided before a cycle), that is BA of the last cycle
    // (hw/cpu.md rule 14a).
    cpu_.setRdy(!s_.baLowSeen);
    s_.baLowSeen = !vic_.ba();
    s_.cia1IrqSeen = cia1_.irq();
    s_.cia2IrqSeen = cia2_.irq();
    cpu_.tick();
    if (s_.potAny) updatePots();
    sid_.clock();
    if (audioRate_ > 0.0) resampler_.push(static_cast<float>(boardFilter_.clock(sid_.clockAudio())));
    cia1_.clock();
    cia2_.clock();
    if (cart_.clocked()) cart_.clock();
    if (driveAttached()) {
        // The drive sees the lines as our CIA 2 pulls them after this cycle's access, and
        // runs the cycles its own clock has reached.
        const uint8_t pa = cia2_.portAOut();
        drive_.setBus((pa & 0x08) != 0, (pa & 0x10) != 0, (pa & 0x20) != 0);
        s_.drivePhase += driveNum_;
        while (s_.drivePhase >= driveDen_) {
            s_.drivePhase -= driveDen_;
            drive_.tick();
        }
    }
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
    // The KERNAL's input loop polls the keyboard buffer at $E5CD: from there on it is ready.
    if (s_.autotypePos < autotype_.size() && cpu_.regs().pc == 0xE5CD && ram_[0xC6] == 0) {
        size_t n = 0;
        while (n < 10 && s_.autotypePos < autotype_.size()) {
            const char c = autotype_[s_.autotypePos++];
            ram_[0x0277 + n++] = static_cast<uint8_t>(c);
            if (c == '\r') break;   // one line at a time: what follows waits for the next prompt
        }
        ram_[0xC6] = static_cast<uint8_t>(n);
    }
    if (s_.autostartPending && cpu_.regs().pc == 0xE5CD) {
        if (!s_.autostartReadyAt) s_.autostartReadyAt = s_.cycle;
        if (s_.cycle >= s_.autostartReadyAt + autostartDelay_) injectProgram();
    }
}

void C64::runFrame() {
    // Ends where vertical blanking starts, so the visible picture of one call is one frame.
    const int stop = timing_->firstVblankLine;
    do tick();
    while (!(vic_.line() == stop && vic_.cycleInLine() == 1));
}

void C64::visibleFrame(uint32_t* out) const {
    const auto& pal = vicPalette(timing_->firstRevision);
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
    return 4 + 4 + 3 + sizeof(State) + sizeof(Vic2::State) + 2 * sizeof(Cia6526::State) + sizeof(Sid::State) + sizeof(Cartridge::State) + cart_.contentSize() + sizeof(BoardFilter::State) + ram_.size() + colorRam_.size() + sizeof(Cpu6502<CpuBus>::Snapshot) + C1541::stateSize();
}

void C64::saveState(uint8_t* out) const {
    auto put = [&out](const void* p, size_t n) { std::memcpy(out, p, n); out += n; };
    put(&kStateMagic, 4);
    put(&kStateVersion, 4);
    const uint8_t model = static_cast<uint8_t>(timing_->model);
    put(&model, 1);
    const uint8_t cia = static_cast<uint8_t>(ciaModel());
    put(&cia, 1);
    const uint8_t sidModelByte = static_cast<uint8_t>(sidModel());
    put(&sidModelByte, 1);
    put(&s_, sizeof s_);
    const Vic2::State vs = vic_.state();
    put(&vs, sizeof vs);
    const Cia6526::State c1 = cia1_.state(), c2 = cia2_.state();
    put(&c1, sizeof c1);
    put(&c2, sizeof c2);
    const Sid::State ss = sid_.state();
    put(&ss, sizeof ss);
    const Cartridge::State cs = cart_.state();
    put(&cs, sizeof cs);
    const BoardFilter::State bs = boardFilter_.state();
    put(&bs, sizeof bs);
    put(ram_.data(), ram_.size());
    put(colorRam_.data(), colorRam_.size());
    const auto snap = cpu_.snapshot();
    put(&snap, sizeof snap);
    drive_.saveState(out);
    out += C1541::stateSize();
    cart_.saveContent(out);
}

bool C64::loadState(const uint8_t* in, size_t size) {
    if (size < stateSize()) return false;
    auto get = [&in](void* p, size_t n) { std::memcpy(p, in, n); in += n; };
    uint32_t magic = 0, version = 0;
    uint8_t model = 0, cia = 0, sidModelByte = 0;
    get(&magic, 4);
    get(&version, 4);
    get(&model, 1);
    get(&cia, 1);
    get(&sidModelByte, 1);
    if (magic != kStateMagic || version != kStateVersion || model != static_cast<uint8_t>(timing_->model) ||
        cia != static_cast<uint8_t>(ciaModel()) || sidModelByte != static_cast<uint8_t>(sidModel()))
        return false;
    State s;
    get(&s, sizeof s);
    Vic2::State vs;
    get(&vs, sizeof vs);
    Cia6526::State c1, c2;
    get(&c1, sizeof c1);
    get(&c2, sizeof c2);
    Sid::State ss;
    get(&ss, sizeof ss);
    Cartridge::State cs;
    get(&cs, sizeof cs);
    BoardFilter::State bs;
    get(&bs, sizeof bs);
    if (vs.cycle < 1 || vs.cycle > timing_->cyclesPerLine || vs.line < 0 || vs.line >= timing_->linesPerFrame ||
        vs.vmli >= 40 || vs.dmli > 40 || vs.xscrollPipe > 7)
        return false;
    if (!cart_.setState(cs)) return false;   // a state of another cartridge
    s_ = s;
    vic_.setState(vs);
    cia1_.setState(c1);
    cia2_.setState(c2);
    sid_.setState(ss);
    updatePots();
    boardFilter_.setState(bs);
    get(ram_.data(), ram_.size());
    get(colorRam_.data(), colorRam_.size());
    Cpu6502<CpuBus>::Snapshot snap;
    get(&snap, sizeof snap);
    cpu_.restore(snap);
    drive_.loadState(in);
    in += C1541::stateSize();
    cart_.loadContent(in);
    return true;
}

} // namespace k64

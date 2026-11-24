// The CMD HD's board. Rules: local-notes/hw/cmdhd.md.
#include "cmdhd.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace k64 {

CmdHd::CmdHd() = default;

bool CmdHd::setRom(const uint8_t* data, size_t size) {
    romOk_ = false;
    if (size == 32768) {
        // A 27256 image holding the 16K ROM twice: either half is the ROM, if they agree.
        if (std::memcmp(data, data + 16384, 16384) != 0) return false;
    } else if (size != 16384) {
        return false;
    }
    std::copy_n(data, 16384, rom_.begin());
    romOk_ = true;
    return true;
}

void CmdHd::reset() {
    ram_.fill(0);
    latch_ = 0xFF;
    busLast_ = 0;
    via1_.reset();
    via2_.reset();
    ppi_.reset();
    scsi_.reset();
    cpu_.reset();
    ackLow_ = false;
    cycle_ = 0;
    via1_.setCa1(!atnLow_);
}

void CmdHd::pressReset() {
    // The panel's RESET restarts the board; RAM keeps what it holds (the warm-start
    // signature at $90F0 tells the ROM so, hw/cmdhd.md).
    via1_.reset();
    via2_.reset();
    ppi_.reset();
    cpu_.reset();
    ackLow_ = false;
    via1_.setCa1(!atnLow_);
}

// ---- address decoding (hw/cmdhd.md "Memory map") -----------------------------------

uint8_t CmdHd::read(uint16_t a) {
    uint8_t v;
    if (a < 0x4000) v = ram_[a];
    else if (a < 0x8000) v = (portC() & 2) ? ram_[a] : ram_[(a & 0x3FFF) | 0xC000];
    else if (a < 0x9000) {
        switch ((a >> 8) & 15) {
        case 0x0: case 0x1: v = via1_.read(a & 15); break;
        case 0x4: case 0x5: v = via2_.read(a & 15); break;
        case 0x8: case 0x9: v = ppi_.read(a & 3, ppiPins()); break;
        case 0xC: case 0xD: v = static_cast<uint8_t>((rtc_.read(a & 15) & 0x0F) | (busLast_ & 0xF0)); break;
        default: v = ram_[a]; break;
        }
    } else if (a < 0xC000) v = ram_[a];
    else v = (portC() & 1) ? rom_[a & 0x3FFF] : ram_[a];
    busLast_ = v;
    return v;
}

void CmdHd::write(uint16_t a, uint8_t v) {
    busLast_ = v;
    const bool writable = latch_ & 0x20;   // latch bit 5: RAM above $8000 may be written
    if (a < 0x4000) { ram_[a] = v; return; }
    if (a < 0x8000) { ram_[(portC() & 2) ? a : ((a & 0x3FFF) | 0xC000)] = v; return; }
    if (a < 0x9000) {
        switch ((a >> 8) & 15) {
        case 0x0: case 0x1: via1_.write(a & 15, v); return;
        case 0x4: case 0x5: via2_.write(a & 15, v); scsiLines(); return;
        case 0x8: case 0x9: ppi_.write(a & 3, v); scsiLines(); return;
        case 0xC: case 0xD: rtc_.write(a & 15, v); return;
        case 0xE: ram_[a] = v; return;
        case 0xF: latch_ = v; ram_[a] = v; return;   // RAM, and every write also goes to the latch
        default: if (writable) ram_[a] = v; return;
        }
    }
    if (writable) ram_[a] = v;
}

Ppi8255::Pins CmdHd::ppiPins() const {
    Ppi8255::Pins p;
    // Port B: PB1 SWAP 8, PB2 SWAP 9, PB3 WRITE PROTECT (active low); the parallel cable's
    // lines (PB0, PB6, PB7) and port A read high with nothing connected.
    p.b = static_cast<uint8_t>(0xFF & ~((swap8_ ? 2 : 0) | (swap9_ ? 4 : 0) | (wp_ ? 8 : 0)));
    return p;
}

// ---- SCSI (hw/cmdhd.md "SCSI") ------------------------------------------------------

void CmdHd::scsiLines() {
    const uint8_t pc = portC();
    const uint8_t pb = static_cast<uint8_t>(via2_.outB() | ~via2_.ddrB());
    const bool sel = via2_.ddrB() & pb & 0x10;
    // The data bus as the host drives it (inverted: a 0 bit is a true line).
    const uint8_t data = via2_.ddrA() ? static_cast<uint8_t>(~via2_.outA() & via2_.ddrA()) : 0;
    scsi_.setLines(sel, pc & 4, pc & 8, data);
}

uint8_t CmdHd::viaPortA(int via, uint8_t out, uint8_t ddr) {
    if (via == 0) return static_cast<uint8_t>(out | ~ddr);   // U10 port A: not connected
    // The SCSI data bus: the target drives it in the phases with I/O true.
    const auto ph = scsi_.phase();
    const bool targetDrives = ph == ScsiDisk::Phase::DataIn || ph == ScsiDisk::Phase::Status || ph == ScsiDisk::Phase::MessageIn;
    uint8_t pins = static_cast<uint8_t>(out | ~ddr);
    if (targetDrives) pins &= static_cast<uint8_t>(~scsi_.data());
    return pins;
}

uint8_t CmdHd::viaPortB(int via, uint8_t out, uint8_t ddr) {
    uint8_t pins = static_cast<uint8_t>(out | ~ddr);
    if (via == 0) {
        // The 1541's serial port (C1541::viaPortB) + PB5 (fast-serial direction, an output)
        // + PB6 (an input with nothing on it: high).
        pins &= 0x3A;
        if (dataLow_ || pullsData()) pins |= 0x01;
        if (clkLow_ || pullsClk()) pins |= 0x04;
        pins |= 0x40;
        if (atnLow_) pins |= 0x80;
        return pins;
    }
    // U9 port B: REQ, ACK, BSY (through the board's logic: with CB2 high it shows SEL AND
    // BSY), SEL and the direction as driven, and the phase code (hw/cmdhd.md).
    using P = ScsiDisk::Phase;
    uint8_t code = 5;   // bus free reads as STATUS's code
    switch (scsi_.phase()) {
    case P::DataOut: code = 0; break;
    case P::DataIn: code = 4; break;
    case P::Command: code = 1; break;
    case P::Status: code = 5; break;
    case P::MessageOut: code = 3; break;
    case P::MessageIn: code = 7; break;
    default: break;
    }
    const bool sel = (pins & 0x10) != 0;
    const bool bsy = via2_.cb2Out() ? (sel && scsi_.bsy()) : (!sel && scsi_.bsy());
    return static_cast<uint8_t>((scsi_.req() ? 0x80 : 0) | (!via2_.ca2Out() ? 0x40 : 0) | (bsy ? 0x20 : 0) |
                                (pins & 0x18) | code);
}

void CmdHd::viaOutputs(int via) {
    if (via != 1) return;
    // ACK = CA2's pulse, made by each access to port A: one byte handed over.
    const bool ackLow = !via2_.ca2Out();
    if (ackLow && !ackLow_) { scsiLines(); scsi_.ack(); }
    ackLow_ = ackLow;
    scsiLines();
}

// ---- serial bus (as C1541) ----------------------------------------------------------

bool CmdHd::pullsClk() const {
    return ((via1_.outB() | ~via1_.ddrB()) & 0x08) != 0;
}

// DATA out (PB1), or the ATN acknowledge gate: with ATN true at the line and PB4 (ATNA) the
// same, the gate pulls DATA. The ROM leaves PB4 set while idle (ORB $D5, $DE80), clears it to
// answer ATN ($C5B0) and sets it again after ($C628): the 1541's gate the other way round, as
// its CA1 is (hw/cmdhd.md).
bool CmdHd::pullsData() const {
    const uint8_t pins = static_cast<uint8_t>(via1_.outB() | ~via1_.ddrB());
    const bool atna = (pins & 0x10) != 0;
    return (pins & 0x02) != 0 || atna == atnLow_;
}

void CmdHd::setBus(bool atnLow, bool clkLow, bool dataLow) {
    atnLow_ = atnLow;
    clkLow_ = clkLow;
    dataLow_ = dataLow;
    // CA1 sees the ATN line itself (low = asserted): the ROM sets CA1 to its falling edge
    // (PCR $00, $DE9E), where the 1541 takes ATN through an inverter on a rising edge.
    via1_.setCa1(!atnLow);
}

void CmdHd::tick() {
    rtc_.tick();
    cpu_.setIrq(via1_.irq() || via2_.irq());
    if (!watch.empty() && watchLeft > 0 && cpu_.atInstructionStart()) {
        const uint16_t pc = cpu_.regs().pc;
        for (uint16_t w : watch)
            if (w == pc) {
                --watchLeft;
                std::fprintf(stderr, "hd: %04X at cycle %llu a=%02X x=%02X y=%02X pb1=%02X\n", pc,
                             static_cast<unsigned long long>(cycle_), cpu_.regs().a, cpu_.regs().x, cpu_.regs().y,
                             via1_.peek(0));
            }
    }
    cpu_.tick();
    via1_.clock();
    via2_.clock();
    viaOutputs(1);   // the pulse's end
    ++cycle_;
}

} // namespace k64

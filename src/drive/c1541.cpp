#include "c1541.h"

#include <algorithm>
#include <cstring>

namespace k64 {

C1541::C1541() { reset(); }

void C1541::setRoms(const RomSet& roms) {
    romsOk_ = roms.has(RomRole::Drive1541Low) && roms.has(RomRole::Drive1541High);
    if (!romsOk_) return;
    std::copy_n(roms.get(RomRole::Drive1541Low).begin(), 8192, rom_.begin());
    std::copy_n(roms.get(RomRole::Drive1541High).begin(), 8192, rom_.begin() + 8192);
}

void C1541::reset() {
    for (size_t i = 0; i < ram_.size(); ++i) ram_[i] = (i & 0x40) ? 0xFF : 0x00;
    via1_.reset();
    via2_.reset();
    cpu_.reset();
    cycle_ = 0;
    via1_.setCa1(atnLow_);
}

// The address decoder (hw/drive.md B1): the upper half of the address space is the ROM,
// twice; the lower half repeats every $2000: RAM at $0000-$07FF, the serial bus's 6522
// at $1800-$1BFF, the disk's at $1C00-$1FFF, nothing in between.
uint8_t C1541::read(uint16_t a) {
    if (a & 0x8000) return rom_[a & 0x3FFF];
    switch (a & 0x1C00) {
    case 0x0000: case 0x0400: return ram_[a & 0x07FF];
    case 0x1800: return via1_.read(a & 15);
    case 0x1C00: return via2_.read(a & 15);
    default: return static_cast<uint8_t>(a >> 8);   // nothing answers: the bus keeps the address's high byte
    }
}

void C1541::write(uint16_t a, uint8_t v) {
    if (a & 0x8000) return;
    switch (a & 0x1C00) {
    case 0x0000: case 0x0400: ram_[a & 0x07FF] = v; break;
    case 0x1800: via1_.write(a & 15, v); break;
    case 0x1C00: via2_.write(a & 15, v); break;
    default: break;
    }
}

// ---- the serial bus (hw/drive.md B2) -----------------------------------------------
//
// Port B of the first 6522 through inverters: PB1 high pulls DATA, PB3 high pulls CLK;
// PB0, PB2 and PB7 read 1 while DATA, CLK and ATN are low. PB4 (ATNA) and the ATN line
// go through an exclusive OR onto DATA: the drive answers ATN without its CPU. A pin
// the chip does not drive floats high at the inverter.

bool C1541::pullsClk() const {
    return ((via1_.outB() | ~via1_.ddrB()) & 0x08) != 0;
}

bool C1541::pullsData() const {
    const uint8_t pins = static_cast<uint8_t>(via1_.outB() | ~via1_.ddrB());
    const bool atna = (pins & 0x10) != 0;
    return (pins & 0x02) != 0 || atna != atnLow_;
}

void C1541::setBus(bool atnLow, bool clkLow, bool dataLow) {
    atnLow_ = atnLow;
    clkLow_ = clkLow;
    dataLow_ = dataLow;
    via1_.setCa1(atnLow);   // ATN, inverted, is also the chip's CA1 input
}

uint8_t C1541::viaPortA(int, uint8_t out, uint8_t ddr) {
    // Neither port A has anything on it that pulls (the disk's data path is not built).
    return static_cast<uint8_t>(out | ~ddr);
}

uint8_t C1541::viaPortB(int via, uint8_t out, uint8_t ddr) {
    uint8_t pins = static_cast<uint8_t>(out | ~ddr);
    if (via == 0) {
        pins &= 0x1A;                                         // PB1, PB3, PB4: the chip's own
        if (dataLow_ || pullsData()) pins |= 0x01;
        if (clkLow_ || pullsClk()) pins |= 0x04;
        pins |= static_cast<uint8_t>(jumpers_ << 5);          // PB5, PB6: the device number
        if (atnLow_) pins |= 0x80;
        return pins;
    }
    // The disk's 6522: no disk, so no SYNC (PB7 high) and the write protect sensor sees light (PB4 high).
    return pins;
}

// PB3 and PB2 of the disk's 6522, through drivers: a pin the chip does not drive yet
// floats high, so both are on from power on until the ROM has set the port up.
bool C1541::led() const { return ((via2_.outB() | ~via2_.ddrB()) & 0x08) != 0; }
bool C1541::motor() const { return ((via2_.outB() | ~via2_.ddrB()) & 0x04) != 0; }

// ---- save states ------------------------------------------------------------------

size_t C1541::stateSize() {
    return 2048 + sizeof(uint64_t) + 3 + 2 * sizeof(Via6522::State) + sizeof(Cpu6502<Bus>::Snapshot);
}

void C1541::saveState(uint8_t* out) const {
    auto put = [&out](const void* p, size_t n) { std::memcpy(out, p, n); out += n; };
    put(ram_.data(), ram_.size());
    put(&cycle_, sizeof cycle_);
    const uint8_t lines[3] = {atnLow_, clkLow_, dataLow_};
    put(lines, 3);
    put(&via1_.state(), sizeof(Via6522::State));
    put(&via2_.state(), sizeof(Via6522::State));
    const auto snap = cpu_.snapshot();
    put(&snap, sizeof snap);
}

void C1541::loadState(const uint8_t* in) {
    auto get = [&in](void* p, size_t n) { std::memcpy(p, in, n); in += n; };
    get(ram_.data(), ram_.size());
    get(&cycle_, sizeof cycle_);
    uint8_t lines[3];
    get(lines, 3);
    atnLow_ = lines[0] != 0;
    clkLow_ = lines[1] != 0;
    dataLow_ = lines[2] != 0;
    Via6522::State v;
    get(&v, sizeof v);
    via1_.setState(v);
    get(&v, sizeof v);
    via2_.setState(v);
    Cpu6502<Bus>::Snapshot snap;
    get(&snap, sizeof snap);
    cpu_.restore(snap);
}

void C1541::tick() {
    cpu_.setIrq(via1_.irq() || via2_.irq());
    cpu_.tick();
    via1_.clock();
    via2_.clock();
    ++cycle_;
}

} // namespace k64

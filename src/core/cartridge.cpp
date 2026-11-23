#include "cartridge.h"

namespace k64 {

namespace {

constexpr size_t kBank = 0x2000;

bool powerOfTwo(size_t n) { return n && !(n & (n - 1)); }

// A chip in an 8K window: one smaller than the window does not decode the upper address
// lines and repeats; anything else sits where it is loaded.
void place(uint8_t* window, size_t offset, const uint8_t* data, size_t size) {
    if (powerOfTwo(size) && size <= kBank) {
        for (size_t i = 0; i < kBank; ++i) window[i] = data[i & (size - 1)];
        return;
    }
    for (size_t i = 0; i < size && offset + i < kBank; ++i) window[offset + i] = data[i];
}

// T32's flash chips (the Am29F040B's data sheet; hw/cart.md FL). Times in machine cycles,
// which are close to microseconds: the sheet's typical values.
constexpr uint32_t kProgramCycles = 7;            // one byte
constexpr uint32_t kEraseTimeoutCycles = 50;      // more sectors can be named within it
constexpr uint32_t kSectorEraseCycles = 1000000;  // per sector
constexpr uint32_t kChipEraseCycles = 8000000;
constexpr size_t kSectorSize = 0x10000;
enum FlashSeq : uint8_t { Idle, Unlock1, Unlock2, ProgramByte, EraseSetup, EraseUnlock1, EraseUnlock2 };
enum FlashOp : uint8_t { None, Programming, ProgramFailed, EraseTimeout, SectorErasing, ChipErasing };

// Type 7's register holds the bank number as bits 5-3 (low three) and bit 0 (the fourth).
unsigned scrambledBank(unsigned v) { return ((v >> 3) & 7) | ((v & 1) << 3); }

} // namespace

bool Cartridge::supports(unsigned type) {
    switch (type) {
    case 0: case 1: case 3: case 4: case 5: case 7: case 8: case 9: case 10: case 15: case 17: case 18: case 19: case 21:
    case 32: case 36: case 53: case 61: case 70:
        return true;
    default: return false;
    }
}

void Cartridge::detach() {
    type_ = -1;
    banks_ = 1;
    romL_.assign(kBank, 0xFF);
    romH_.assign(kBank, 0xFF);
    high_ = &romH_;
    ram_.clear();
    laterBoard_ = false;
    exromAtReset_ = gameAtReset_ = 1;
    reset();
}

bool Cartridge::attach(const CartImage& image, std::string& error) {
    detach();
    if (!supports(image.type)) {
        error = "cartridge hardware type " + std::to_string(image.type) + " is not supported yet";
        return false;
    }
    const unsigned type = image.type;
    // The banks the chips name. Type 7's files carry the register value as bank number.
    auto bankOf = [type](const CartImage::Chip& c) { return type == 7 ? scrambledBank(c.bank) : unsigned{c.bank}; };
    // T32: two chips of 512K whatever the file fills. T21: a second socket, empty.
    unsigned banks = type == 32 ? 64 : type == 21 ? 8 : 1;
    for (const auto& c : image.chips) {
        if (c.data.empty()) continue;
        const unsigned b = bankOf(c);
        if (b > 127 || (type == 32 && b > 63)) { error = "cartridge file names bank " + std::to_string(b); return false; }
        if (b + 1 > banks) banks = b + 1;
    }
    unsigned size = 1;
    while (size < banks) size <<= 1;   // the register's unused bits repeat the chips
    std::vector<uint8_t> low(size * kBank, 0xFF), high(size * kBank, 0xFF);
    bool anyHigh = false;
    for (const auto& c : image.chips) {
        if (c.data.empty()) continue;
        const unsigned b = bankOf(c);
        const size_t n = c.data.size();
        const bool inLow = c.load >= 0x8000 && c.load < 0xA000;
        const bool inHigh = (c.load >= 0xA000 && c.load < 0xC000) || c.load >= 0xE000;
        if (!inLow && !inHigh) { error = "cartridge chip at an address no cartridge ROM can be at"; return false; }
        if (inLow && c.load == 0x8000 && n == 2 * kBank) {   // one 16K chip: both windows
            place(&low[b * kBank], 0, c.data.data(), kBank);
            place(&high[b * kBank], 0, c.data.data() + kBank, kBank);
            anyHigh = true;
        } else if (inLow || type == 5) {
            // (Type 5 has one row of chips: its files load the upper half of the banks at
            // $A000, where the software reads them.)
            if (n > kBank) { error = "cartridge chip larger than its window"; return false; }
            place(&low[b * kBank], inLow ? c.load & 0x1FFF : 0, c.data.data(), n);
        } else {
            if (n > kBank) { error = "cartridge chip larger than its window"; return false; }
            place(&high[b * kBank], c.load & 0x1FFF, c.data.data(), n);
            anyHigh = true;
        }
    }
    for (unsigned b = banks; b < size; ++b) {   // mirrors
        for (size_t i = 0; i < kBank; ++i) {
            low[b * kBank + i] = low[(b % banks) * kBank + i];
            high[b * kBank + i] = high[(b % banks) * kBank + i];
        }
    }
    type_ = static_cast<int>(type);
    banks_ = size;
    romL_ = std::move(low);
    romH_ = std::move(high);
    high_ = &romH_;
    // The lines at power on. Only type 0 takes them from the file; the others' hardware
    // decides (hw/cart.md F1).
    switch (type) {
    case 0: exromAtReset_ = image.exrom; gameAtReset_ = image.game; break;
    case 5:
        // One 8K bank in both windows, 16K configuration; the 512K kind is wired for 8K.
        high_ = &romL_;
        exromAtReset_ = 0;
        gameAtReset_ = banks > 32 ? 1 : 0;
        break;
    case 1: case 9: case 36:
        // One row of ROM banks, seen through ROML and through ROMH; 8K of RAM, type 36 32K.
        high_ = &romL_;
        ram_.assign(type == 36 ? 0x8000 : 0x2000, 0);
        laterBoard_ = type == 36 && image.subtype == 1;
        exromAtReset_ = 0;
        gameAtReset_ = 1;
        break;
    case 53: ram_.assign(0x8000, 0); exromAtReset_ = 0; gameAtReset_ = 0; break;
    case 61: case 70: exromAtReset_ = 1; gameAtReset_ = 0; break;   // Ultimax: its own reset vector
    case 3: case 4: case 8: case 18: case 21: exromAtReset_ = 0; gameAtReset_ = 0; break;   // 16K
    case 32: exromAtReset_ = 1; gameAtReset_ = 0; break;   // Ultimax: it starts from its own reset vector
    default: exromAtReset_ = 0; gameAtReset_ = 1; break;   // 7, 10, 15, 17, 19: 8K
    }
    if (type == 18 && !anyHigh) { error = "cartridge file without the chips its hardware type has"; detach(); return false; }
    reset();
    return true;
}

void Cartridge::reset() {
    s_ = State{};
    s_.type = static_cast<int16_t>(type_);
    s_.exrom = exromAtReset_;
    s_.game = gameAtReset_;
    // RAM chips at power on: the same pattern as the computer's (c64.cpp).
    for (size_t i = 0; i < ram_.size(); ++i) ram_[i] = (i & 0x40) ? 0xFF : 0x00;
}

bool Cartridge::setState(const State& s) {
    if (s.type != type_ || s.bank >= banks_ || s.bankH >= banks_ || s.writes > 7 || s.select > 7) return false;
    for (const Flash& f : s.flash)
        if (f.seq > EraseUnlock2 || f.op > ChipErasing || (type_ != 32 && (f.op || f.autoselect || f.suspended))) return false;
    s_ = s;
    return true;
}

void Cartridge::setBank(unsigned bank) {
    s_.bank = static_cast<uint8_t>(bank & (banks_ - 1));
    if (type_ != 18) s_.bankH = s_.bank;
}

uint8_t Cartridge::readRomL(uint16_t addr, uint8_t open, uint8_t ram) {
    if (type_ == 18) s_.bankH = (addr & 0x1000) ? 1 : 0;   // T18: which half was read picks ROMH's bank
    if (type_ == 10) s_.capacitor = 0;                      // T10
    if (flashBusy(0)) return flashValue(0, addr, s_.flash[0].toggle);   // a read moves the toggle bits
    if (type_ == 53 && s_.select >= 4) return selectedRam(addr, open);
    return peekRomL(addr, open, ram);
}

// T53 with its RAM or nothing chosen: the half of the 32K the register names, in both
// windows; or the bus's last byte.
uint8_t Cartridge::selectedRam(uint16_t addr, uint8_t open) const {
    if (s_.select >= 6) return open;
    return ram_[(size_t{s_.select} & 1) << 14 | (addr & 0x3FFF)];
}

uint8_t Cartridge::readRomH(uint16_t addr, uint8_t open) {
    if (flashBusy(1)) return flashValue(1, addr, s_.flash[1].toggle);
    if (type_ == 53 && s_.select >= 4) return selectedRam(addr, open);
    return peekRomH(addr);
}

// ---- T1, T9, T36: the freezers ----------------------------------------------------------

// What ROML's window shows.
uint8_t Cartridge::freezerRomL(uint16_t addr, uint8_t open, uint8_t ram) const {
    const size_t rom = (size_t{s_.bank} << 13) | (addr & 0x1FFF);
    if (type_ != 36) {
        // T1 with its RAM on and both lines let go: the cartridge's RAM and the computer's
        // answer together.
        if (s_.contend) return ram | ram_[addr & 0x1FFF];
        return s_.ramOn ? ram_[addr & 0x1FFF] : romL_[rom];
    }
    if (s_.frozen) return open;                       // nothing of the cartridge's is here
    if (s_.ramOn) return ram_[ramAt(addr, true)];
    if (laterBoard_ && s_.ramHigh) return romL_[rom];
    if (!s_.game && !s_.exrom) return open;           // 16K configuration without RAM: nothing
    return romL_[rom];
}

// What ROMH's window shows: the same row of ROM banks, or the RAM.
uint8_t Cartridge::freezerRomH(uint16_t addr) const {
    const size_t rom = (size_t{s_.bank} << 13) | (addr & 0x1FFF);
    if (type_ == 1) return romL_[rom];
    if (type_ == 9) return s_.ramHigh ? ram_[addr & 0x1FFF] : romL_[rom];
    // T36: with its RAM chosen and banking of the I/O window not allowed, the two low
    // bank bits do not reach the ROM here.
    const size_t rom4 = ((size_t{s_.bank} & ~size_t{3}) << 13) | (addr & 0x1FFF);
    // (The later board's RAM in this window, $A000-$BFFF, never comes this way: claims().)
    return romL_[(s_.allowBank || !(s_.ramOn || (laterBoard_ && s_.ramHigh))) ? rom : rom4];
}

// T1 and T9: the one register, anywhere in I/O 1.
//   bit 0  1 = GAME pulled low        bit 3-4  ROM bank
//   bit 1  1 = EXROM let go           bit 5    1 = RAM in place of ROM
//   bit 2  1 = the register is off    bit 6    1 = the freeze is over
void Cartridge::freezerRegister(uint8_t value) {
    setBank((value >> 3) & 3);
    s_.contend = 0;
    if (type_ == 9 && (value & 0xE7) == 0x22) {
        // T9's extra mode: ROM in ROML's window, the RAM in ROMH's, 16K configuration.
        s_.game = 0;
        s_.exrom = 0;
        s_.ramHigh = 1;
        s_.ramOn = 0;
    } else {
        s_.game = (value & 1) ? 0 : 1;
        s_.exrom = (value & 2) ? 1 : 0;
        s_.ramHigh = 0;
        s_.ramOn = (value & 0x20) ? 1 : 0;
        if (value & 0x40) s_.frozen = 0;
        // T1 with the same value: its RAM answers at $8000 although the computer's does too.
        if (type_ == 1 && (value & 0x23) == 0x22) {
            s_.contend = 1;
            s_.exrom = 0;
        }
    }
    // T1 holds the lines through the whole cycle, T9 in the CPU's half only.
    s_.vicUltimax = type_ == 1 && ultimax();
    if (value & 4) s_.active = 0;
}

// The CPU has saved its return address and flags: the next two cycles read the vector.
// The cartridge's ROM goes where the vectors are, and the registers answer again.
void Cartridge::enterFreeze() {
    s_.pending = 0;
    s_.writes = 0;
    s_.frozen = 1;
    s_.active = 1;
    s_.game = 0;
    s_.exrom = 1;
    s_.vicUltimax = 1;
    s_.contend = 0;
    s_.ramOn = type_ == 36 ? 0 : 1;
    setBank(0);
}

void Cartridge::setFreezeButton(bool down) {
    if (type_ == 3) {
        // T3: NMI for as long as the button is held; GAME seven cycles after it went
        // down (clock()), until it is let go.
        if (!down) s_.forceGame = 0;
        if (down && !s_.button) s_.writes = 0;
        s_.button = down ? 1 : 0;
        return;
    }
    if (!freezer()) return;
    // Letting go of it starts the freeze, unless one is in progress or type 36's second
    // register has switched the function off (the button itself can still be read).
    if (s_.button && !down && !s_.frozen && !s_.noFreeze) {
        s_.pending = 1;
        s_.writes = 0;
    }
    s_.button = down ? 1 : 0;
}

void Cartridge::writeRomL(uint16_t addr, uint8_t value) {
    if (type_ == 32) flashWrite(0, addr, value);
    else if (freezer() && s_.ramOn) ram_[type_ == 36 ? ramAt(addr, true) : addr & 0x1FFF] = value;
}

bool Cartridge::writeMem(uint16_t addr, uint8_t value, bool romhForReads) {
    if (type_ == 53) {
        // With the RAM chosen it takes every write to its 16K, and the computer's RAM too.
        if ((s_.select & 6) == 4) ram_[(size_t{s_.select} & 1) << 14 | (addr & 0x3FFF)] = value;
        return false;
    }
    if (addr < 0xA000) {
        // The RAM of T1, T9 and the later T36 board takes writes whatever the PLA selects;
        // the computer's RAM takes them too.
        if (s_.ramOn && (type_ != 36 || laterBoard_)) ram_[type_ == 36 ? ramAt(addr, true) : addr & 0x1FFF] = value;
        return false;
    }
    if (!s_.ramHigh) return false;
    // The RAM in ROMH's window takes the write alone.
    if (type_ == 9 && romhForReads) {
        ram_[addr & 0x1FFF] = value;
        return true;
    }
    return false;
}

// T36: the window into the selected bank's page $1E (I/O 1 from $DE02, when the second
// register says so) or $1F (I/O 2): RAM when it is chosen, else ROM.
bool Cartridge::ioWindow(uint16_t addr, bool write, uint8_t& value) {
    if (s_.frozen) return false;
    const auto offset = static_cast<uint16_t>(0x1E00 | (addr & 0x01FF));
    if (write) {
        if (s_.ramOn || (laterBoard_ && s_.ramHigh)) ram_[ramAt(offset, s_.allowBank)] = value;
        return true;
    }
    if (s_.ramOn || s_.ramHigh) {
        value = ram_[ramAt(offset, s_.allowBank)];
        return true;
    }
    if (!s_.game) return false;   // 16K and Ultimax configuration: only the RAM shows
    value = romL_[(size_t{s_.bank} << 13) | offset];
    return true;
}

// ---- T32: the flash chips ---------------------------------------------------------------

// What a read of a chip gives while it is not simply showing its contents; `toggle` holds
// the toggle bits and is moved as a read moves them.
uint8_t Cartridge::flashValue(int chip, uint16_t addr, uint8_t& toggle) const {
    const Flash& f = s_.flash[chip];
    const std::vector<uint8_t>& rom = chip ? romH_ : romL_;
    const size_t a = (size_t{s_.bank} << 13) | (addr & 0x1FFF);
    const bool chosen = f.op == ChipErasing || ((f.sectors >> (a / kSectorSize)) & 1);
    switch (f.op) {
    case Programming: case ProgramFailed:
        // DQ7 the complement of the bit being programmed, DQ6 toggles; DQ5 once it failed.
        toggle ^= 0x40;
        return static_cast<uint8_t>((~f.data & 0x80) | (toggle & 0x40) | (f.op == ProgramFailed ? 0x20 : 0));
    case EraseTimeout: case SectorErasing: case ChipErasing:
        // DQ7 0, DQ6 toggles, DQ2 toggles in a sector being erased, DQ3 once the time-out
        // for naming more sectors is over.
        toggle ^= chosen ? 0x44 : 0x40;
        return static_cast<uint8_t>((toggle & (chosen ? 0x44 : 0x40)) | (f.op != EraseTimeout ? 0x08 : 0));
    default: break;
    }
    if (f.autoselect) {
        switch (a & 0xFF) {
        case 0: return 0x01;   // manufacturer
        case 1: return 0xA4;   // device
        case 2: return 0x00;   // the sector is not protected
        default: return rom[a];
        }
    }
    if (f.suspended && chosen) {   // a sector whose erase is on hold: DQ7 1, DQ2 toggles
        toggle ^= 0x04;
        return static_cast<uint8_t>(0x80 | (toggle & 0x04));
    }
    return rom[a];
}

void Cartridge::flashWrite(int chip, uint16_t addr, uint8_t value) {
    Flash& f = s_.flash[chip];
    std::vector<uint8_t>& rom = chip ? romH_ : romL_;
    const size_t a = (size_t{s_.bank} << 13) | (addr & 0x1FFF);
    const unsigned key = a & 0x7FF;   // the upper address lines do not matter in a command
    const uint8_t sector = static_cast<uint8_t>(1u << (a / kSectorSize));
    switch (f.op) {
    case Programming: case ChipErasing:
        return;   // nothing is accepted until it has finished
    case ProgramFailed:
        if (value == 0xF0) { f.op = None; f.failed = 0; f.seq = Idle; }   // only a reset gets out
        return;
    case SectorErasing:
        if (value == 0xB0) {   // on hold; the time left is kept (a byte programmed meanwhile has its own)
            f.op = None;
            f.suspended = 1;
            f.held = f.timer;
        }
        return;
    case EraseTimeout:
        if (value == 0x30) {            // one more sector; the time-out starts again
            f.sectors |= sector;
            f.timer = kEraseTimeoutCycles;
        } else if (value == 0xB0) {     // on hold before it began
            f.op = None;
            f.suspended = 1;
            f.held = kSectorEraseCycles;
        } else {                        // anything else calls the erase off
            f.op = None;
            f.sectors = 0;
            f.timer = 0;
            f.seq = Idle;
        }
        return;
    default: break;
    }
    const FlashSeq seq = static_cast<FlashSeq>(f.seq);
    f.seq = Idle;
    if (value == 0xF0 && seq != ProgramByte) { f.autoselect = 0; return; }   // reset: back to the contents
    switch (seq) {
    case Idle:
        if (key == 0x555 && value == 0xAA) f.seq = Unlock1;
        else if (value == 0x30 && f.suspended) {   // resume
            f.suspended = 0;
            f.op = SectorErasing;
            f.timer = f.held;
        } else f.autoselect = 0;   // not the start of a command: back to the contents
        break;
    case Unlock1:
        if (key == 0x2AA && value == 0x55) f.seq = Unlock2;
        else f.autoselect = 0;
        break;
    case Unlock2:
        if (key != 0x555) { f.autoselect = 0; break; }
        if (value == 0x90) f.autoselect = 1;
        else if (value == 0xA0) f.seq = ProgramByte;
        else if (value == 0x80 && !f.suspended) f.seq = EraseSetup;
        else f.autoselect = 0;
        break;
    case ProgramByte:
        // Not into a sector whose erase is on hold. Bits only go from 1 to 0; asking for
        // a 0 back to 1 fails (DQ5) and leaves the 0.
        if (f.suspended && (f.sectors & sector)) break;
        f.data = value;
        f.failed = (rom[a] & value) != value;
        rom[a] &= value;
        f.op = Programming;
        f.timer = kProgramCycles;
        break;
    case EraseSetup:
        if (key == 0x555 && value == 0xAA) f.seq = EraseUnlock1;
        break;
    case EraseUnlock1:
        if (key == 0x2AA && value == 0x55) f.seq = EraseUnlock2;
        break;
    case EraseUnlock2:
        if (key == 0x555 && value == 0x10) {
            f.op = ChipErasing;
            f.timer = kChipEraseCycles;
        } else if (value == 0x30) {
            f.op = EraseTimeout;
            f.sectors = sector;
            f.timer = kEraseTimeoutCycles;
        }
        break;
    }
}

void Cartridge::clockFlash(int chip) {
    Flash& f = s_.flash[chip];
    if (f.op == None || f.op == ProgramFailed || f.timer == 0 || --f.timer) return;
    std::vector<uint8_t>& rom = chip ? romH_ : romL_;
    switch (f.op) {
    case Programming:
        f.op = f.failed ? ProgramFailed : None;
        break;
    case EraseTimeout:
        f.op = SectorErasing;
        f.timer = kSectorEraseCycles;
        break;
    case SectorErasing:   // one sector after the other
        for (unsigned s = 0; s < 8; ++s) {
            if (!((f.sectors >> s) & 1)) continue;
            for (size_t i = 0; i < kSectorSize; ++i) rom[s * kSectorSize + i] = 0xFF;
            f.sectors = static_cast<uint8_t>(f.sectors & ~(1u << s));
            break;
        }
        if (f.sectors) f.timer = kSectorEraseCycles;
        else f.op = None;
        break;
    case ChipErasing:
        for (uint8_t& b : rom) b = 0xFF;
        f.op = None;
        break;
    default: break;
    }
}

void Cartridge::saveContent(uint8_t* out) const {
    for (size_t i = 0; i < ram_.size(); ++i) out[i] = ram_[i];
    if (type_ != 32) return;
    for (size_t i = 0; i < romL_.size(); ++i) out[i] = romL_[i];
    for (size_t i = 0; i < romH_.size(); ++i) out[romL_.size() + i] = romH_[i];
}

void Cartridge::loadContent(const uint8_t* in) {
    for (size_t i = 0; i < ram_.size(); ++i) ram_[i] = in[i];
    if (type_ != 32) return;
    for (size_t i = 0; i < romL_.size(); ++i) romL_[i] = in[i];
    for (size_t i = 0; i < romH_.size(); ++i) romH_[i] = in[romL_.size() + i];
}

bool Cartridge::readIo(uint16_t addr, uint8_t& value) {
    const bool io1 = !(addr & 0x0100);
    switch (type_) {
    case 1: case 9:
        if (!s_.active) return false;
        // The register has no read/write input: a read stores what the bus holds.
        if (io1) {
            freezerRegister(value);
            return false;
        }
        // I/O 2: the last page of the RAM or of the selected ROM bank.
        value = (s_.ramOn || s_.ramHigh) ? ram_[0x1F00 | (addr & 0xFF)] : romL_[(size_t{s_.bank} << 13) | 0x1F00 | (addr & 0xFF)];
        return true;
    case 36:
        if (!s_.active) return false;
        if (io1 && (addr & 0xFF) < 2) {
            // The status: bank bits 13, 14, 15 in bits 3, 4, 7; the window's banking
            // allowed (1); the button held (2); the window in I/O 1 (6).
            value = static_cast<uint8_t>(((s_.bank & 3) << 3) | ((s_.bank & 4) << 5) | (s_.allowBank ? 0x02 : 0) |
                                         (s_.button ? 0x04 : 0) | (s_.reuMap ? 0x40 : 0));
            return true;
        }
        if (io1 && s_.clockPort && (addr & 0xFF) < 0x10) return false;   // the accessory connector: nothing on it
        if (io1 != (s_.reuMap != 0)) return false;
        return ioWindow(addr, false, value);
    case 3:    // T3: the last two pages of the bank's first half
        value = romL_[(size_t{s_.bank} << 13) | 0x1E00 | (addr & 0x01FF)];
        return true;
    case 4:    // T4: a read in I/O 1 takes the second chip out
        if (io1) s_.game = 1;
        return false;
    case 10:   // T10: I/O 1 empties the capacitor; I/O 2 shows the ROM's last page
        if (io1) {
            s_.capacitor = 0;
            s_.exrom = 0;
            return false;
        }
        value = romL_[0x1F00 | (addr & 0xFF)];
        return true;
    case 15:   // T15: a read in I/O 1 goes back to the first bank
        if (io1) setBank(0);
        return false;
    case 17:   // T17: the address read in I/O 1 is the bank
        if (io1) setBank(addr & 0x0F);
        return false;
    case 32:   // T32: 256 bytes of RAM in I/O 2
        if (io1) return false;
        value = s_.ram[addr & 0xFF];
        return true;
    default:
        return false;
    }
}

void Cartridge::writeIo(uint16_t addr, uint8_t value) {
    const bool io1 = !(addr & 0x0100);
    switch (type_) {
    case 1: case 9:
        if (!s_.active) break;
        if (io1) freezerRegister(value);
        else if (s_.ramOn || s_.ramHigh) ram_[0x1F00 | (addr & 0xFF)] = value;
        break;
    case 36:
        if (!s_.active) break;
        if (io1 && (addr & 0xFF) == 0) {
            // The first register: bits as T1's, bit 7 a third bank bit.
            setBank(((value >> 3) & 3) | ((value >> 5) & 4));
            s_.game = (value & 1) ? 0 : 1;
            s_.exrom = (value & 2) ? 1 : 0;
            if (laterBoard_ && (value & 0x67) == 0x22) {
                // The later board has T9's extra mode: ROM in ROML's window, RAM in ROMH's.
                s_.game = 0;
                s_.exrom = 0;
                s_.ramHigh = 1;
                s_.ramOn = 0;
            } else {
                s_.ramHigh = 0;
                if (value & 0x40) s_.frozen = 0;
                s_.ramOn = (value & 0x20) ? 1 : 0;
                // The first board with the same value: both lines let go, the RAM in the I/O window.
                if ((value & 0x67) == 0x22) s_.ramHigh = 1;
            }
            // Until the freeze is over, bits 0 and 1 do not reach the lines.
            if (s_.frozen) {
                s_.game = 0;
                s_.exrom = 1;
            }
            s_.vicUltimax = 0;
            // Bit 2: the registers take no more writes, and the memory map is the
            // computer's own, as with nothing plugged in.
            if (value & 4) {
                s_.active = 0;
                if (!s_.frozen) s_.game = s_.exrom = 1;
            }
        } else if (io1 && (addr & 0xFF) == 1) {
            // The second register: bits 1, 2 and 6 take the first write only.
            if (!s_.written) {
                s_.allowBank = (value & 0x02) ? 1 : 0;
                s_.noFreeze = (value & 0x04) ? 1 : 0;
                s_.reuMap = (value & 0x40) ? 1 : 0;
                s_.written = 1;
            }
            setBank(((value >> 3) & 3) | ((value >> 5) & 4));
            s_.clockPort = value & 1;
        } else if (!(io1 && s_.clockPort && (addr & 0xFF) < 0x10) && io1 == (s_.reuMap != 0)) {
            ioWindow(addr, true, value);
        }
        break;
    case 3:    // T3: the register at $DFFF
        if (addr == 0xDFFF && !s_.locked) {
            setBank(value & 3);
            s_.exrom = (value & 0x10) ? 1 : 0;
            s_.game = (value & 0x20) ? 1 : 0;
            s_.pending = (value & 0x40) ? 0 : 1;   // bit 6 clear: the NMI line pulled
            s_.locked = (value & 0x80) ? 1 : 0;
        }
        break;
    case 21:   // T21: bank, and the configuration in the top three bits
        if (io1) {
            setBank(value & 7);
            const unsigned top = value & 0xE0;
            s_.exrom = top == 0xE0 ? 1 : 0;
            s_.game = (top == 0xE0 || top == 0x40) ? 1 : 0;
        }
        break;
    case 53:   // T53: in the upper half of I/O 1: half, chip, off
        if (io1 && (addr & 0x80)) {
            s_.select = (value >> 1) & 7;
            setBank(s_.select & 3);
            s_.exrom = s_.game = (value & 0x10) ? 1 : 0;
        }
        break;
    case 70:   // T70: the address written in I/O 1: bank, and bit 7 no more changes
        if (io1 && !s_.locked) {
            setBank(addr & 0x3F);
            s_.locked = (addr & 0x80) ? 1 : 0;
        }
        break;
    case 4:    // T4: a write in I/O 1 puts the second chip in
        if (io1) s_.game = 0;
        break;
    case 5:    // T5: bits 0-5
        if (io1) setBank(value & 0x3F);
        break;
    case 7:    // T7: scrambled bank bits; $86 in bits 7, 6, 2, 1 switches the cartridge off
        if (io1) {
            setBank(scrambledBank(value));
            if ((value & 0xC6) == 0x00) s_.exrom = 0;
            else if ((value & 0xC6) == 0x86) s_.exrom = 1;
        }
        break;
    case 8:    // T8: in I/O 2; bit 2 off, bit 3 no more changes
        if (!io1 && !s_.locked) {
            setBank(value & 3);
            s_.exrom = s_.game = (value & 4) ? 1 : 0;
            s_.locked = (value & 8) ? 1 : 0;
        }
        break;
    case 15:   // T15: the address written in I/O 1 is the bank
        if (io1) setBank(addr & 0x3F);
        break;
    case 32:   // T32: bank register at $DE00, control register at $DE02; RAM in I/O 2
        if (!io1) {
            s_.ram[addr & 0xFF] = value;
        } else if (!(addr & 2)) {
            setBank(value & 0x3F);
        } else {
            // Bit 1 pulls EXROM low. GAME is low unless bit 2 hands it to bit 0 (1 = low).
            s_.exrom = (value & 2) ? 0 : 1;
            s_.game = (value & 4) ? ((value & 1) ? 0 : 1) : 0;
        }
        break;
    case 19:   // T19: bits 0-6 the bank, bit 7 the EXROM line
        if (io1) {
            setBank(value & 0x7F);
            s_.exrom = (value & 0x80) ? 1 : 0;
        }
        break;
    default:
        break;
    }
}

// T10: 512 cycles without a read of ROML or I/O 1 and the capacitor has charged: the
// cartridge lets go of the EXROM line. Such a read empties it and pulls the line again.
void Cartridge::clock() {
    if (type_ == 3) {
        // The button's counter: after seven cycles GAME is pulled, and a hidden register
        // takes writes again.
        if (s_.button && !s_.forceGame && ++s_.writes == 7) {
            s_.forceGame = 1;
            s_.locked = 0;
        }
        return;
    }
    if (type_ == 32) {
        clockFlash(0);
        clockFlash(1);
        return;
    }
    if (type_ != 10) return;
    if (s_.capacitor == 0) s_.exrom = 0;
    if (s_.capacitor < 512 && ++s_.capacitor == 512) s_.exrom = 1;
}

} // namespace k64

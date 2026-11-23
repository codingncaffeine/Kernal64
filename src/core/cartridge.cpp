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

// Type 7's register holds the bank number as bits 5-3 (low three) and bit 0 (the fourth).
unsigned scrambledBank(unsigned v) { return ((v >> 3) & 7) | ((v & 1) << 3); }

} // namespace

bool Cartridge::supports(unsigned type) {
    switch (type) {
    case 0: case 4: case 5: case 7: case 8: case 10: case 15: case 17: case 18: case 19: case 32: return true;
    default: return false;
    }
}

void Cartridge::detach() {
    type_ = -1;
    banks_ = 1;
    romL_.assign(kBank, 0xFF);
    romH_.assign(kBank, 0xFF);
    high_ = &romH_;
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
    unsigned banks = 1;
    for (const auto& c : image.chips) {
        if (c.data.empty()) continue;
        const unsigned b = bankOf(c);
        if (b > 127) { error = "cartridge file names bank " + std::to_string(b); return false; }
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
    case 4: case 8: case 18: exromAtReset_ = 0; gameAtReset_ = 0; break;   // 16K
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
}

bool Cartridge::setState(const State& s) {
    if (s.type != type_ || s.bank >= banks_ || s.bankH >= banks_) return false;
    s_ = s;
    return true;
}

void Cartridge::setBank(unsigned bank) {
    s_.bank = static_cast<uint8_t>(bank & (banks_ - 1));
    if (type_ != 18) s_.bankH = s_.bank;
}

uint8_t Cartridge::readRomL(uint16_t addr) {
    if (type_ == 18) s_.bankH = (addr & 0x1000) ? 1 : 0;   // T18: which half was read picks ROMH's bank
    if (type_ == 10) s_.capacitor = 0;                      // T10
    return peekRomL(addr);
}

uint8_t Cartridge::readRomH(uint16_t addr) { return peekRomH(addr); }

bool Cartridge::readIo(uint16_t addr, uint8_t& value) {
    const bool io1 = !(addr & 0x0100);
    switch (type_) {
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
    if (type_ != 10) return;
    if (s_.capacitor == 0) s_.exrom = 0;
    if (s_.capacitor < 512 && ++s_.capacitor == 512) s_.exrom = 1;
}

} // namespace k64

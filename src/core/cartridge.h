// What is plugged into the expansion port: the ROM chips of a cartridge and the logic
// that switches them, by hardware type.
//
// The machine asks it for the level of the port's two configuration lines (GAME and
// EXROM, into the PLA) and passes on the accesses the PLA selects it for: ROML, ROMH and
// the two I/O areas ($DE00-$DEFF, $DF00-$DFFF). Every rule and its source:
// local-notes/hw/cart.md (P = the port, T<n> = hardware type n of the file format).
// Cartridges with RAM of their own see more than the PLA's selects: every write to
// $8000-$BFFF, and what the bus holds when nobody drives it. The ones with a freeze button
// also pull the NMI and IRQ lines and watch the CPU's read/write line (hw/cart.md Z).
#pragma once

#include "crt.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace k64 {

class Cartridge {
public:
    // Takes a parsed file. False, with the reason in `error`, when its hardware type is
    // not built or its chips do not fit the type.
    bool attach(const CartImage& image, std::string& error);
    void detach();
    bool attached() const { return type_ >= 0; }
    int type() const { return type_; }
    // Whether the hardware type is one attach() accepts.
    static bool supports(unsigned type);

    void reset();   // the port's RESET line: the switching logic as at power on

    // The lines' levels: true = high (not pulled). No cartridge: both high.
    bool game() const { return s_.game != 0 && !s_.forceGame; }
    bool exrom() const { return s_.exrom != 0; }
    // The same two lines in the VIC-II's half of the cycle: the freezer types pull them
    // for the CPU's half only, except right after a freeze.
    bool gameVic() const { return freezer() ? !s_.vicUltimax : s_.game != 0; }
    bool exromVic() const { return freezer() ? true : s_.exrom != 0; }

    // Accesses of the CPU the PLA selects the cartridge for. On some types a read switches
    // something. `open` is what the data bus holds when nothing drives it (returned when
    // the cartridge does not answer), `ram` what the computer's RAM holds at the address
    // (one type lets both drive the bus).
    uint8_t readRomL(uint16_t addr, uint8_t open, uint8_t ram);
    uint8_t readRomH(uint16_t addr, uint8_t open);
    // False when the cartridge does not drive the data bus for that address. `value` comes
    // in as what the bus holds (a register without a read/write input takes it).
    bool readIo(uint16_t addr, uint8_t& value);   // $DE00-$DFFF
    void writeIo(uint16_t addr, uint8_t value);
    // Writes the PLA selects the cartridge's chips for (it does so in the Ultimax
    // configuration only): type 32's flash chips take their commands this way, the
    // freezers' RAM its bytes.
    void writeRomL(uint16_t addr, uint8_t value);
    void writeRomH(uint16_t addr, uint8_t value) { if (type_ == 32) flashWrite(1, addr, value); }
    // The same bytes without any effect (the VIC-II's reads, the tools).
    uint8_t peekRomL(uint16_t addr, uint8_t open, uint8_t ram) const {
        if (freezer()) return freezerRomL(addr, open, ram);
        if (flashBusy(0)) { uint8_t toggle = s_.flash[0].toggle; return flashValue(0, addr, toggle); }
        return romL_[(size_t{s_.bank} << 13) | (addr & 0x1FFF)];
    }
    uint8_t peekRomH(uint16_t addr) const {
        if (freezer()) return freezerRomH(addr);
        if (flashBusy(1)) { uint8_t toggle = s_.flash[1].toggle; return flashValue(1, addr, toggle); }
        return (*high_)[(size_t{s_.bankH} << 13) | (addr & 0x1FFF)];
    }

    // What a cartridge with RAM of its own decodes from the address bus itself.
    bool seesBus() const { return freezer() || type_ == 53; }
    // A write to $8000-$BFFF the PLA did not select the cartridge for. `romhForReads`: a
    // read of the address would get ROMH. True when the computer's RAM must not take it.
    bool writeMem(uint16_t addr, uint8_t value, bool romhForReads);
    // An address at which the cartridge's RAM answers whatever the PLA would select (it
    // keeps the computer's own memory off the bus there): the later T36 board with its RAM
    // in ROMH's window, $A000-$BFFF.
    bool claims(uint16_t addr) const { return laterBoard_ && s_.ramHigh && (addr & 0xE000) == 0xA000; }
    uint8_t readClaimed(uint16_t addr) const { return ram_[ramAt(addr, s_.allowBank != 0)]; }
    void writeClaimed(uint16_t addr, uint8_t value) { ram_[ramAt(addr, s_.allowBank != 0)] = value; }

    // The freeze button (hw/cart.md Z). Letting go of it starts a freeze: the cartridge
    // pulls NMI and IRQ, waits for three write cycles in a row (the CPU saving its return
    // address and flags), then puts its ROM where the interrupt vectors are read from.
    void setFreezeButton(bool down);
    // (Type 3 pulls NMI alone: while its button is held, or through its register.)
    bool nmi() const { return s_.pending != 0 || (type_ == 3 && s_.button); }
    bool irq() const { return s_.pending != 0 && type_ != 3; }
    bool watchesCycles() const { return s_.pending != 0 && type_ != 3; }
    void cpuCycle(bool write) {
        if (!write) s_.writes = 0;
        else if (++s_.writes == 3) enterFreeze();
    }

    // True while something runs on the clock; then clock() once per cycle.
    bool clocked() const { return type_ == 10 || s_.flash[0].op || s_.flash[1].op || (type_ == 3 && s_.button && !s_.forceGame); }
    void clock();

    // Chips that can be written are part of a save state beyond State: their contents.
    size_t contentSize() const { return type_ == 32 ? romL_.size() + romH_.size() : ram_.size(); }
    void saveContent(uint8_t* out) const;
    void loadContent(const uint8_t* in);

#pragma pack(push, 1)
    // T32: one flash chip's command logic (hw/cart.md FL).
    struct Flash {
        uint8_t seq = 0;           // how far a command sequence has come
        uint8_t op = 0;            // the operation in progress
        uint8_t autoselect = 0;    // reads give the chip's identifier codes
        uint8_t suspended = 0;     // a sector erase is on hold
        uint8_t sectors = 0;       // the sectors chosen for erasing, one bit each
        uint8_t toggle = 0;        // the toggle bits (DQ6, DQ2) as last read
        uint8_t data = 0;          // the byte being programmed
        uint8_t failed = 0;        // the programming tried to set a cleared bit
        uint32_t timer = 0;        // cycles left of the operation or the time-out
        uint32_t held = 0;         // cycles left of an erase on hold
    };
    struct State {
        int16_t type = -1;         // the hardware type the state belongs to
        uint8_t game = 1, exrom = 1;
        uint8_t bank = 0;          // the bank ROML shows
        uint8_t bankH = 0;         // the bank ROMH shows
        uint8_t off = 0;           // switched off until the next reset
        uint8_t locked = 0;        // the register takes no more writes until the next reset
        uint16_t capacitor = 0;    // type 10: cycles since the last access that emptied it
        uint8_t ram[256] = {};     // type 32: the RAM in the second I/O area
        Flash flash[2];            // type 32: the chip behind ROML, the chip behind ROMH
        // Types 1, 9, 36 (hw/cart.md Z, T1, T9, T36).
        uint8_t active = 1;        // the registers answer
        uint8_t ramOn = 0;         // the cartridge's RAM in place of its ROM
        uint8_t ramHigh = 0;       // the RAM where ROMH is ("register value $22")
        uint8_t contend = 0;       // type 1: cartridge RAM and computer RAM both answer at $8000
        uint8_t vicUltimax = 0;    // the VIC-II's half of the cycle sees the Ultimax lines too
        uint8_t pending = 0;       // the button was let go of: NMI and IRQ pulled, waiting for the CPU
        uint8_t writes = 0;        // write cycles in a row while pending
        uint8_t frozen = 0;        // the freeze map is in; ends with bit 6 of the register
        uint8_t button = 0;        // the button is held down
        // Type 36's second register.
        uint8_t written = 0;       // its write-once bits have been written
        uint8_t allowBank = 0;     // the bank bits also choose the RAM bank in the I/O window
        uint8_t noFreeze = 0;      // the button does nothing
        uint8_t reuMap = 0;        // the window is in I/O 1 ($DE02-$DEFF), I/O 2 stays free
        uint8_t clockPort = 0;     // $DE02-$DE0F belong to the accessory connector
        // Type 3: GAME pulled by the button's counter. Type 53: the register's chip and half.
        uint8_t forceGame = 0;
        uint8_t select = 0;
    };
#pragma pack(pop)
    const State& state() const { return s_; }
    // False when the state belongs to another hardware type than the one attached.
    bool setState(const State& s);

private:
    void setBank(unsigned bank);
    bool freezer() const { return type_ == 1 || type_ == 9 || type_ == 36; }
    uint8_t freezerRomL(uint16_t addr, uint8_t open, uint8_t ram) const;
    uint8_t freezerRomH(uint16_t addr) const;
    void freezerRegister(uint8_t value);
    void enterFreeze();
    bool ultimax() const { return !s_.game && s_.exrom; }
    // Type 36: where in its RAM an address is, by the bank bits or always in the first 8K.
    size_t ramAt(uint16_t offset, bool banked) const { return (banked ? size_t{s_.bank} & 3 : 0) << 13 | (offset & 0x1FFF); }
    bool ioWindow(uint16_t addr, bool write, uint8_t& value);
    uint8_t selectedRam(uint16_t addr, uint8_t open) const;
    bool flashBusy(int chip) const { const Flash& f = s_.flash[chip]; return f.op || f.autoselect || f.suspended; }
    uint8_t flashValue(int chip, uint16_t addr, uint8_t& toggle) const;
    void flashWrite(int chip, uint16_t addr, uint8_t value);
    void clockFlash(int chip);

    int type_ = -1;
    unsigned banks_ = 1;
    uint8_t exromAtReset_ = 1, gameAtReset_ = 1;
    std::vector<uint8_t> romL_ = std::vector<uint8_t>(0x2000, 0xFF);   // banks of 8K
    std::vector<uint8_t> romH_ = std::vector<uint8_t>(0x2000, 0xFF);
    const std::vector<uint8_t>* high_ = &romH_;   // what ROMH shows: its own chips, or ROML's
    std::vector<uint8_t> ram_;   // a cartridge's RAM: 8K (types 1, 9), 32K (types 36, 53)
    bool laterBoard_ = false;        // type 36, subtype 1: the board with the "$22" RAM mode
    State s_;
};

} // namespace k64

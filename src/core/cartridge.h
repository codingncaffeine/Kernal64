// What is plugged into the expansion port: the ROM chips of a cartridge and the logic
// that switches them, by hardware type.
//
// The machine asks it for the level of the port's two configuration lines (GAME and
// EXROM, into the PLA) and passes on the accesses the PLA selects it for: ROML, ROMH and
// the two I/O areas ($DE00-$DEFF, $DF00-$DFFF). Every rule and its source:
// local-notes/hw/cart.md (P = the port, T<n> = hardware type n of the file format).
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
    bool game() const { return s_.game != 0; }
    bool exrom() const { return s_.exrom != 0; }

    // Accesses of the CPU. On some types a read switches something.
    uint8_t readRomL(uint16_t addr);
    uint8_t readRomH(uint16_t addr);
    // False when the cartridge does not drive the data bus for that address.
    bool readIo(uint16_t addr, uint8_t& value);   // $DE00-$DFFF
    void writeIo(uint16_t addr, uint8_t value);
    // Writes the PLA selects the cartridge's chips for (it does so in the Ultimax
    // configuration only): type 32's flash chips take their commands this way.
    void writeRomL(uint16_t addr, uint8_t value) { if (type_ == 32) flashWrite(0, addr, value); }
    void writeRomH(uint16_t addr, uint8_t value) { if (type_ == 32) flashWrite(1, addr, value); }
    // The same bytes without any effect (the VIC-II's reads, the tools).
    uint8_t peekRomL(uint16_t addr) const {
        if (flashBusy(0)) { uint8_t toggle = s_.flash[0].toggle; return flashValue(0, addr, toggle); }
        return romL_[(size_t{s_.bank} << 13) | (addr & 0x1FFF)];
    }
    uint8_t peekRomH(uint16_t addr) const {
        if (flashBusy(1)) { uint8_t toggle = s_.flash[1].toggle; return flashValue(1, addr, toggle); }
        return (*high_)[(size_t{s_.bankH} << 13) | (addr & 0x1FFF)];
    }

    // True while something runs on the clock; then clock() once per cycle.
    bool clocked() const { return type_ == 10 || s_.flash[0].op || s_.flash[1].op; }
    void clock();

    // Chips that can be written are part of a save state beyond State: their contents.
    size_t contentSize() const { return type_ == 32 ? romL_.size() + romH_.size() : 0; }
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
    };
#pragma pack(pop)
    const State& state() const { return s_; }
    // False when the state belongs to another hardware type than the one attached.
    bool setState(const State& s);

private:
    void setBank(unsigned bank);
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
    State s_;
};

} // namespace k64

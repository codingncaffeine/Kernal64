// What is plugged into the expansion port: the ROM chips of a cartridge and the logic
// that switches them, by hardware type.
//
// The machine asks it for the level of the port's two configuration lines (GAME and
// EXROM, into the PLA) and passes on the accesses the PLA selects it for: ROML, ROMH and
// the two I/O areas ($DE00-$DEFF, $DF00-$DFFF). Every rule and its source:
// local-notes/hw/cart.md (P = the port, T<n> = hardware type n of the file format).
#pragma once

#include "crt.h"

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
    // The same bytes without any effect (the VIC-II's reads, the tools).
    uint8_t peekRomL(uint16_t addr) const { return romL_[(size_t{s_.bank} << 13) | (addr & 0x1FFF)]; }
    uint8_t peekRomH(uint16_t addr) const { return (*high_)[(size_t{s_.bankH} << 13) | (addr & 0x1FFF)]; }

    // True for the types with something that runs on the clock; clock() once per cycle.
    bool clocked() const { return type_ == 10; }
    void clock();

#pragma pack(push, 1)
    struct State {
        int16_t type = -1;         // the hardware type the state belongs to
        uint8_t game = 1, exrom = 1;
        uint8_t bank = 0;          // the bank ROML shows
        uint8_t bankH = 0;         // the bank ROMH shows
        uint8_t off = 0;           // switched off until the next reset
        uint8_t locked = 0;        // the register takes no more writes until the next reset
        uint16_t capacitor = 0;    // type 10: cycles since the last access that emptied it
        uint8_t ram[256] = {};     // type 32: the RAM in the second I/O area
    };
#pragma pack(pop)
    const State& state() const { return s_; }
    // False when the state belongs to another hardware type than the one attached.
    bool setState(const State& s);

private:
    void setBank(unsigned bank);

    int type_ = -1;
    unsigned banks_ = 1;
    uint8_t exromAtReset_ = 1, gameAtReset_ = 1;
    std::vector<uint8_t> romL_ = std::vector<uint8_t>(0x2000, 0xFF);   // banks of 8K
    std::vector<uint8_t> romH_ = std::vector<uint8_t>(0x2000, 0xFF);
    const std::vector<uint8_t>* high_ = &romH_;   // what ROMH shows: its own chips, or ROML's
    State s_;
};

} // namespace k64

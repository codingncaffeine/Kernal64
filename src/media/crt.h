// The .crt cartridge file: a header that names the hardware type and the level of the
// port's two configuration lines, then one packet per ROM chip (or bank of one).
// Format: local-notes/hw/cart.md F1-F3.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace k64 {

struct CartImage {
    struct Chip {
        uint16_t kind = 0;   // 0 ROM, 1 RAM, 2 flash, 3 EEPROM
        uint16_t bank = 0;
        uint16_t load = 0;   // where in the address space the chip sits
        std::vector<uint8_t> data;
    };
    uint16_t type = 0;       // hardware type number
    uint8_t exrom = 1, game = 1;   // line levels at power on: 0 = pulled low
    uint8_t subtype = 0;
    std::string name;
    std::vector<Chip> chips;
};

// False, with the reason in `error`, when the bytes are not a C64 cartridge file.
bool parseCrt(const uint8_t* data, size_t size, CartImage& out, std::string& error);

} // namespace k64

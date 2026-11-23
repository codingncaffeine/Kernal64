// The C64's PLA (906114-01): chip selects from the address, the 6510 port lines,
// the cartridge lines and the VIC-II's bus signals.
//
// The logic is the 82S100's own fuse map as tabulated in "The C64 PLA Dissected"
// (Thomas Giesel, rev. 1.1, 2012, section 2.6, CC BY-SA 3.0): 32 AND terms over the
// 16 inputs, and 8 outputs that OR a set of terms. The table below was generated
// from that section mechanically, and is checked for all 65,536 input combinations
// against a read-out of a real PLA (tools/placheck).
//
// Inputs and outputs use the bit order of that read-out. All "#" signals are active
// low; an output of 0 selects its chip.
#pragma once

#include <array>
#include <cstdint>

namespace k64::pla {

// Input bits.
constexpr uint16_t VA12 = 1u << 0, nCAS = 1u << 1, nLORAM = 1u << 2, nHIRAM = 1u << 3, nCHAREN = 1u << 4,
                   nVA14 = 1u << 5, A15 = 1u << 6, A14 = 1u << 7, nAEC = 1u << 8, RnW = 1u << 9, VA13 = 1u << 10,
                   nEXROM = 1u << 11, A13 = 1u << 12, BA = 1u << 13, A12 = 1u << 14, nGAME = 1u << 15;

// Output bits.
constexpr uint8_t nROML = 1u << 0, nIO = 1u << 1, nGRW = 1u << 2, nCHARROM = 1u << 3, nKERNAL = 1u << 4,
                  nBASIC = 1u << 5, nCASRAM = 1u << 6, nROMH = 1u << 7;

namespace detail {

struct Term { uint16_t mask, value; };
struct Output { uint8_t bit; uint32_t terms; bool inverted; };

// Section 2.6 of the paper, one column per term ('*' input high, '/' input low):
//   #CAS     ........*..................../*/
//   #LORAM   *...*......**..**..*............
//   #HIRAM   ****.*...**..**....*.*..........
//   #CHAREN  ...///...********...............
//   #VA14    ......**........................
//   A15      ******..***************.///**..*
//   A14      /*****..***********///*.//*/*..*
//   A13      ***///../////////////**..*.*/../
//   A12      ...***..***********.....*.../..*
//   BA       .........*.*.*.*.*..............
//   #AEC     //////**///////////////*......./
//   R/#W     ******../*/*/*/*/*/*.*........./
//   #EXROM   ../.././.....////**/*/*******...
//   #GAME    **/**/*/.****//////./////////...
//   VA13     ......//...............*........
//   VA12     ......**...............*........
inline constexpr Term kTerms[32] = {
    {0x93CC, 0x924C}, {0x93C8, 0x92C8}, {0x9BC8, 0x12C8}, {0xD3D8, 0xC2C8}, // p0-p3
    {0xD3D4, 0xC2C4}, {0xDBD8, 0x42C8}, {0x8521, 0x8121}, {0x8D21, 0x0121}, // p4-p7
    {0x53C2, 0x40C2}, {0xF3D8, 0xE2D8}, {0xD3D8, 0xC0D8}, {0xF3D4, 0xE2D4}, // p8-p11
    {0xD3D4, 0xC0D4}, {0xFBD8, 0x62D8}, {0xDBD8, 0x40D8}, {0xFBD4, 0x62D4}, // p12-p15
    {0xDBD4, 0x40D4}, {0xFBC0, 0x6AC0}, {0xDBC0, 0x48C0}, {0x1BCC, 0x024C}, // p16-p19
    {0x99C0, 0x0840}, {0x9BC8, 0x1248}, {0x99C0, 0x18C0}, {0x8D01, 0x0D01}, // p20-p23
    {0xC8C0, 0x4800}, {0x98C0, 0x1800}, {0x88C0, 0x0880}, {0x98C0, 0x1840}, // p24-p27
    {0xD8C0, 0x08C0}, {0x0002, 0x0000}, {0x0002, 0x0002}, {0x53C2, 0x40C0}, // p28-p31
};

//   outputs  terms ('+')                         sense
//            ++++++++.++++++++++++++++++++.+.     #CASRAM
//            +............................... NOT #BASIC
//            .++............................. NOT #KERNAL
//            ...+++++........................ NOT #CHARROM
//            ...............................+ NOT #GRW
//            .........++++++++++............. NOT #IO
//            ...................++........... NOT #ROML
//            .....................+++........ NOT #ROMH
inline constexpr Output kOutputs[8] = {
    {6, 0x5FFFFEFFu, false}, // #CASRAM: high (DRAM not selected) when any of its terms is true
    {5, 0x00000001u, true},  // #BASIC
    {4, 0x00000006u, true},  // #KERNAL
    {3, 0x000000F8u, true},  // #CHARROM
    {2, 0x80000000u, true},  // #GRW (color RAM write)
    {1, 0x0007FE00u, true},  // #IO
    {0, 0x00180000u, true},  // #ROML
    {7, 0x00E00000u, true},  // #ROMH
};

constexpr uint8_t evaluate(uint16_t in) {
    uint32_t active = 0;
    for (int p = 0; p < 32; ++p)
        if ((in & kTerms[p].mask) == kTerms[p].value) active |= 1u << p;
    uint8_t out = 0;
    for (const Output& o : kOutputs) {
        const bool any = (active & o.terms) != 0;
        if (o.inverted ? !any : any) out = static_cast<uint8_t>(out | (1u << o.bit));
    }
    return out;
}

} // namespace detail

// The outputs for one combination of the 16 inputs.
constexpr uint8_t evaluate(uint16_t inputs) { return detail::evaluate(inputs); }

// All 65,536 combinations, for lookups in the machine's memory map.
inline const std::array<uint8_t, 65536>& table() {
    static const std::array<uint8_t, 65536> t = [] {
        std::array<uint8_t, 65536> a{};
        for (uint32_t i = 0; i < 65536; ++i) a[i] = detail::evaluate(static_cast<uint16_t>(i));
        return a;
    }();
    return t;
}

} // namespace k64::pla

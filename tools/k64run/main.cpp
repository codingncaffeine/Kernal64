// k64run: boots the machine headless with the ROMs from a folder, runs a number of
// frames and prints the text screen (screen memory at $0400, as the KERNAL sets it).
//
//   k64run ROM_FOLDER [--frames N] [--model pal|ntsc|ntsc-old] [--expect TEXT]...
//
// --expect TEXT fails the run unless TEXT appears on the screen.
#include "c64.h"
#include "roms.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
char screenCodeToAscii(uint8_t c) {
    c &= 0x7F; // reverse video
    if (c < 0x20) return static_cast<char>(c + 0x40); // @, A-Z, [, ...
    if (c < 0x40) return static_cast<char>(c);         // space, punctuation, digits
    return '?';
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: k64run ROM_FOLDER [--frames N] [--model pal|ntsc|ntsc-old] [--expect TEXT]...\n"); return 2; }
    int frames = 150;
    k64::VicModel model = k64::VicModel::Mos6569;
    std::vector<std::string> expect;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--model" && i + 1 < argc) {
            std::string m = argv[++i];
            model = m == "ntsc" ? k64::VicModel::Mos6567R8 : m == "ntsc-old" ? k64::VicModel::Mos6567R56A : k64::VicModel::Mos6569;
        } else if (a == "--expect" && i + 1 < argc) expect.emplace_back(argv[++i]);
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    const k64::RomSet roms = k64::findRoms(argv[1]);
    static k64::C64 machine(model);
    machine.setRoms(roms);
    if (!machine.hasSystemRoms()) { std::fprintf(stderr, "k64run: BASIC, KERNAL or character ROM not found in %s\n", argv[1]); return 3; }
    machine.reset();
    for (int f = 0; f < frames; ++f) machine.runFrame();

    std::string screen;
    for (int row = 0; row < 25; ++row) {
        std::string line;
        for (int col = 0; col < 40; ++col) line += screenCodeToAscii(machine.ram()[0x0400 + row * 40 + col]);
        while (!line.empty() && line.back() == ' ') line.pop_back();
        screen += line + "\n";
    }
    std::printf("%s", screen.c_str());
    const auto& r = machine.cpuRegs();
    // $02A6 is where the KERNAL records what it measured at start-up: 1 = PAL, 0 = NTSC.
    std::printf("k64run: %d frames, %llu cycles, pc=%04x, KERNAL detected %s\n", frames,
                static_cast<unsigned long long>(machine.cycle()), r.pc, machine.ram()[0x02A6] ? "PAL" : "NTSC");
    int fail = 0;
    for (const auto& e : expect)
        if (screen.find(e) == std::string::npos) { std::printf("k64run: expected text not on screen: \"%s\"\n", e.c_str()); fail = 1; }
    return fail;
}

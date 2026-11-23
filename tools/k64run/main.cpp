// k64run: boots the machine headless with the ROMs from a folder, runs a number of
// frames and prints the text screen (screen memory at $0400, as the KERNAL sets it).
//
//   k64run ROM_FOLDER [--frames N] [--model pal|ntsc|ntsc-old] [--expect TEXT]...
//
// --expect TEXT fails the run unless TEXT appears on the screen.
// --png FILE writes the visible picture of the last frame.
// --raster FILE writes the whole raster of the last frame: one palette index per pixel,
//   rows = raster lines 0.., columns = pixels from the start of cycle 1.
// --autostart PRG once the KERNAL waits for input: puts the program into memory as LOAD
//   would and types RUN (the reference emulator's "inject" autostart).
// --debugcart ends the run when the program writes its result to $D7FF; the exit status is
//   that result (0 = passed), or 124 when --cycles runs out first.
// --cycles N runs N cycles from power on instead of --frames.
// --hash prints an FNV-1a hash of the last frame's visible palette indices.
#include "c64.h"
#include "roms.h"
#include "png.h"

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
    if (argc < 2) { std::fprintf(stderr, "usage: k64run ROM_FOLDER [--frames N] [--model pal|ntsc|ntsc-old] [--expect TEXT]... [--png FILE] [--hash]\n"); return 2; }
    int frames = 150;
    k64::VicModel model = k64::VicModel::Mos6569;
    std::vector<std::string> expect;
    std::string png, rasterOut, autostart;
    bool debugCart = false;
    uint64_t cycleLimit = 0;
    bool hash = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--model" && i + 1 < argc) {
            std::string m = argv[++i];
            model = m == "ntsc" ? k64::VicModel::Mos6567R8 : m == "ntsc-old" ? k64::VicModel::Mos6567R56A : k64::VicModel::Mos6569;
        } else if (a == "--expect" && i + 1 < argc) expect.emplace_back(argv[++i]);
        else if (a == "--png" && i + 1 < argc) png = argv[++i];
        else if (a == "--hash") hash = true;
        else if (a == "--raster" && i + 1 < argc) rasterOut = argv[++i];
        else if (a == "--autostart" && i + 1 < argc) autostart = argv[++i];
        else if (a == "--debugcart") debugCart = true;
        else if (a == "--cycles" && i + 1 < argc) cycleLimit = std::strtoull(argv[++i], nullptr, 10);
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    const k64::RomSet roms = k64::findRoms(argv[1]);
    static k64::C64 machine(model);
    machine.setRoms(roms);
    if (!machine.hasSystemRoms()) { std::fprintf(stderr, "k64run: BASIC, KERNAL or character ROM not found in %s\n", argv[1]); return 3; }
    machine.reset();
    machine.enableDebugCart(debugCart);
    std::vector<uint8_t> prg;
    if (!autostart.empty()) {
        FILE* f = std::fopen(autostart.c_str(), "rb");
        if (!f) { std::fprintf(stderr, "k64run: cannot open %s\n", autostart.c_str()); return 3; }
        int ch;
        while ((ch = std::fgetc(f)) != EOF) prg.push_back(static_cast<uint8_t>(ch));
        std::fclose(f);
        if (prg.size() < 3) { std::fprintf(stderr, "k64run: %s is not a program file\n", autostart.c_str()); return 3; }
    }
    bool injected = prg.empty();
    auto inject = [&] {
        // The KERNAL's input loop polls the keyboard buffer at $E5CD; from there on it is ready.
        const uint16_t load = static_cast<uint16_t>(prg[0] | (prg[1] << 8));
        const uint16_t end = static_cast<uint16_t>(load + prg.size() - 2);
        for (size_t i = 2; i < prg.size(); ++i) machine.pokeRam(static_cast<uint16_t>(load + i - 2), prg[i]);
        for (int p : {0x2D, 0x2F, 0x31, 0xAE}) {   // VARTAB, ARYTAB, STREND, end of load
            const auto ptr = static_cast<uint16_t>(p);
            machine.pokeRam(ptr, static_cast<uint8_t>(end));
            machine.pokeRam(static_cast<uint16_t>(ptr + 1), static_cast<uint8_t>(end >> 8));
        }
        const char run[] = {'R', 'U', 'N', 13};
        for (int i = 0; i < 4; ++i) machine.pokeRam(static_cast<uint16_t>(0x0277 + i), static_cast<uint8_t>(run[i]));
        machine.pokeRam(0xC6, 4);
        injected = true;
    };
    if (cycleLimit) {
        while (machine.cycle() < cycleLimit && machine.debugCartResult() < 0) {
            machine.tick();
            if (!injected && machine.cpuRegs().pc == 0xE5CD) inject();
        }
    } else {
        for (int f = 0; f < frames; ++f) machine.runFrame();
    }

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
    const auto& t = machine.timing();
    if (!png.empty()) {
        std::vector<uint32_t> frame(static_cast<size_t>(t.visibleWidth) * t.visibleLines);
        machine.visibleFrame(frame.data());
        if (!k64png::write(png, frame.data(), t.visibleWidth, t.visibleLines)) { std::fprintf(stderr, "k64run: cannot write %s\n", png.c_str()); return 3; }
    }
    if (!rasterOut.empty()) {
        FILE* f = std::fopen(rasterOut.c_str(), "wb");
        const size_t w = static_cast<size_t>(t.cyclesPerLine) * 8;
        bool ok = f != nullptr;
        for (int line = 0; ok && line < t.linesPerFrame; ++line)
            ok = std::fwrite(machine.raster() + static_cast<size_t>(line) * k64::kMaxRasterWidth, 1, w, f) == w;
        if (!f || std::fclose(f) != 0 || !ok) { std::fprintf(stderr, "k64run: cannot write %s\n", rasterOut.c_str()); return 3; }
    }
    if (hash) {
        uint64_t h = 1469598103934665603ull;
        for (int row = 0; row < t.visibleLines; ++row) {
            const int line = (t.firstVisibleLine + row) % t.linesPerFrame;
            const uint8_t* src = machine.raster() + static_cast<size_t>(line) * k64::kMaxRasterWidth + t.firstVisiblePixel;
            for (int x = 0; x < t.visibleWidth; ++x) { h ^= src[x]; h *= 1099511628211ull; }
        }
        std::printf("k64run: frame hash %016llx\n", static_cast<unsigned long long>(h));
    }
    if (debugCart) {
        const int result = machine.debugCartResult();
        std::printf("k64run: %s at cycle %llu\n", result < 0 ? "no result" : result == 0 ? "passed" : "failed",
                    static_cast<unsigned long long>(machine.cycle()));
        return result < 0 ? 124 : result;
    }
    int fail = 0;
    for (const auto& e : expect)
        if (screen.find(e) == std::string::npos) { std::printf("k64run: expected text not on screen: \"%s\"\n", e.c_str()); fail = 1; }
    return fail;
}

// k64run: boots the machine headless with the ROMs from a folder, runs a number of
// frames and prints the text screen (screen memory at $0400, as the KERNAL sets it).
//
//   k64run ROM_FOLDER [--frames N] [--model pal|pal-new|pal-old|ntsc|ntsc-new|ntsc-old] [--cia old|new] [--sid old|new] [--expect TEXT]...
//
// --sid-noise-pulse keeps: the 6581 variant on which noise + pulse leaves the noise register
//   alone (the core option kernal64_sid_noise_pulse).
//
// --paddles PORT X Y puts a pair of paddles in control port 1 or 2, set so that the SID's
//   registers read X and Y (0-255) with that port alone switched through.
//
// --expect TEXT fails the run unless TEXT appears on the screen.
// --png FILE writes the visible picture of the last frame.
// --raster FILE writes the whole raster of the last frame: one palette index per pixel,
//   rows = raster lines 0.., columns = pixels from the start of cycle 1.
// --raster-frames N (with --raster and --cycles) appends the raster after each of N more
//   frames: N+1 rasters, to find the frame in which an animated picture equals a reference.
// --cart FILE.crt plugs a cartridge in before the machine is switched on.
// --autostart FILE (a program file, the same in a P00 wrapper, or a T64 container) once
//   the KERNAL waits for input: puts the program into memory as LOAD would and types RUN
//   (the reference emulator's "inject" autostart).
// --autostart-delay N waits N more cycles after the KERNAL first waits for input (a
//   program whose picture depends on when it was started shows it by changing with N).
// --debugcart ends the run when the program writes its result to $D7FF; the exit status is
//   that result (0 = passed), or 124 when --cycles runs out first.
// --cycles N runs N cycles from power on instead of --frames.
// --hash prints an FNV-1a hash of the last frame's visible palette indices.
// --where ADDR (hex; with --cycles) prints the raster line and cycle of the opcode fetch each
//   time the CPU starts the instruction at ADDR (the first 32 times): where on the screen
//   a piece of a test program runs.
// --freeze-at CYCLE (with --cycles; may be given several times) presses the freeze button of
//   a freezer cartridge at that machine cycle; --freeze-pc ADDR (hex; several) the first time
//   the CPU starts the instruction at ADDR. --freeze-hold N: let go of it N cycles later
//   (default 1).
// --type-at CYCLE TEXT (with --cycles; may be given several times) puts TEXT into the
//   KERNAL's keyboard buffer at that machine cycle, as if typed ("\r" = RETURN, "\xNN" = the
//   PETSCII code NN, e.g. "\x86" = F3; at most 10 characters).
// --drive puts a 1541 on the serial bus as drive 8 (its ROMs must be in the folder). Off by
//   default: every expected result so far was made without one, on both emulators.
// --disk FILE (D64 or G64) puts the disk in drive 8 (and implies --drive); --blank-disk a
//   blank one (nothing recorded, as out of its box).
// --save-disk FILE.g64 writes the disk in drive 8, as it is at the end, as a G64.
// --dump START:LEN prints LEN bytes of RAM from START (hex) at the end, 16 per line.
#include "c64.h"
#include "roms.h"
#include "png.h"

#include <cctype>
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
    if (argc < 2) { std::fprintf(stderr, "usage: k64run ROM_FOLDER [--frames N] [--model pal|pal-new|pal-old|ntsc|ntsc-new|ntsc-old] [--expect TEXT]... [--png FILE] [--hash]\n"); return 2; }
    int frames = 150;
    k64::VicModel model = k64::VicModel::Mos6569;
    k64::CiaModel cia = k64::CiaModel::Mos6526;
    k64::SidModel sid = k64::SidModel::Mos6581;
    bool noisePulseDrains = true;
    int paddles[2][2] = {{-1, -1}, {-1, -1}};
    std::vector<std::string> expect;
    std::string png, rasterOut, autostart, cartFile;
    bool debugCart = false;
    uint64_t cycleLimit = 0;
    bool hash = false;
    int rasterFrames = 0;
    uint64_t autostartDelay = 0;
    int where = -1, whereLeft = 32;
    bool whereSeen = false;
    std::vector<std::pair<unsigned, unsigned>> dumps;
    std::vector<uint64_t> freezeAt;
    std::vector<int> freezePc;
    uint64_t freezeHold = 1, freezeRelease = 0;
    std::vector<std::pair<uint64_t, std::string>> typeAt;
    bool drive = false;
    std::string diskFile, saveDisk;
    std::shared_ptr<k64::GcrDisk> theDisk;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--model" && i + 1 < argc) {
            if (!k64::vicModelFromName(argv[++i], model)) { std::fprintf(stderr, "k64run: unknown model %s (%s)\n", argv[i], k64::kVicModelNames); return 2; }
        } else if (a == "--cia" && i + 1 < argc) cia = std::string(argv[++i]) == "new" ? k64::CiaModel::Mos8521 : k64::CiaModel::Mos6526;
        else if (a == "--sid" && i + 1 < argc) sid = std::string(argv[++i]) == "new" ? k64::SidModel::Mos8580 : k64::SidModel::Mos6581;
        else if (a == "--sid-noise-pulse" && i + 1 < argc) noisePulseDrains = std::string(argv[++i]) != "keeps";
        else if (a == "--paddles" && i + 3 < argc) {
            const int port = std::atoi(argv[++i]);
            if (port != 1 && port != 2) { std::fprintf(stderr, "k64run: --paddles takes control port 1 or 2\n"); return 2; }
            paddles[port - 1][0] = std::atoi(argv[++i]);
            paddles[port - 1][1] = std::atoi(argv[++i]);
        }
        else if (a == "--cart" && i + 1 < argc) cartFile = argv[++i];
        else if (a == "--expect" && i + 1 < argc) expect.emplace_back(argv[++i]);
        else if (a == "--png" && i + 1 < argc) png = argv[++i];
        else if (a == "--hash") hash = true;
        else if (a == "--raster" && i + 1 < argc) rasterOut = argv[++i];
        else if (a == "--raster-frames" && i + 1 < argc) rasterFrames = std::atoi(argv[++i]);
        else if (a == "--autostart" && i + 1 < argc) autostart = argv[++i];
        else if (a == "--autostart-delay" && i + 1 < argc) autostartDelay = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--debugcart") debugCart = true;
        else if (a == "--cycles" && i + 1 < argc) cycleLimit = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--freeze-at" && i + 1 < argc) freezeAt.push_back(std::strtoull(argv[++i], nullptr, 10));
        else if (a == "--freeze-pc" && i + 1 < argc) freezePc.push_back(static_cast<int>(std::strtoul(argv[++i], nullptr, 16)));
        else if (a == "--freeze-hold" && i + 1 < argc) freezeHold = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--type-at" && i + 2 < argc) {
            const uint64_t at = std::strtoull(argv[++i], nullptr, 10);
            std::string text;
            for (const char* p = argv[++i]; *p; ++p) {
                if (p[0] == '\\' && p[1] == 'r') { text += '\r'; ++p; }
                else if (p[0] == '\\' && p[1] == 'x' && std::isxdigit(static_cast<unsigned char>(p[2])) &&
                         std::isxdigit(static_cast<unsigned char>(p[3]))) {
                    text += static_cast<char>(std::strtoul(std::string(p + 2, 2).c_str(), nullptr, 16));
                    p += 3;
                }
                else text += *p;
            }
            typeAt.emplace_back(at, text.substr(0, 10));
        }
        else if (a == "--drive") drive = true;
        else if (a == "--disk" && i + 1 < argc) { diskFile = argv[++i]; drive = true; }
        else if (a == "--blank-disk") { diskFile = "-"; drive = true; }
        else if (a == "--save-disk" && i + 1 < argc) saveDisk = argv[++i];
        else if (a == "--where" && i + 1 < argc) where = static_cast<int>(std::strtoul(argv[++i], nullptr, 16));
        else if (a == "--dump" && i + 1 < argc) {
            char* end = nullptr;
            const unsigned start = static_cast<unsigned>(std::strtoul(argv[++i], &end, 16));
            const unsigned len = (end && *end == ':') ? static_cast<unsigned>(std::strtoul(end + 1, nullptr, 16)) : 0x100;
            dumps.emplace_back(start, len);
        }
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    const k64::RomSet roms = k64::findRoms(argv[1]);
    static k64::C64 machine(model);
    machine.setRoms(roms);
    machine.setDriveWanted(drive);
    machine.setCiaModel(cia);
    machine.setSidModel(sid);
    machine.setSidNoisePulseDrains(noisePulseDrains);
    if (!machine.hasSystemRoms()) { std::fprintf(stderr, "k64run: BASIC, KERNAL or character ROM not found in %s\n", argv[1]); return 3; }
    auto readFile = [](const std::string& path, std::vector<uint8_t>& bytes) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        int ch;
        while ((ch = std::fgetc(f)) != EOF) bytes.push_back(static_cast<uint8_t>(ch));
        std::fclose(f);
        return true;
    };
    if (!cartFile.empty()) {
        std::vector<uint8_t> bytes;
        k64::CartImage image;
        std::string error;
        if (!readFile(cartFile, bytes)) { std::fprintf(stderr, "k64run: cannot open %s\n", cartFile.c_str()); return 3; }
        if (!k64::parseCrt(bytes.data(), bytes.size(), image, error) || !machine.attachCartridge(image, error)) {
            std::fprintf(stderr, "k64run: %s: %s\n", cartFile.c_str(), error.c_str());
            return 3;
        }
    }
    if (!diskFile.empty()) {
        std::vector<uint8_t> bytes;
        auto disk = std::make_shared<k64::GcrDisk>();
        std::string error;
        if (diskFile == "-") *disk = k64::blankDisk();
        else if (!readFile(diskFile, bytes)) { std::fprintf(stderr, "k64run: cannot open %s\n", diskFile.c_str()); return 3; }
        else if (!k64::parseDisk(bytes.data(), bytes.size(), *disk, error)) {
            std::fprintf(stderr, "k64run: %s: %s\n", diskFile.c_str(), error.c_str());
            return 3;
        }
        machine.insertDisk(disk);
        theDisk = disk;
    }
    if (!autostart.empty()) {
        std::vector<uint8_t> bytes;
        k64::ProgramFile program;
        std::string error;
        if (!readFile(autostart, bytes)) { std::fprintf(stderr, "k64run: cannot open %s\n", autostart.c_str()); return 3; }
        if (!k64::parseProgram(bytes.data(), bytes.size(), program, error)) {
            std::fprintf(stderr, "k64run: %s: %s\n", autostart.c_str(), error.c_str());
            return 3;
        }
        machine.setAutostart(program, autostartDelay);
    }
    machine.reset();
    for (int port = 0; port < 2; ++port) machine.setPaddles(port + 1, paddles[port][0], paddles[port][1]);
    machine.enableDebugCart(debugCart);
    if (cycleLimit) {
        while (machine.cycle() < cycleLimit && machine.debugCartResult() < 0) {
            if (where >= 0) {
                const bool at = machine.cpuAtInstructionStart() && machine.cpuRegs().pc == where;
                if (at && !whereSeen && whereLeft > 0) {
                    --whereLeft;
                    std::printf("k64run: $%04x starts in line %d, cycle %d (machine cycle %llu)\n", static_cast<unsigned>(where),
                                machine.vic().line(), machine.vic().cycleInLine(), static_cast<unsigned long long>(machine.cycle()));
                }
                whereSeen = at;
            }
            for (const auto& [at, text] : typeAt) {
                if (machine.cycle() != at) continue;
                for (size_t n = 0; n < text.size(); ++n) machine.pokeRam(static_cast<uint16_t>(0x0277 + n), static_cast<uint8_t>(text[n]));
                machine.pokeRam(0x00C6, static_cast<uint8_t>(text.size()));
            }
            bool press = false;
            for (uint64_t at : freezeAt) press = press || machine.cycle() == at;
            for (int& pc : freezePc)
                if (pc >= 0 && machine.cpuAtInstructionStart() && machine.cpuRegs().pc == pc) { press = true; pc = -1; }
            if (press) {
                machine.setFreezeButton(true);
                freezeRelease = machine.cycle() + freezeHold;
            } else if (freezeRelease && machine.cycle() >= freezeRelease) {
                machine.setFreezeButton(false);
                freezeRelease = 0;
            }
            machine.tick();
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
    if (machine.driveAttached())
        std::printf("k64run: drive 8: head on half-track %d, pc=%04x, motor %s\n", machine.drive().halfTrack(),
                    static_cast<unsigned>(machine.drive().cpuRegs().pc), machine.drive().motor() ? "on" : "off");
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
        for (int n = 0; ok && n <= rasterFrames; ++n) {
            for (int line = 0; ok && line < t.linesPerFrame; ++line)
                ok = std::fwrite(machine.raster() + static_cast<size_t>(line) * k64::kMaxRasterWidth, 1, w, f) == w;
            if (n < rasterFrames)
                for (int c = 0; c < t.cyclesPerFrame(); ++c) machine.tick();
        }
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
    if (!saveDisk.empty() && theDisk) {
        const auto g = k64::writeG64(*theDisk);
        FILE* f = std::fopen(saveDisk.c_str(), "wb");
        if (!f || std::fwrite(g.data(), 1, g.size(), f) != g.size()) std::fprintf(stderr, "k64run: cannot write %s\n", saveDisk.c_str());
        if (f) std::fclose(f);
    }
    for (const auto& [start, len] : dumps)
        for (unsigned a = start; a < start + len; a += 16) {
            std::printf("%04x:", a & 0xFFFF);
            for (unsigned b = a; b < a + 16 && b < start + len; ++b) std::printf(" %02x", machine.ram()[b & 0xFFFF]);
            std::printf("\n");
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

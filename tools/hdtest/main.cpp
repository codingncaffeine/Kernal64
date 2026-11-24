// hdtest: runs the CMD HD's board on its own: its boot ROM, a disk image, the front panel.
//
//   hdtest ROM [--disk FILE.dhd] [--seconds S] [--hold swap8,swap9,wp] [--scsi-trace]
//          [--expect-pc-range LO-HI] [--expect-visit ADDR]
//
// Prints the panel latch each time it changes (with the time), the SCSI commands with
// --scsi-trace, and where the processor is at the end. --expect-pc-range: exit 0 only if
// the processor ends inside LO-HI (hex). --expect-visit: exit 0 only if the instruction at
// ADDR (hex) ran in the last half second, e.g. the head of the boot ROM's idle loop.
#include "cmdhd.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: hdtest ROM [--disk FILE] [--seconds S] [--hold LIST] [--scsi-trace] [--expect-pc-range LO-HI]\n"); return 2; }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)), {});
    double seconds = 2;
    std::string disk, hold;
    bool trace = false;
    long lo = -1, hi = -1, visit = -1;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--disk" && i + 1 < argc) disk = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--hold" && i + 1 < argc) hold = argv[++i];
        else if (a == "--scsi-trace") trace = true;
        else if (a == "--expect-pc-range" && i + 1 < argc) {
            const std::string r = argv[++i];
            lo = std::strtol(r.c_str(), nullptr, 16);
            hi = std::strtol(r.substr(r.find('-') + 1).c_str(), nullptr, 16);
        } else if (a == "--expect-visit" && i + 1 < argc) visit = std::strtol(argv[++i], nullptr, 16);
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    static k64::CmdHd hd;
    if (!hd.setRom(rom.data(), rom.size())) { std::fprintf(stderr, "hdtest: not a 16K boot ROM (or a 32K image of it twice)\n"); return 2; }
    static k64::BlockFile file;
    if (!disk.empty()) {
        if (!file.open(disk, 0)) { std::fprintf(stderr, "hdtest: cannot open %s\n", disk.c_str()); return 2; }
        hd.attachDisk(&file);
    }
    hd.scsi().trace = trace;
    hd.setButtons(hold.find("swap8") != std::string::npos, hold.find("swap9") != std::string::npos,
                  hold.find("wp") != std::string::npos);
    hd.setTime({26, 11, 24, 2, 12, 0, 0});
    hd.reset();
    const uint64_t cycles = static_cast<uint64_t>(seconds * k64::CmdHd::kClockHz);
    uint8_t lights = hd.lights();
    std::printf("hdtest: latch %02X at 0.000000 s\n", lights);
    long changes = 0, visits = 0;
    const uint64_t lastHalf = cycles > k64::CmdHd::kClockHz / 2 ? cycles - k64::CmdHd::kClockHz / 2 : 0;
    for (uint64_t c = 0; c < cycles; ++c) {
        if (visit >= 0 && c >= lastHalf && hd.cpuAtInstructionStart() && hd.cpuRegs().pc == visit) ++visits;
        hd.tick();
        if (hd.lights() != lights && changes < 60) {
            lights = hd.lights();
            ++changes;
            std::printf("hdtest: latch %02X at %.6f s (pc %04X)\n", lights, double(c) / k64::CmdHd::kClockHz, hd.cpuRegs().pc);
        }
    }
    const unsigned pc = hd.cpuRegs().pc;
    std::printf("hdtest: pc %04X after %.2f s, %ld latch changes\n", pc, seconds, changes);
    if (visit >= 0) {
        std::printf("hdtest: %04lX ran %ld times in the last half second: %s\n", visit, visits, visits ? "PASS" : "FAIL");
        if (!visits) return 1;
    }
    if (lo >= 0) {
        const bool ok = pc >= static_cast<unsigned>(lo) && pc <= static_cast<unsigned>(hi);
        std::printf("hdtest: %s\n", ok ? "PASS" : "FAIL (pc outside the expected range)");
        return ok ? 0 : 1;
    }
    return 0;
}

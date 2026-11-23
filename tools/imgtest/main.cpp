// imgtest: runs a 6502 test program image on the CPU core with 64 KB of plain RAM
// and decides pass/fail from where the program ends up.
//
//   imgtest FILE@HEXADDR --start HEX [--success HEX] [--fail HEX]... [--print HEX]
//           [--end HEX] [--max-cycles N]
//
//   --success HEX  the program parks in a loop at this address when it passes
//   --end HEX      the program parks here when it is done (pass decided by --fail)
//   --fail HEX     entering this address counts one failure
//   --print HEX    entering this address prints the character in A
//
// Any other "JMP *" loop is a failure (the trap of the functional test suite).
#include "cpu6502.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
struct Ram {
    uint8_t mem[65536] = {};
    uint8_t read(uint16_t a) { return mem[a]; }
    void write(uint16_t a, uint8_t v) { mem[a] = v; }
};
uint16_t hex(const char* s) { return static_cast<uint16_t>(std::strtoul(s, nullptr, 16)); }
} // namespace

int main(int argc, char** argv) {
    static Ram ram;
    int start = -1, success = -1, end = -1, print = -1;
    std::vector<int> fails;
    long long maxCycles = 200'000'000;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--start") start = hex(next());
        else if (a == "--success") success = hex(next());
        else if (a == "--end") end = hex(next());
        else if (a == "--fail") fails.push_back(hex(next()));
        else if (a == "--print") print = hex(next());
        else if (a == "--max-cycles") maxCycles = std::atoll(next());
        else {
            const auto at = a.rfind('@');
            if (at == std::string::npos) { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
            std::ifstream f(a.substr(0, at), std::ios::binary);
            if (!f) { std::fprintf(stderr, "cannot read %s\n", a.substr(0, at).c_str()); return 2; }
            std::vector<char> data((std::istreambuf_iterator<char>(f)), {});
            const size_t base = hex(a.c_str() + at + 1);
            if (base + data.size() > sizeof ram.mem) { std::fprintf(stderr, "image does not fit\n"); return 2; }
            std::memcpy(ram.mem + base, data.data(), data.size());
        }
    }
    if (start < 0) { std::fprintf(stderr, "--start is required\n"); return 2; }

    k64::Cpu6502<Ram> cpu(ram);
    cpu.state().r.pc = static_cast<uint16_t>(start);
    long long cycles = 0;
    long failures = 0;
    uint16_t lastPc = 0xFFFF;
    std::string out;
    while (cycles < maxCycles) {
        cpu.tick();
        ++cycles;
        if (!cpu.atInstructionStart()) continue;
        const uint16_t pc = cpu.regs().pc;
        if (pc == lastPc) {
            // A JMP * (or a branch to itself): the program has parked.
            if (pc == success) { std::printf("imgtest: passed (parked at %04x after %lld cycles)\n", pc, cycles); return 0; }
            if (pc != end) { std::printf("imgtest: FAILED, trapped at %04x after %lld cycles\n", pc, cycles); return 1; }
        }
        if (pc == end) {
            if (!out.empty()) std::printf("%s\n", out.c_str());
            std::printf("imgtest: finished after %lld cycles, %ld failure(s)\n", cycles, failures);
            return failures ? 1 : 0;
        }
        for (int f : fails) if (pc == f) ++failures;
        if (pc == print) {
            const char c = static_cast<char>(cpu.regs().a);
            if (c == 13 || c == 10) { if (!out.empty() && out.back() != '\n') out += '\n'; }
            else if (c == 8) { if (!out.empty()) out.pop_back(); }
            else if (c >= 32 && c < 127) out += c;
        }
        // Detect parking on the next fetch: a JMP * returns to the same address.
        const uint8_t op = ram.mem[pc];
        lastPc = (op == 0x4C && ram.mem[static_cast<uint16_t>(pc + 1)] == (pc & 0xFF) &&
                  ram.mem[static_cast<uint16_t>(pc + 2)] == (pc >> 8)) ? pc : 0xFFFF;
        if (op != 0x4C) lastPc = pc == lastPc ? pc : 0xFFFF;
    }
    if (!out.empty()) std::printf("%s\n", out.c_str());
    std::printf("imgtest: FAILED, no result after %lld cycles (pc %04x)\n", cycles, cpu.regs().pc);
    return 1;
}

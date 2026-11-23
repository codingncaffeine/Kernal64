// cpudiff: runs our 6502 and the transistor-level simulation of the real chip
// (perfect6502) side by side on the same random programs, with IRQ, NMI and RDY
// changing at random cycles, and compares every bus cycle.
//
//   cpudiff [--seeds N] [--first S] [--cycles N] [--irq] [--nmi] [--rdy] [--all-opcodes]
//           [--exclude HEX,HEX,...] [--probe OPCODE_HEX]
//
// --probe loops one opcode in immediate mode over random A, X, flags and operand
// (LDA #p, PHA, PLP, LDA #a, LDX #x, OP #i, then A, X and P are stored) and
// prints the inputs of the first case where the two CPUs disagree.
//
// --scan HEX,HEX,... [--scan-len N] [--scan-line rdy|irq|nmi] prints the chip's
// own bus trace for that instruction with the line active for N cycles from each
// cycle in turn.
//
// Without --irq/--nmi/--rdy no line is touched. Memory is filled with opcodes only
// (so wherever execution lands it finds an instruction); the halt opcodes are
// always left out, and ANE, LXA, LAS, TAS, ANC, ALR and ARR unless --all-opcodes
// is given (see the note where the list is built).
#include "cpu6502.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern "C" {
void* initAndResetChip(void);
void destroyChip(void*);
void step(void*);
unsigned short readAddressBus(void*);
unsigned char readDataBus(void*);
unsigned int readRW(void*);
unsigned short readPC(void*);
unsigned char readA(void*);
unsigned char readX(void*);
unsigned char readY(void*);
unsigned char readSP(void*);
unsigned char readP(void*);
extern unsigned char memory[65536];
void p6502_set_irq(void*, int);
void p6502_set_nmi(void*, int);
void p6502_set_rdy(void*, int);
}

namespace {

struct Cycle { uint16_t addr; uint8_t data; bool write; };

struct OurBus {
    uint8_t mem[65536];
    Cycle last{};
    uint8_t read(uint16_t a) { last = {a, mem[a], false}; return mem[a]; }
    void write(uint16_t a, uint8_t v) { mem[a] = v; last = {a, v, true}; }
};

struct Lines { bool irq = false, nmi = false, rdy = true; };

std::string show(const Cycle& c) {
    char b[32];
    std::snprintf(b, sizeof b, "%c %04x %02x", c.write ? 'W' : 'r', c.addr, c.data);
    return b;
}

} // namespace

int main(int argc, char** argv) {
    int seeds = 4, first = 1;
    long cycles = 20000;
    bool useIrq = false, useNmi = false, useRdy = false, allOps = false;
    std::vector<int> excluded;
    int probe = -1;
    std::vector<uint8_t> scan;
    int scanLen = 2;
    std::string scanLine = "rdy";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--seeds" && i + 1 < argc) seeds = std::atoi(argv[++i]);
        else if (a == "--first" && i + 1 < argc) first = std::atoi(argv[++i]);
        else if (a == "--cycles" && i + 1 < argc) cycles = std::atol(argv[++i]);
        else if (a == "--irq") useIrq = true;
        else if (a == "--nmi") useNmi = true;
        else if (a == "--rdy") useRdy = true;
        else if (a == "--all-opcodes") allOps = true;
        else if (a == "--scan" && i + 1 < argc) {
            for (char* tok = std::strtok(argv[++i], ","); tok; tok = std::strtok(nullptr, ","))
                scan.push_back(static_cast<uint8_t>(std::strtol(tok, nullptr, 16)));
        } else if (a == "--scan-len" && i + 1 < argc) scanLen = std::atoi(argv[++i]);
        else if (a == "--scan-line" && i + 1 < argc) scanLine = argv[++i];
        else if (a == "--probe" && i + 1 < argc) probe = static_cast<int>(std::strtol(argv[++i], nullptr, 16));
        else if (a == "--exclude" && i + 1 < argc) {
            for (char* tok = std::strtok(argv[++i], ","); tok; tok = std::strtok(nullptr, ","))
                excluded.push_back(static_cast<int>(std::strtol(tok, nullptr, 16)));
        }
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }

    std::vector<uint8_t> allowed;
    for (int op = 0; op < 256; ++op) {
        const bool jam = (op & 0x0F) == 0x02 && op != 0x82 && op != 0xA2 && op != 0xC2 && op != 0xE2;
        // Left out: ANE and LXA (unstable on real chips), and the opcodes whose result comes from
        // internal drivers fighting over one bus, which a switch-level netlist does not resolve the
        // way the measured chips do: LAS and TAS (the stack pointer they load; A = X = S = M & S for
        // LAS on hardware) and ANC/ALR/ARR (the simulation leaves A unchanged; on hardware they write
        // it). Those are checked against the per-opcode bus traces and hardware test programs instead.
        const bool unstable = op == 0x8B || op == 0xAB || op == 0x9B || op == 0xBB ||
                              op == 0x0B || op == 0x2B || op == 0x4B || op == 0x6B;
        bool skip = false;
        for (int e : excluded) skip |= e == op;
        if (!jam && !skip && (allOps || !unstable)) allowed.push_back(static_cast<uint8_t>(op));
    }

    bool isAllowed[256] = {};
    for (uint8_t op : allowed) isAllowed[op] = true;
    static OurBus bus;
    if (!scan.empty()) {
        // RDY scan: the chip alone. Program at $0200: the bytes given, then JMP $0200. For each start
        // cycle k after the loop's first opcode fetch, RDY is held low for `scanLen` cycles; the chip's
        // bus is printed for the instruction and the cycles around the stall.
        for (int k = 0; k <= 9; ++k) {
            std::memset(memory, 0xEA, sizeof memory);
            for (size_t b = 0; b < scan.size(); ++b) memory[0x200 + b] = scan[b];
            const size_t j = 0x200 + scan.size();
            memory[j] = 0x4C; memory[j + 1] = 0x00; memory[j + 2] = 0x02;
            memory[0xFFFC] = 0x00; memory[0xFFFD] = 0x02;
            memory[0xFFFE] = 0x00; memory[0xFFFF] = 0x03; memory[0x300] = 0x40;
            void* chip = initAndResetChip();
            int fetches = 0;
            for (int c = 0; c < 200 && fetches < 2; ++c) {
                step(chip); step(chip);
                if (readRW(chip) && readAddressBus(chip) == 0x200) ++fetches;
            }
            std::printf("%s from cycle +%d for %d:", scanLine.c_str(), k + 1, scanLen);
            for (int c = 1; c <= 14; ++c) {
                const bool active = c >= k + 1 && c < k + 1 + scanLen;
                if (scanLine == "rdy") p6502_set_rdy(chip, !active);
                else if (scanLine == "nmi") p6502_set_nmi(chip, active);
                else p6502_set_irq(chip, active);
                step(chip); step(chip);
                std::printf(" %c%04x", readRW(chip) ? 'r' : 'W', readAddressBus(chip));
                if (c == k + scanLen) std::printf("|");
            }
            std::printf("\n");
            destroyChip(chip);
        }
        return 0;
    }
    int failures = 0;
    for (int seed = first; seed < first + seeds; ++seed) {
        std::mt19937 rng(static_cast<uint32_t>(seed));
        auto pick = [&](unsigned n) { return static_cast<unsigned>(rng() % n); };
        for (int a = 0; a < 65536; ++a) memory[a] = allowed[pick(static_cast<unsigned>(allowed.size()))];
        static const uint8_t kProbe[] = {0xA9, 0, 0x48, 0x28, 0xA9, 0, 0xA2, 0, 0, 0, 0x85, 0x10, 0x86, 0x11,
                                         0x08, 0x68, 0x85, 0x12, 0x4C, 0x00, 0x02};
        if (probe >= 0) {
            std::memcpy(memory + 0x200, kProbe, sizeof kProbe);
            memory[0x208] = static_cast<uint8_t>(probe);
            memory[0xFFFC] = 0x00; memory[0xFFFD] = 0x02;
        }
        std::memcpy(bus.mem, memory, sizeof bus.mem);

        void* chip = initAndResetChip();
        const uint16_t resetTarget = static_cast<uint16_t>(memory[0xFFFC] | (memory[0xFFFD] << 8));
        // Run the real chip until it fetches at the reset target; then align ours to it.
        int guard = 0;
        for (;;) {
            step(chip); step(chip);
            if (readRW(chip) && readAddressBus(chip) == resetTarget) break;
            if (++guard > 100) { std::fprintf(stderr, "seed %d: chip never reached the reset target\n", seed); return 3; }
        }
        k64::Cpu6502<OurBus> cpu(bus);
        auto& st = cpu.state();
        st = {};
        st.r.a = readA(chip); st.r.x = readX(chip); st.r.y = readY(chip); st.r.s = readSP(chip);
        st.r.p = static_cast<uint8_t>((readP(chip) | k64::flag::U) & ~k64::flag::B);
        st.r.pc = resetTarget;
        // Our first cycle is the opcode fetch the chip just did.
        cpu.tick();
        // The chip read the opcode from its memory; the two memories are identical.

        Lines ln;
        uint8_t probeIn[4] = {};
        int irqHold = 0, nmiHold = 0, rdyHold = 0;
        std::vector<std::pair<Cycle, Cycle>> recent;
        std::vector<Lines> recentLines;
        struct Step { k64::CpuRegs r; uint8_t op; uint8_t chipA; };
        std::vector<Step> history;
        long n = 1;
        bool ok = true;
        bool excludedStop = false;
        for (; n < cycles; ++n) {
            // Change the lines between cycles, the same way on both.
            if (useIrq && --irqHold <= 0) { ln.irq = !ln.irq && pick(3) == 0; irqHold = 1 + static_cast<int>(pick(ln.irq ? 40 : 400)); }
            if (useNmi && --nmiHold <= 0) { ln.nmi = !ln.nmi && pick(4) == 0; nmiHold = 1 + static_cast<int>(pick(ln.nmi ? 20 : 900)); }
            if (useRdy && --rdyHold <= 0) { ln.rdy = !(ln.rdy && pick(3) == 0); rdyHold = 1 + static_cast<int>(pick(ln.rdy ? 200 : 14)); }
            p6502_set_irq(chip, ln.irq); p6502_set_nmi(chip, ln.nmi); p6502_set_rdy(chip, ln.rdy);
            cpu.setIrq(ln.irq); cpu.setNmi(ln.nmi); cpu.setRdy(ln.rdy);

            step(chip); step(chip);
            const Cycle real{readAddressBus(chip), readDataBus(chip), readRW(chip) == 0};
            if (probe >= 0 && cpu.atInstructionStart() && cpu.regs().pc == 0x200) {
                const uint8_t in[4] = {static_cast<uint8_t>(rng()), static_cast<uint8_t>(rng()), static_cast<uint8_t>(rng()),
                                       static_cast<uint8_t>(rng())};
                const uint16_t at[4] = {0x201, 0x205, 0x207, 0x209};
                for (int k = 0; k < 4; ++k) { memory[at[k]] = in[k]; bus.mem[at[k]] = in[k]; }
                std::memcpy(probeIn, in, 4);
            }
            const bool fetch = cpu.atInstructionStart();
            const k64::CpuRegs before = cpu.regs();
            cpu.tick();
            const Cycle ours = bus.last;
            if (fetch && !cpu.state().takeInterrupt && !isAllowed[ours.data]) {
                // The program wrote an excluded opcode and ran into it: what follows is no longer a
                // comparison of the two CPUs (see the note on the excluded list).
                excludedStop = true;
                break;
            }
            if (fetch) {
                history.push_back({before, ours.data, readA(chip)});
                if (history.size() > 12) history.erase(history.begin());
            }
            recent.emplace_back(real, ours);
            recentLines.push_back(ln);
            if (recent.size() > 24) { recent.erase(recent.begin()); recentLines.erase(recentLines.begin()); }
            if (real.addr != ours.addr || real.write != ours.write || real.data != ours.data) { ok = false; break; }
        }
        if (ok && excludedStop) {
            std::printf("seed %d: %ld cycles identical, then ran into an excluded opcode\n", seed, n);
        } else if (ok) {
            std::printf("seed %d: %ld cycles identical\n", seed, n);
        } else {
            ++failures;
            std::printf("seed %d: DIFFERS at cycle %ld (lines irq=%d nmi=%d rdy=%d)\n", seed, n, ln.irq, ln.nmi, ln.rdy);
            std::printf("   chip         ours         lines\n");
            for (size_t k = 0; k < recent.size(); ++k) {
                const auto& [r, o] = recent[k];
                const Lines& l = recentLines[k];
                std::printf("   %s    %s    %s%s%s%s\n", show(r).c_str(), show(o).c_str(), l.irq ? "IRQ " : "    ",
                            l.nmi ? "NMI " : "    ", l.rdy ? "   " : "RDY",
                            (r.addr != o.addr || r.write != o.write || r.data != o.data) ? "   <--" : "");
            }
            std::printf("   chip regs pc=%04x a=%02x x=%02x y=%02x s=%02x p=%02x | ours pc=%04x a=%02x x=%02x y=%02x s=%02x p=%02x t=%d op=%02x\n",
                        readPC(chip), readA(chip), readX(chip), readY(chip), readSP(chip), readP(chip), cpu.regs().pc,
                        cpu.regs().a, cpu.regs().x, cpu.regs().y, cpu.regs().s, cpu.regs().p, cpu.state().t, cpu.state().op);
        }
        if (!ok && probe >= 0)
            std::printf("   probe %02x inputs: p=%02x a=%02x x=%02x operand=%02x\n", probe, probeIn[0], probeIn[1], probeIn[2], probeIn[3]);
        if (!ok) {
            std::printf("   last instructions (our registers before each; chip A at its fetch):\n");
            for (auto& h : history)
                std::printf("     %04x op %02x  a=%02x x=%02x y=%02x s=%02x p=%02x   chip a=%02x\n", h.r.pc, h.op, h.r.a, h.r.x,
                            h.r.y, h.r.s, h.r.p, h.chipA);
        }
        destroyChip(chip);
    }
    std::printf("cpudiff: %d of %d seed(s) differ\n", failures, seeds);
    return failures ? 1 : 0;
}

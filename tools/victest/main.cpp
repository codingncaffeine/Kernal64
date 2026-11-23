// victest: checks the VIC-II against the rules of Bauer's article, one test per rule.
//
// The chip is driven directly, cycle by cycle, with a host that records every memory
// access and answers with a known pattern; register writes are made where the 6510 makes
// them (between the first and the second phase). Expected values come from the article
// (section numbers in each test's name); local-notes/hw/vic2.md lists the rules.
//
//   victest [--only NAME]
//
// Exit status 0 when every test passes.
#include "vic2.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace k64;

namespace {

struct Access { int line, cycle, phase; uint16_t addr; };

struct Host final : VicHost {
    std::vector<Access> log;
    int line = 0, cycle = 0, phase = 1;
    uint8_t mem[0x4000];
    uint8_t color[0x400];
    Host() {
        for (int i = 0; i < 0x4000; ++i) mem[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
        for (int i = 0; i < 0x400; ++i) color[i] = static_cast<uint8_t>(i & 0x0F);
    }
    uint8_t vicRead(uint16_t a, bool) override {
        log.push_back({line, cycle, phase, a});
        return mem[a & 0x3FFF];
    }
    uint8_t colorRead(uint16_t a) override { return color[a & 0x3FF]; }
    bool vicIsCharRom(uint16_t) const override { return false; }   // all RAM
};

struct Rig {
    Host host;
    Vic2 vic;
    uint8_t cpuData = 0xEA;
    // Called between the phases of each cycle: (line, cycle) -> writes to make.
    std::function<void(Rig&, int, int)> onCycle;
    explicit Rig(const Timing& t) : vic(host, t) {}

    void step() {
        host.line = vic.line();
        host.cycle = vic.cycleInLine();
        host.phase = 1;
        vic.phase1();
        if (onCycle) onCycle(*this, vic.line(), vic.cycleInLine());
        host.phase = 2;
        vic.phase2(cpuData);
        vic.render();
        vic.endCycle();
    }
    void runTo(int line, int cycle) {   // run until the chip is at (line, cycle), not yet executed
        for (long n = 0; n < 400000 && !(vic.line() == line && vic.cycleInLine() == cycle); ++n) step();
    }
    void runLine() { const int l = vic.line(); do step(); while (vic.line() == l); }
    // Accesses made in (line, cycle, phase), in order.
    std::vector<uint16_t> at(int line, int cycle, int phase) const {
        std::vector<uint16_t> v;
        for (const auto& a : host.log)
            if (a.line == line && a.cycle == cycle && a.phase == phase) v.push_back(a.addr);
        return v;
    }
    uint8_t px(int line, int x) const {   // the pixel shown at sprite X coordinate x on a line
        for (int p = 0; p < 8 * 65; ++p)
            if (vic.xAt(p) == x) return vic.pixels()[static_cast<size_t>(line) * kMaxRasterWidth + p];
        return 0xFF;
    }
};

int failures = 0, checks = 0;
std::string current;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(cond)) {                                                         \
            ++failures;                                                        \
            std::printf("FAIL %s: ", current.c_str());                         \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
        }                                                                      \
    } while (0)

const Timing* const kModels[] = {&kTimingPal, &kTimingNtsc, &kTimingNtscOld};

// Standard text screen as the KERNAL sets it: DEN, RSEL, YSCROLL 3; CSEL; matrix $0400,
// characters $1000.
void textScreen(Rig& r) {
    r.vic.write(0x11, 0x1B);
    r.vic.write(0x16, 0x08);
    r.vic.write(0x18, 0x14);
    r.vic.write(0x20, 0x0E);
    r.vic.write(0x21, 0x06);
}

// ---- §3.6.1 / §3.4: X coordinates -------------------------------------------------
void testXCoordinates() {
    Rig pal(kTimingPal), r8(kTimingNtsc), r56(kTimingNtscOld);
    auto x = [](const Rig& r, int cycle, int half) { return r.vic.xAt(8 * (cycle - 1) + 4 * half); };
    CHECK(x(pal, 1, 0) == 0x194, "6569 cycle 1 starts at $194, got $%03x", x(pal, 1, 0));
    CHECK(x(pal, 13, 0) == 0x1F4 && x(pal, 13, 1) == 0x000, "6569 X wraps after $1F7 in cycle 13");
    CHECK(x(pal, 21, 0) == 0x03C, "6569 end of cycle 20 = $03C (light pen example, §3.11)");
    CHECK(x(pal, 16, 1) == 0x018, "6569 column 1 at X $018 in cycle 16, second half");
    CHECK(x(pal, 63, 1) == 0x190, "6569 cycle 63 second half $190");
    CHECK(x(r56, 1, 0) == 0x19C && x(r56, 13, 1) == 0x000 && x(r56, 64, 0) == 0x194, "6567R56A X coordinates");
    CHECK(x(r8, 1, 0) == 0x19C && x(r8, 61, 0) == 0x17C && x(r8, 62, 0) == 0x184, "6567R8 X up to cycle 62");
    CHECK(x(r8, 64, 0) == 0x18C && x(r8, 65, 0) == 0x194, "6567R8 cycles 64/65 at $18C/$194");
    // Every X value occurs exactly once per line on the 6569 and 6567R56A (504 / 512 values).
    for (const Rig* r : {&pal, &r56}) {
        std::vector<int> seen(512, 0);
        const int n = 8 * (r == &pal ? 63 : 64);
        for (int p = 0; p < n; ++p) ++seen[static_cast<size_t>(r->vic.xAt(p))];
        int once = 0;
        for (int v : seen) once += v == 1;
        CHECK(once == n, "every X once per line (%d of %d)", once, n);
    }
}

// ---- §3.6.3 / §3.13: first-phase accesses of a line without bad line ----------------
void testAccessSchedule() {
    for (const Timing* t : kModels) {
        Rig r(*t);
        r.vic.write(0x18, 0x14);
        r.runTo(2, 1);
        r.runLine();   // line 2
        const int last = t->cyclesPerLine;
        for (int c = 1; c <= last; ++c) {
            const auto a = r.at(2, c, 1);
            CHECK(a.size() == 1, "%s line 2 cycle %d: one first-phase access, got %zu", t->name, c, a.size());
            if (a.size() != 1) continue;
            uint16_t want = 0x3FFF;   // idle access / idle-state g-access
            if (c >= 11 && c <= 15) want = static_cast<uint16_t>(0x3F00 | (0xFF - 10 - (c - 11)));  // refresh, line 2
            // Sprite p-accesses: 6569 58, 60, 62, 1, 3, 5, 7, 9; 6567R56A 59, 61, 63, 1, 3, 5, 7, 9;
            // 6567R8 59, 61, 63, 65, 2, 4, 6, 8.
            static constexpr int kP[3][8] = {{58, 60, 62, 1, 3, 5, 7, 9}, {59, 61, 63, 1, 3, 5, 7, 9}, {59, 61, 63, 65, 2, 4, 6, 8}};
            const int* p = kP[last == 63 ? 0 : last == 64 ? 1 : 2];
            for (int n = 0; n < 8; ++n)
                if (c == p[n]) want = static_cast<uint16_t>(0x0400 | 0x3F8 | n);
            CHECK(a[0] == want, "%s line 2 cycle %d: access $%04x, expected $%04x", t->name, c, a[0], want);
        }
        CHECK(r.at(2, 30, 2).empty(), "%s: no second-phase access without a bad line", t->name);
    }
    Rig r(kTimingPal);
    r.runTo(0, 11);
    r.step();
    CHECK(r.at(0, 11, 1).size() == 1 && r.at(0, 11, 1)[0] == 0x3FFF, "§3.13: refresh counter is $FF in line 0");
}

// ---- §3.5, §3.7.2 rule 3, §2.4.3: bad line, BA, AEC, c-accesses ------------------------
void testBadLine() {
    for (const Timing* t : kModels) {
        Rig r(*t);
        textScreen(r);
        r.runTo(0x33, 1);
        std::vector<int> ba, aec;
        for (int c = 1; c <= t->cyclesPerLine; ++c) {
            r.step();
            ba.push_back(r.vic.ba() ? 1 : 0);
            aec.push_back(r.vic.aecLow() ? 1 : 0);
        }
        for (int c = 1; c <= t->cyclesPerLine; ++c) {
            const bool wantBaLow = c >= 12 && c <= 54, wantAecLow = c >= 15 && c <= 54;
            CHECK(ba[static_cast<size_t>(c - 1)] == !wantBaLow, "%s line $33 cycle %d: BA %s", t->name, c, wantBaLow ? "low" : "high");
            CHECK(aec[static_cast<size_t>(c - 1)] == wantAecLow, "%s line $33 cycle %d: AEC %s in phase 2", t->name, c, wantAecLow ? "low" : "high");
            const auto p2 = r.at(0x33, c, 2);
            if (c >= 15 && c <= 54) {
                CHECK(p2.size() == 1 && p2[0] == 0x0400 + (c - 15), "%s cycle %d: c-access at $%04x", t->name, c, 0x0400 + (c - 15));
            } else {
                CHECK(p2.empty(), "%s cycle %d: no c-access", t->name, c);
            }
            if (c >= 16 && c <= 55) {
                const auto p1 = r.at(0x33, c, 1);
                const int col = c - 16;
                const uint16_t ptr = r.host.mem[0x0400 + col];
                const uint16_t want = static_cast<uint16_t>(0x1000 | (ptr << 3) | 0);   // RC = 0
                CHECK(p1.size() == 1 && p1[0] == want, "%s cycle %d: g-access $%04x, got $%04x", t->name, c, want, p1.empty() ? 0 : p1[0]);
            }
        }
        // Lines $34-$3A are display lines without bad line: g-accesses with RC 1..7, VC from
        // VCBASE again; line $3B is the next bad line with VC = 40.
        r.runTo(0x34, 1);
        r.runLine();
        {
            const auto p1 = r.at(0x34, 16, 1);
            CHECK(p1.size() == 1 && p1[0] == (0x1000 | (r.host.mem[0x0400] << 3) | 1), "%s line $34: RC=1 g-access", t->name);
            CHECK(r.at(0x34, 20, 2).empty(), "%s line $34: no c-access", t->name);
        }
        r.runTo(0x3B, 1);
        r.runLine();
        {
            const auto p2 = r.at(0x3B, 15, 2);
            CHECK(p2.size() == 1 && p2[0] == 0x0400 + 40, "%s line $3B: c-access from VC = 40", t->name);
        }
        // §3.7.2: the last text row ends with line $FA (RC = 7); from $FB the display is idle
        // and the g-accesses go to $3FFF.
        r.runTo(0xFB, 1);
        r.runLine();
        {
            const auto p1 = r.at(0xFB, 20, 1);
            CHECK(p1.size() == 1 && p1[0] == 0x3FFF, "%s line $FB: idle-state g-access at $3FFF", t->name);
        }
    }
}

// ---- §3.5 / §3.10: DEN must be set in some cycle of line $30 ---------------------------
void testDenInLine30() {
    Rig r(kTimingPal);
    textScreen(r);
    r.vic.write(0x11, 0x0B);   // DEN off
    r.onCycle = [](Rig& rr, int line, int cycle) {
        if (line == 0x31 && cycle == 1) rr.vic.write(0x11, 0x1B);   // on again, after line $30
    };
    r.runTo(0x34, 1);
    CHECK(r.at(0x33, 20, 2).empty(), "no bad line when DEN was off throughout line $30");
    Rig r2(kTimingPal);
    textScreen(r2);
    r2.vic.write(0x11, 0x0B);
    r2.onCycle = [](Rig& rr, int line, int cycle) {
        if (line == 0x30 && cycle == 62) rr.vic.write(0x11, 0x1B);   // set in the last cycles of $30
        if (line == 0x30 && cycle == 63) rr.vic.write(0x11, 0x0B);
        if (line == 0x31 && cycle == 1) rr.vic.write(0x11, 0x1B);
    };
    r2.runTo(0x34, 1);
    CHECK(r2.at(0x33, 20, 2).size() == 1, "bad line when DEN was set for one cycle of line $30");
}

// ---- §3.12: raster interrupt ----------------------------------------------------------
void testRasterIrq() {
    for (const Timing* t : kModels) {
        for (int line : {0x80, 0}) {
            Rig r(*t);
            r.vic.write(0x1A, 0x01);
            r.vic.write(0x11, static_cast<uint8_t>(line & 0x100 ? 0x80 : 0));
            r.vic.write(0x12, static_cast<uint8_t>(line));
            r.runTo(line == 0 ? 1 : 0, 1);   // skip the power-on line 0
            r.runTo(line, 1);
            r.vic.write(0x19, 0x0F);   // the power-on frame may have latched it already
            const int expect = line == 0 ? 2 : 1;
            int got = -1;
            for (int c = 1; c <= 4 && got < 0; ++c) {
                r.step();
                if (r.vic.irq()) got = c;
            }
            CHECK(got == expect, "%s line %d: raster IRQ in cycle %d, got %d", t->name, line, expect, got);
            CHECK((r.vic.peek(0x19) & 0x81) == 0x81, "%s: $D019 shows IRQ and RST", t->name);
            r.vic.write(0x19, 0x01);
            CHECK(!r.vic.irq() && (r.vic.peek(0x19) & 0x81) == 0, "%s: writing 1 clears the latch", t->name);
        }
    }
    // RASTER reads the previous line in cycle 1 of line 0.
    Rig r(kTimingPal);
    r.runTo(1, 1);
    r.runTo(0, 1);
    CHECK(r.vic.peek(0x12) == (311 & 0xFF) && (r.vic.peek(0x11) & 0x80), "RASTER still 311 in cycle 1 of line 0");
    r.step();
    CHECK(r.vic.peek(0x12) == 0 && !(r.vic.peek(0x11) & 0x80), "RASTER 0 in cycle 2 of line 0");
}

// ---- §3.2: registers --------------------------------------------------------------------
void testRegisters() {
    Rig r(kTimingPal);
    for (unsigned reg = 0; reg < 0x2F; ++reg) r.vic.write(reg, 0);
    CHECK(r.vic.peek(0x16) == 0xC0, "$D016 bits 6-7 read 1");
    CHECK(r.vic.peek(0x18) == 0x01, "$D018 bit 0 reads 1");
    CHECK(r.vic.peek(0x19) == 0x70, "$D019 bits 4-6 read 1");
    CHECK(r.vic.peek(0x1A) == 0xF0, "$D01A bits 4-7 read 1");
    for (unsigned reg = 0x20; reg < 0x2F; ++reg) CHECK(r.vic.peek(reg) == 0xF0, "$D0%02X bits 4-7 read 1", reg);
    for (unsigned reg = 0x2F; reg < 0x40; ++reg) {
        r.vic.write(reg, 0);
        CHECK(r.vic.peek(reg) == 0xFF, "$D0%02X unused reads $FF", reg);
    }
    r.vic.write(0x1E, 0x55);
    CHECK(r.vic.peek(0x1E) == 0, "$D01E cannot be written");
    r.vic.write(0x05, 0xA5);
    CHECK(r.vic.read(0x45) == 0xA5, "registers repeat every 64 bytes");
}

// ---- §3.9: border flip-flops and comparators ---------------------------------------------
void testBorder() {
    for (int csel : {0, 1})
        for (int rsel : {0, 1}) {
            Rig r(kTimingPal);
            textScreen(r);
            r.vic.write(0x11, static_cast<uint8_t>(0x13 | (rsel << 3)));
            r.vic.write(0x16, static_cast<uint8_t>(csel << 3));
            r.runTo(1, 1);
            r.runTo(0, 1);   // one full frame
            const int left = csel ? 24 : 31, right = csel ? 343 : 334;
            const int top = rsel ? 51 : 55, bottom = rsel ? 250 : 246;
            const uint8_t ec = 0x0E;
            CHECK(r.px(100, left - 1) == ec && r.px(100, left) != ec, "CSEL=%d: window starts at X %d", csel, left);
            CHECK(r.px(100, right) != ec && r.px(100, right + 1) == ec, "CSEL=%d: window ends at X %d", csel, right);
            CHECK(r.px(top - 1, 100) == ec && r.px(top, 100) != ec, "RSEL=%d: window starts at line %d", rsel, top);
            CHECK(r.px(bottom, 100) != ec && r.px(bottom + 1, 100) == ec, "RSEL=%d: window ends at line %d", rsel, bottom);
        }
    // DEN off: border everywhere.
    Rig r(kTimingPal);
    textScreen(r);
    r.vic.write(0x11, 0x0B);
    r.runTo(1, 1);
    r.runTo(0, 1);
    CHECK(r.px(100, 100) == 0x0E, "DEN=0: border covers the display window");
}

// §3.14.1: opening the side border needs CSEL 1 -> 0 exactly in cycle 56; switching CSEL
// 0 -> 1 in cycle 17 keeps the border closed.
void testSideBorderTricks() {
    for (int writeCycle = 54; writeCycle <= 58; ++writeCycle) {
        Rig r(kTimingPal);
        textScreen(r);
        r.onCycle = [writeCycle](Rig& rr, int line, int cycle) {
            if (line == 100 && cycle == writeCycle) rr.vic.write(0x16, 0x00);
            if (line == 101 && cycle == 1) rr.vic.write(0x16, 0x08);
        };
        r.runTo(102, 1);
        const bool open = r.px(100, 360) != 0x0E;
        CHECK(open == (writeCycle == 56), "CSEL 1->0 in cycle %d: side border %s", writeCycle, open ? "open" : "closed");
    }
    for (int writeCycle = 15; writeCycle <= 19; ++writeCycle) {
        Rig r(kTimingPal);
        textScreen(r);
        r.vic.write(0x16, 0x00);
        r.onCycle = [writeCycle](Rig& rr, int line, int cycle) {
            if (line == 100 && cycle == writeCycle) rr.vic.write(0x16, 0x08);
            if (line == 100 && cycle == 60) rr.vic.write(0x16, 0x00);
        };
        r.runTo(101, 1);
        const bool closed = r.px(100, 200) == 0x0E;
        CHECK(closed == (writeCycle == 17), "CSEL 0->1 in cycle %d: border %s across the line", writeCycle, closed ? "stays" : "opens");
    }
}

// §3.14.1: RSEL 1 -> 0 in lines 248-250 keeps the lower border open.
void testVerticalBorderTrick() {
    for (int writeLine : {246, 247, 248, 250, 251}) {
        Rig r(kTimingPal);
        textScreen(r);
        r.onCycle = [writeLine](Rig& rr, int line, int cycle) {
            if (line == writeLine && cycle == 20) rr.vic.write(0x11, 0x13);
        };
        r.runTo(270, 1);
        const bool open = r.px(260, 100) != 0x0E;
        const bool want = writeLine >= 248 && writeLine <= 250;
        CHECK(open == want, "RSEL 1->0 in line %d: lower border %s", writeLine, open ? "open" : "closed");
    }
}

// ---- §3.7.2 / §3.14: counters under bad-line manipulation ----------------------------------
// FLD: YSCROLL kept away from the line holds the display in idle state.
void testFld() {
    Rig r(kTimingPal);
    textScreen(r);
    r.onCycle = [](Rig& rr, int line, int cycle) {
        if (cycle == 60 && line >= 0x30 && line < 0x50) rr.vic.write(0x11, static_cast<uint8_t>(0x18 | ((line + 2) & 7)));
        if (line == 0x50 && cycle == 60) rr.vic.write(0x11, 0x1B);
    };
    r.runTo(0x54, 1);
    CHECK(r.at(0x33, 20, 2).empty(), "FLD: no bad line at $33");
    CHECK(r.at(0x53, 15, 2).size() == 1 && r.at(0x53, 15, 2)[0] == 0x0400, "FLD: first text line read at $53 from VC 0");
}

// Linecrunch (§3.14.4): bad line condition taken away before cycle 14 -> display, RC stays 7,
// VCBASE advances by 40 at cycle 58.
void testLinecrunch() {
    Rig r(kTimingPal);
    textScreen(r);
    // Line $33 is a bad line; in line $3B (the next one) take the condition away in cycle 13.
    r.onCycle = [](Rig& rr, int line, int cycle) {
        if (line == 0x3B && cycle == 11) rr.vic.write(0x11, 0x1C);
    };
    r.runTo(0x3C, 1);
    r.runTo(0x3C, 20);
    // Line $3C: still display state (RC was 7), and VC continued past 40 -> the next bad line
    // (YSCROLL 4 -> line $3C) reads from VC 80.
    const auto p2 = r.at(0x3C, 15, 2);
    CHECK(p2.size() == 1 && p2[0] == 0x0400 + 80, "linecrunch: next bad line reads from VC 80, got $%04x", p2.empty() ? 0 : p2[0]);
}

// DMA delay (§3.14.6): a bad line created in cycle 30 of an idle line: BA low from 31, the
// first three c-accesses read $FF and the CPU's data nybble, stored from the start of the
// matrix line.
void testDmaDelay() {
    Rig r(kTimingPal);
    textScreen(r);
    r.cpuData = 0x37;
    r.onCycle = [](Rig& rr, int line, int cycle) {
        if (line == 0x32 && cycle == 1) rr.vic.write(0x11, 0x1D);   // YSCROLL 5: no bad line at $33/$34
        if (line == 0x34 && cycle == 30) rr.vic.write(0x11, 0x1C);  // YSCROLL 4 = line $34
    };
    r.runTo(0x34, 30);
    r.step();
    CHECK(r.vic.ba(), "DMA delay: BA still high in the cycle of the write");
    r.step();
    CHECK(!r.vic.ba() && !r.vic.aecLow(), "DMA delay: BA low in cycle 31, AEC high");
    r.step();
    r.step();
    CHECK(!r.vic.aecLow(), "DMA delay: AEC high in cycle 33");
    r.step();
    CHECK(r.vic.aecLow(), "DMA delay: AEC low from cycle 34");
    const auto& m = r.vic.state().matrix;
    CHECK(m[0] == 0x7FF && m[1] == 0x7FF && m[2] == 0x7FF, "DMA delay: first three c-accesses $FF + CPU nybble 7 (%03x %03x %03x)", m[0], m[1], m[2]);
    // VC counts on with each display-state g-access from cycle 32, so the fourth c-access
    // (cycle 34) reads video matrix address 3: the whole line is displaced by 16 columns.
    CHECK((m[3] & 0xFF) == r.host.mem[0x0400 + 3], "DMA delay: fourth c-access is video matrix data from VC 3");
    // The first-phase access of the cycle in which the accesses start comes from $38FF on
    // the 6569 and $3807 on the 8565; the cycle before is a plain idle access (V1).
    for (const Timing* t : {&kTimingPal, &kTimingPalNew}) {
        Rig v(*t);
        textScreen(v);
        v.onCycle = r.onCycle;
        v.runTo(0x34, 32);
        const uint16_t want = t->newChip ? 0x3807 : 0x38FF;
        CHECK(v.at(0x34, 30, 1) == std::vector<uint16_t>{0x3FFF}, "DMA delay (%s): idle access in the cycle of the write", t->name);
        CHECK(v.at(0x34, 31, 1) == std::vector<uint16_t>{want}, "DMA delay (%s): access in the starting cycle from $%04X", t->name, want);
    }
}

// The palette: the first chip revisions have five luma levels, the later ones nine. Black and
// white are the same; the three grays (no chroma) sit on other levels.
void testPalette() {
    const auto& common = vicPalette(false);
    const auto& early = vicPalette(true);
    auto gray = [](uint32_t c) { return ((c >> 16) & 0xFF) == ((c >> 8) & 0xFF) && ((c >> 8) & 0xFF) == (c & 0xFF); };
    CHECK(common[0] == early[0] && common[1] == early[1], "black and white are the same on both");
    for (int i : {0, 1, 11, 12, 15}) CHECK(gray(common[size_t(i)]) && gray(early[size_t(i)]), "color %d has no chroma", i);
    CHECK((early[11] & 0xFF) < (common[11] & 0xFF), "dark gray is darker on the first revision");
    CHECK((early[12] & 0xFF) > (common[12] & 0xFF), "gray is brighter on the first revision");
    CHECK((early[15] & 0xFF) > (common[15] & 0xFF), "light gray is brighter on the first revision");
    // Five levels: every color's green channel order follows its level group (8, 16, 24 of 32).
    CHECK(early[12] == early[12] && (early[15] & 0xFF) > (early[12] & 0xFF) && (early[12] & 0xFF) > (early[11] & 0xFF), "first-revision grays in order");
    int levels = 0;
    uint32_t seen[16];
    for (int i : {0, 1, 11, 12, 15}) {
        bool dup = false;
        for (int k = 0; k < levels; ++k) dup |= seen[k] == early[size_t(i)];
        if (!dup) seen[levels++] = early[size_t(i)];
    }
    CHECK(levels == 5, "black, the three grays and white are five different levels (%d)", levels);
}

// Doubled text lines (§3.14.5): a Bad Line Condition in cycles 54-57 of a row's last line keeps
// the display state; RC wraps to 0 and the row's characters are fetched again.
void testDoubledLine() {
    Rig r(kTimingPal);
    textScreen(r);
    r.onCycle = [](Rig& rr, int line, int cycle) {
        if (line == 0x3A && cycle == 55) rr.vic.write(0x11, 0x1A);   // YSCROLL 2 = line $3A
        if (line == 0x3B && cycle == 1) rr.vic.write(0x11, 0x1D);
    };
    r.runTo(0x3C, 1);
    const auto p1 = r.at(0x3B, 16, 1);
    const uint16_t want = static_cast<uint16_t>(0x1000 | (r.host.mem[0x0400] << 3) | 0);
    CHECK(p1.size() == 1 && p1[0] == want, "line $3B: first row again with RC 0 ($%04x), got $%04x", want, p1.empty() ? 0 : p1[0]);
    CHECK(r.at(0x3B, 20, 2).empty(), "line $3B: no new c-accesses");
}

// §3.9 rules 2-3 are checked in cycle 63: RSEL 1 -> 0 written in cycle 63 of line 247 still
// closes the border (the check sees the new comparator), written in line 248 it does not.
void testVerticalCheckCycle() {
    // The comparison in a cycle sees the registers as the cycle before left them (P1), so
    // the last write that still closes the border in line 247 is the one in cycle 62.
    struct Case { int line, cycle; bool open; };
    for (const Case k : {Case{247, 62, false}, Case{247, 63, true}, Case{248, 1, true}}) {
        Rig r(kTimingPal);
        textScreen(r);
        r.onCycle = [k](Rig& rr, int line, int cycle) {
            if (line == k.line && cycle == k.cycle) rr.vic.write(0x11, 0x13);
        };
        r.runTo(260, 1);
        const bool open = r.px(255, 100) != 0x0E;
        CHECK(open == k.open, "RSEL 1->0 at line %d cycle %d: lower border %s", k.line, k.cycle, open ? "open" : "closed");
    }
}

// §3.7.1: idle-state g-access at $39FF with ECM set.
void testIdleEcm() {
    Rig r(kTimingPal);
    textScreen(r);
    r.vic.write(0x11, 0x5B);
    r.runTo(0x10, 1);
    r.runLine();
    const auto p1 = r.at(0x10, 20, 1);
    CHECK(p1.size() == 1 && p1[0] == 0x39FF, "idle-state g-access with ECM at $39FF");
}

// §3.7.3: the eight modes, displayed pixel colors in one character cell.
void testModes() {
    struct Case { uint8_t d011, d016; uint8_t gbyte, ptr, colorNyb; uint8_t expect[8]; const char* name; };
    // Background colors 6, 1, 2, 3 ($D021-$D024).
    const Case cases[] = {
        {0x1B, 0x08, 0xA5, 0x01, 0x05, {5, 6, 5, 6, 6, 5, 6, 5}, "standard text"},
        {0x1B, 0x18, 0x1B, 0x01, 0x0D, {6, 6, 1, 1, 2, 2, 5, 5}, "multicolor text, MC flag set"},
        {0x1B, 0x18, 0xA5, 0x01, 0x05, {5, 6, 5, 6, 6, 5, 6, 5}, "multicolor text, MC flag clear"},
        {0x5B, 0x08, 0xA5, 0x81, 0x05, {5, 2, 5, 2, 2, 5, 2, 5}, "ECM text, background 2"},
        {0x3B, 0x08, 0xA5, 0x4C, 0x05, {4, 12, 4, 12, 12, 4, 12, 4}, "standard bitmap"},
        {0x3B, 0x18, 0x1B, 0x4C, 0x05, {6, 6, 4, 4, 12, 12, 5, 5}, "multicolor bitmap"},
        {0x7B, 0x08, 0xFF, 0x01, 0x05, {0, 0, 0, 0, 0, 0, 0, 0}, "invalid bitmap 1"},
    };
    for (const auto& k : cases) {
        Rig r(kTimingPal);
        textScreen(r);
        r.vic.write(0x21, 6); r.vic.write(0x22, 1); r.vic.write(0x23, 2); r.vic.write(0x24, 3);
        r.vic.write(0x11, k.d011);
        r.vic.write(0x16, k.d016);
        for (int i = 0; i < 0x4000; ++i) r.host.mem[i] = k.gbyte;
        for (int i = 0; i < 1000; ++i) r.host.mem[0x0400 + i] = k.ptr;
        for (int i = 0; i < 0x400; ++i) r.host.color[i] = k.colorNyb;
        r.runTo(0x34, 1);
        bool ok = true;
        char got[64] = {};
        int n = 0;
        for (int i = 0; i < 8; ++i) {
            const uint8_t p = r.px(0x33, 24 + i);
            n += std::snprintf(got + n, sizeof got - static_cast<size_t>(n), "%d ", p);
            ok &= p == k.expect[i];
        }
        CHECK(ok, "%s: pixels %s", k.name, got);
    }
}

// XSCROLL moves the graphics right inside the window (§3.4).
void testXscroll() {
    for (int xs = 0; xs < 8; ++xs) {
        Rig r(kTimingPal);
        textScreen(r);
        r.vic.write(0x16, static_cast<uint8_t>(0x08 | xs));
        for (int i = 0; i < 0x4000; ++i) r.host.mem[i] = 0x80;   // one pixel per cell
        for (int i = 0; i < 0x400; ++i) r.host.color[i] = 1;
        r.runTo(0x34, 1);
        int first = -1;
        for (int x = 24; x < 60 && first < 0; ++x)
            if (r.px(0x33, x) == 1) first = x;
        CHECK(first == 24 + xs, "XSCROLL %d: first pixel at X %d, got %d", xs, 24 + xs, first);
    }
}

struct Test { const char* name; void (*fn)(); };
const Test kTests[] = {
    {"x-coordinates", testXCoordinates},   {"access-schedule", testAccessSchedule},
    {"bad-line", testBadLine},             {"den-line-30", testDenInLine30},
    {"raster-irq", testRasterIrq},         {"registers", testRegisters},
    {"border", testBorder},                {"side-border-tricks", testSideBorderTricks},
    {"vertical-border-trick", testVerticalBorderTrick}, {"fld", testFld},
    {"linecrunch", testLinecrunch},        {"dma-delay", testDmaDelay},
    {"idle-ecm", testIdleEcm},             {"modes", testModes},
    {"xscroll", testXscroll},              {"doubled-line", testDoubledLine},
    {"vertical-check-cycle", testVerticalCheckCycle}, {"palette", testPalette},
};

} // namespace

int main(int argc, char** argv) {
    const char* only = argc > 2 && !std::strcmp(argv[1], "--only") ? argv[2] : nullptr;
    int ran = 0;
    for (const auto& t : kTests) {
        if (only && std::strcmp(only, t.name)) continue;
        current = t.name;
        const int before = failures;
        t.fn();
        ++ran;
        std::printf("%s %s\n", failures == before ? "pass" : "FAIL", t.name);
    }
    std::printf("victest: %d tests, %d checks, %d failed\n", ran, checks, failures);
    return failures ? 1 : 0;
}

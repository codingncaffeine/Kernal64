#include "vic2.h"

#include <algorithm>
#include <cmath>

namespace k64 {

namespace {
// Border comparators, §3.9: exact match only.
constexpr int kLeft[2] = {31, 24}, kRight[2] = {335, 344};   // CSEL 0 / 1
constexpr int kTop[2] = {55, 51}, kBottom[2] = {247, 251};   // RSEL 0 / 1
} // namespace

Vic2::Vic2(VicHost& host, const Timing& t)
    : host_(host), t_(&t), pixels_(static_cast<size_t>(kMaxRasterWidth) * kMaxRasterHeight, 0) {
    reset();
}

void Vic2::setTiming(const Timing& t) {
    t_ = &t;
    reset();
}

void Vic2::reset() {
    s_ = State{};
    std::fill(pixels_.begin(), pixels_.end(), uint8_t{0});
}

int Vic2::xAt(int px) const {
    int x = t_->firstX + px;
    if (t_->cyclesPerLine == 63) return x >= 0x1F8 ? x - 0x1F8 : x;  // the 6569 counts to $1F7
    // The 6567R8's 65 cycles are 8 pixels more than the 512 X coordinates: X $184-$18B
    // occurs in two cycles (62 and 63). PROVISIONAL: the article's R8 diagram has
    // inconsistent digits here; see hw/vic2.md.
    if (t_->cyclesPerLine == 65 && px >= 8 * 62) x -= 8;
    return x & 0x1FF;
}

// p-access cycle of sprite n (§3.6.3 diagrams): every two cycles from cycle 58 (6569) or
// 59 (both 6567), running on into the next line. On the 65-cycle 6567R8 that puts sprite 3
// in cycle 65 and sprites 4-7 in cycles 2, 4, 6, 8 (the suite's phi1timing program, which
// matches a real 6567R8).
int Vic2::spritePCycle(int n) const {
    const int first = t_->cyclesPerLine == 63 ? 58 : 59;
    return (first + 2 * n - 1) % t_->cyclesPerLine + 1;
}

// g-access address in display state, §3.7.3, for a given mode register value: BMM picks
// counter or character addressing, ECM forces address bits 9-10 low.
uint16_t Vic2::gAddress(uint8_t r11) const {
    const uint8_t r18 = s_.reg[0x18];
    uint16_t a;
    if (r11 & 0x20) a = static_cast<uint16_t>(((r18 & 0x08) << 10) | (s_.vc << 3) | s_.rc);
    else a = static_cast<uint16_t>(((r18 & 0x0E) << 10) | ((s_.matrix[s_.vmli] & 0xFF) << 3) | s_.rc);
    if (r11 & 0x40) a &= 0x39FF;
    return a;
}

// Latches the light pen position once per frame (§3.11): half the X coordinate of this
// cycle's start (6567/6569); a line still held low when the frame starts latches a fixed X
// in line 0. Nothing is latched in the last line after its first cycle, but the frame's one
// latch is used up all the same (reference behavior; suite lightpen programs).
void Vic2::latchLightPen(bool held) {
    if (s_.lpTriggered) return;
    s_.lpTriggered = 1;
    const int last = t_->linesPerFrame - 1;
    const int raster = (s_.line == 0 && s_.cycle == 1) ? last : s_.line;
    if (raster == last && s_.cycle > 1) return;
    const int x = held ? (t_->cyclesPerLine == 65 ? 0xD5 : 0xD1) : xAt(8 * (s_.cycle - 1)) / 2;
    s_.lpx = static_cast<uint8_t>(x);
    s_.lpy = static_cast<uint8_t>(raster);
    s_.irqLatch |= 0x08;
}

void Vic2::phase1() {
    const int c = s_.cycle, L = s_.line;
    const uint8_t r11 = s_.reg[0x11];

    if (L == 0 && c == 1) {
        s_.ref = 0xFF;      // §3.13
        s_.vcBase = 0;      // §3.7.2 rule 1 (somewhere outside lines $30-$F7)
        s_.denSeen = 0;
    }
    // DEN set in any cycle of line $30 allows bad lines (§3.5). The registers here are as
    // the last cycle's access left them, so the line's last cycle is seen in the next line.
    if ((L == 0x30 || (L == 0x31 && c == 1)) && (r11 & 0x10)) s_.denSeen = 1;
    s_.badLine = L >= 0x30 && L <= 0xF7 && (L & 7) == (r11 & 7) && s_.denSeen;

    // Raster interrupt, §3.12: the compare runs in every cycle and fires on the change from
    // no match to match, so it fires at the start of the line (line 0 one cycle later, when
    // RASTER changes) and also when the compare value is written to the current line
    // (reference emulator; hw/vic2.md rule 12).
    const int raster = (L == 0 && c == 1) ? t_->linesPerFrame - 1 : L;
    const bool match = raster == s_.rasterCompare;
    if (match && !s_.rasterMatched) s_.irqLatch |= 0x01;
    s_.rasterMatched = match;

    // Vertical border flip-flop (§3.9 rules 2-5 as the reference applies them, P1): the top
    // comparator, with DEN, clears it in any cycle of the line; a bottom match in any cycle
    // is held and takes effect in cycle 1 or at the left comparator (render).
    {
        const int rsel = (r11 >> 3) & 1;
        if (L == kTop[rsel] && (r11 & 0x10)) s_.vertBorder = s_.vertPending = 0;
        if (L == kBottom[rsel]) s_.vertPending = 1;
        if (c == 1) s_.vertBorder = s_.vertPending;
    }

    if (c == 14) {          // rule 2
        s_.vc = s_.vcBase;
        s_.vmli = 0;
        if (s_.badLine) s_.rc = 0;
    }

    // The first-phase access, §3.6.3.
    uint8_t byte;
    if (c >= 11 && c <= 15) {
        byte = host_.vicRead(static_cast<uint16_t>(0x3F00 | s_.ref));
        s_.ref = static_cast<uint8_t>(s_.ref - 1);
    } else if (c >= 16 && c <= 55) {
        if (s_.display) {
            // P10: BMM acts if it is set now or was a cycle ago. When it has just changed
            // and the address moves from RAM into the character ROM, the low byte is still
            // the old mode's.
            const uint8_t prev = s_.reg11Prev;
            uint16_t a = gAddress(static_cast<uint8_t>(r11 | (prev & 0x20)));
            if ((r11 ^ prev) & 0x20) {
                const uint16_t from = gAddress(prev), to = gAddress(r11);
                if (!host_.vicIsCharRom(from) && host_.vicIsCharRom(to))
                    a = static_cast<uint16_t>((from & 0x00FF) | (to & 0x3F00));
            }
            byte = host_.vicRead(a);
            s_.vc = (s_.vc + 1) & 0x3FF;    // rule 4
            s_.vmli = static_cast<uint8_t>((s_.vmli + 1) % 40);
        } else {
            byte = host_.vicRead((r11 & 0x40) ? 0x39FF : 0x3FFF);   // idle state, §3.7.1
        }
        s_.gbuf = byte;
    } else {
        int sprite = -1;
        for (int n = 0; n < 8; ++n)
            if (spritePCycle(n) == c) sprite = n;
        if (sprite >= 0) {
            const uint16_t vm = static_cast<uint16_t>((s_.reg[0x18] & 0xF0) << 6);
            byte = host_.vicRead(static_cast<uint16_t>(vm | 0x3F8 | sprite));
            s_.spritePtr[sprite] = byte;
        } else {
            byte = host_.vicRead(0x3FFF);   // idle access (also the unused s-access slots)
        }
    }
    s_.lastByte = byte;

    if (c == 58) {          // rule 5
        if (s_.rc == 7) {
            s_.display = 0;
            s_.vcBase = s_.vc;
        }
        if (s_.badLine) s_.display = 1;
        if (s_.display) s_.rc = (s_.rc + 1) & 7;
    }
    if (s_.badLine) s_.display = 1;   // idle -> display on any Bad Line Condition, §3.7.1

    // BA and AEC, §2.4.3 / §3.7.2 rule 3: AEC stays low in the second phase from the fourth
    // cycle of BA low.
    const bool baLow = c >= 12 && c <= 54 && s_.badLine;
    s_.baLow = baLow;
    s_.baRun = baLow ? static_cast<uint8_t>(std::min(s_.baRun + 1, 4)) : 0;
    s_.aecLow = baLow && s_.baRun >= 4;
    s_.reg11Prev = r11;

    // Light pen: a new frame allows a new latch (taken at once if the line is still low);
    // a latch asked for by the last cycle's access happens now.
    if (L == 0 && c == 2) {
        s_.lpTriggered = 0;
        if (s_.lpLow) latchLightPen(true);
    }
    if (s_.lpPending) {
        s_.lpPending = 0;
        latchLightPen(false);
    }
}

void Vic2::phase2(uint8_t cpuData) {
    const int c = s_.cycle;
    if (c < 15 || c > 54 || !s_.badLine) return;
    // c-access. With AEC still high the VIC-II's data drivers are closed: it reads $FF, and
    // as color the low nybble of the CPU's data bus (§3.14.6).
    uint16_t v;
    if (s_.aecLow) {
        const uint16_t a = static_cast<uint16_t>(((s_.reg[0x18] & 0xF0) << 6) | s_.vc);
        v = static_cast<uint16_t>(((host_.colorRead(s_.vc) & 0x0F) << 8) | host_.vicRead(a));
    } else {
        v = static_cast<uint16_t>(((cpuData & 0x0F) << 8) | 0xFF);
    }
    s_.matrix[s_.vmli] = v;
}

namespace {
// A pixel on its way to the output: a color 0-15, or a reference to a color register.
constexpr uint8_t kRefBorder = 0x20, kRefBg0 = 0x21;
} // namespace

// One pixel of the graphics data path (hw/vic2.md P4, P6, P7).
uint8_t Vic2::graphicsPixel(int i) {
    if (i == s_.xscrollPipe) {
        s_.shData = s_.gPipe;
        s_.shC = s_.cPipe1;
        s_.mcFlop = 1;
    }
    // Pixel forming.
    const bool wide = (s_.mode11 & 0x20) || (s_.shC & 0x0800);   // BMM, or color bit 3
    if (s_.mode16b) {
        if (!wide) s_.pixelReg = (s_.shData & 0x80) ? 3 : 0;
        else if (s_.mcFlop) s_.pixelReg = s_.shData >> 6;
    } else {
        s_.pixelReg = (s_.shData & 0x80) ? (wide ? 2 : 3) : 0;
    }
    const unsigned v = s_.pixelReg;
    s_.shData = static_cast<uint8_t>(s_.shData << 1);
    s_.mcFlop ^= 1;
    // Color decode.
    const uint16_t cd = s_.shC;
    const uint8_t color = (cd >> 8) & 0x0F, hi = (cd >> 4) & 0x0F, lo = cd & 0x0F;
    switch (((s_.mode11 & 0x60) >> 4) | (s_.mode16 >> 4)) {   // ECM, BMM, MCM
    case 0: return v >= 2 ? color : kRefBg0;                                              // text
    case 1: return v == 3 ? static_cast<uint8_t>(color & 7) : static_cast<uint8_t>(kRefBg0 + v);   // multicolor text
    case 2: return v >= 2 ? hi : lo;                                                      // bitmap
    case 3: return v == 0 ? kRefBg0 : v == 1 ? hi : v == 2 ? lo : color;                  // multicolor bitmap
    case 4: return v >= 2 ? color : static_cast<uint8_t>(kRefBg0 + ((cd >> 6) & 3));      // ECM text
    default: return 0;                                                                    // invalid modes
    }
}

void Vic2::render() {
    const int c = s_.cycle;
    const uint8_t r11 = s_.reg[0x11], r16 = s_.reg[0x16];
    const int csel = (r16 >> 3) & 1, rsel = (r11 >> 3) & 1;

    // Side border comparators of the coming cycle (P2).
    const bool borderBefore = s_.mainBorder;
    const int next = c == t_->cyclesPerLine ? 1 : c + 1;
    if (next == (csel ? 17 : 18)) {
        if (s_.line == kBottom[rsel]) s_.vertPending = 1;
        s_.vertBorder = s_.vertPending;
        if (!s_.vertBorder) s_.mainBorder = 0;
    }
    if (next == (csel ? 57 : 56)) s_.mainBorder = 1;

    // Eight pixels of graphics; the mode bits arrive part-way through (P5).
    uint8_t px[8];
    const uint8_t mode11 = r11 & 0x60, mcm = r16 & 0x10;
    for (int i = 0; i < 8; ++i) {
        if (i == 4) {
            s_.mode16 = mcm;
            s_.mode11 |= mode11;
        } else if (i == 6) {
            s_.mode11 &= mode11;
        } else if (i == 7) {
            if (s_.mode16 && !s_.mode16b) s_.mcFlop = 0;
            s_.mode16b = s_.mode16;
        }
        px[i] = graphicsPixel(i);
    }

    // Border (P3).
    if (borderBefore || s_.mainBorder) {
        const int last = csel ? 8 : 7;
        if (borderBefore)
            for (int i = 0; i < last; ++i) px[i] = kRefBorder;
        if (!csel && s_.mainBorder) px[7] = kRefBorder;
    }

    // Output stage (P8): 8 pixels behind, register references resolved one pixel ahead.
    if (s_.colorLatchReg != 0xFF) s_.creg[s_.colorLatchReg & 0x0F] = s_.colorLatchVal;
    // Position: 12 pixels before the start of this cycle (P9). The first 12 pixels of a
    // line fall before cycle 1, in horizontal blanking; they are kept at the end of this
    // line's row, where the reference's whole-raster pictures have them.
    const int width = 8 * t_->cyclesPerLine;
    uint8_t* row = &pixels_[static_cast<size_t>(s_.line) * kMaxRasterWidth];
    int at = 8 * (c - 1) - 12;
    for (int i = 0; i < 8; ++i, ++at) {
        uint8_t& ahead = s_.ring[(i + 1) & 7];
        if (ahead >= 0x20) ahead = s_.creg[ahead & 0x0F];
        row[at < 0 ? at + width : at] = s_.ring[i] & 0x0F;
        s_.ring[i] = px[i];
    }
    s_.colorLatchReg = s_.colorPendReg;
    s_.colorLatchVal = s_.colorPendVal;
    s_.colorPendReg = 0xFF;

    // What enters the data path for the next steps (P4).
    s_.gPipe = s_.gPipeValid ? s_.gbuf : 0;
    s_.cPipe1 = s_.cPipe0;
    if (c >= 15 && c <= 54 && !s_.vertBorder) {
        s_.gPipeValid = 1;
        s_.xscrollPipe = r16 & 7;
        s_.cPipe0 = s_.display ? s_.matrix[s_.dmli < 40 ? s_.dmli++ : 39] : 0;
    } else {
        s_.gPipeValid = 0;
        s_.dmli = 0;
    }
}

void Vic2::endCycle() {
    if (++s_.cycle > t_->cyclesPerLine) {
        s_.cycle = 1;
        if (++s_.line == t_->linesPerFrame) s_.line = 0;
    }
}

// ---- registers, §3.2 -------------------------------------------------------------

uint8_t Vic2::peek(unsigned r) const {
    r &= 0x3F;
    // RASTER: in line 0 the counter is reset one cycle late (§3.6.3).
    const int raster = (s_.line == 0 && s_.cycle == 1) ? t_->linesPerFrame - 1 : s_.line;
    switch (r) {
    case 0x11: return static_cast<uint8_t>((s_.reg[0x11] & 0x7F) | ((raster & 0x100) ? 0x80 : 0));
    case 0x12: return static_cast<uint8_t>(raster & 0xFF);
    case 0x13: return s_.lpx;
    case 0x14: return s_.lpy;
    case 0x16: return static_cast<uint8_t>(s_.reg[0x16] | 0xC0);
    case 0x18: return static_cast<uint8_t>(s_.reg[0x18] | 0x01);
    case 0x19: return static_cast<uint8_t>(0x70 | (s_.irqLatch & 0x0F) | (irq() ? 0x80 : 0));
    case 0x1A: return static_cast<uint8_t>(0xF0 | s_.irqEnable);
    case 0x1E: return s_.collSprite;
    case 0x1F: return s_.collData;
    default:
        if (r >= 0x2F) return 0xFF;
        if (r >= 0x20) return static_cast<uint8_t>(0xF0 | s_.reg[r]);
        return s_.reg[r];
    }
}

uint8_t Vic2::read(unsigned r) {
    r &= 0x3F;
    const uint8_t v = peek(r);
    if (r == 0x1E) s_.collSprite = 0;
    if (r == 0x1F) s_.collData = 0;
    return v;
}

void Vic2::write(unsigned r, uint8_t v) {
    r &= 0x3F;
    switch (r) {
    case 0x11:
        s_.reg[r] = v;
        s_.rasterCompare = static_cast<uint16_t>((s_.rasterCompare & 0xFF) | ((v & 0x80) << 1));
        return;
    case 0x12: s_.rasterCompare = static_cast<uint16_t>((s_.rasterCompare & 0x100) | v); return;
    case 0x13: case 0x14: return;               // light pen latches are read-only
    case 0x19: s_.irqLatch = static_cast<uint8_t>(s_.irqLatch & ~v & 0x0F); return;
    case 0x1A: s_.irqEnable = v & 0x0F; return;
    case 0x1E: case 0x1F: return;               // collision registers cannot be written
    default:
        if (r < 0x2F) s_.reg[r] = v;
        if (r >= 0x20 && r < 0x2F) {   // color registers reach the output stage later (P8)
            // (One write per cycle on the machine; a test rig writing several at once must
            // not lose one.)
            if (s_.colorPendReg != 0xFF) s_.creg[s_.colorPendReg & 0x0F] = s_.colorPendVal;
            s_.colorPendReg = static_cast<uint8_t>(r - 0x20);
            s_.colorPendVal = v & 0x0F;
        }
        return;
    }
}

// ---- palette ----------------------------------------------------------------------
//
// Timmermann, "Colors" (pepto.de/projects/colorvic, v1 2017): luma levels of the later chip
// revisions (8 x level on a 0-256 scale), chroma on a 16-step wheel offset by half a step,
// one saturation for all colored entries; YUV to RGB; PAL gamma 2.8 re-encoded for sRGB 2.2.
// Defaults of the article: brightness 50, contrast 100, saturation 50, screen 1/5.

const std::array<uint32_t, 16>& vicPalette() {
    static const std::array<uint32_t, 16> pal = [] {
        constexpr int level[16] = {0, 32, 10, 20, 12, 16, 8, 24, 12, 8, 16, 10, 15, 24, 15, 20};
        constexpr int angle[16] = {-1, -1, 4, 12, 2, 10, 15, 7, 5, 6, 4, -1, -1, 10, 15, -1};
        constexpr double pi = 3.14159265358979323846;
        constexpr double screen = 1.0 / 5.0;
        const double brightness = 50 - 50, contrast = 100 / 100.0, saturation = 50 * (1 - screen);
        std::array<uint32_t, 16> out{};
        for (int i = 0; i < 16; ++i) {
            double u = 0, v = 0;
            if (angle[i] >= 0) {
                const double a = (360.0 / 32 + angle[i] * 360.0 / 16) * pi / 180;
                u = std::cos(a) * saturation;
                v = std::sin(a) * saturation;
            }
            double y = 8.0 * level[i] + brightness;
            y *= contrast + screen;
            u *= contrast + screen;
            v *= contrast + screen;
            const double rgb[3] = {y + 1.140 * v, y - 0.396 * u - 0.581 * v, y + 2.029 * u};
            uint32_t px = 0;
            for (double ch : rgb) {
                ch = std::clamp(ch, 0.0, 255.0);
                ch = std::pow(255.0, 1 - 2.8) * std::pow(ch, 2.8);
                ch = std::pow(255.0, 1 - 1 / 2.2) * std::pow(ch, 1 / 2.2);
                px = (px << 8) | static_cast<uint32_t>(std::lround(ch));
            }
            out[static_cast<size_t>(i)] = px;
        }
        return out;
    }();
    return pal;
}

} // namespace k64

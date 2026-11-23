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

// p-access cycle of sprite n (§3.6.3 diagrams): sprites 3-7 in cycles 1, 3, 5, 7, 9 on every
// model; sprites 0-2 two cycles apart from cycle 58 (6569), 59 (6567R56A), 60 (6567R8).
int Vic2::spritePCycle(int n) const {
    if (n >= 3) return 1 + 2 * (n - 3);
    return t_->cyclesPerLine - 5 + 2 * n;
}

// g-access address, §3.7.3: by display/idle state, BMM, and ECM forcing bits 9-10 low.
uint16_t Vic2::gAddress() const {
    const uint8_t r11 = s_.reg[0x11], r18 = s_.reg[0x18];
    uint16_t a;
    if (!s_.display) {
        a = 0x3FFF;
    } else if (r11 & 0x20) {
        a = static_cast<uint16_t>(((r18 & 0x08) << 10) | (s_.vc << 3) | s_.rc);
    } else {
        a = static_cast<uint16_t>(((r18 & 0x0E) << 10) | ((s_.matrix[s_.vmli] & 0xFF) << 3) | s_.rc);
    }
    if (r11 & 0x40) a &= 0x39FF;
    return a;
}

void Vic2::phase1() {
    const int c = s_.cycle, L = s_.line;
    const uint8_t r11 = s_.reg[0x11];

    if (L == 0 && c == 1) {
        s_.ref = 0xFF;      // §3.13
        s_.vcBase = 0;      // §3.7.2 rule 1 (somewhere outside lines $30-$F7)
        s_.denSeen = 0;
    }
    if (L == 0x30 && (r11 & 0x10)) s_.denSeen = 1;
    s_.badLine = L >= 0x30 && L <= 0xF7 && (L & 7) == (r11 & 7) && s_.denSeen;

    // Raster interrupt, §3.12: tested at the start of each line, line 0 one cycle later.
    if ((L == 0 ? c == 2 : c == 1) && L == s_.rasterCompare) s_.irqLatch |= 0x01;

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
        byte = host_.vicRead(gAddress());
        const int xscroll = s_.reg[0x16] & 7;
        auto& slot = s_.pend[s_.pend[0].valid ? 1 : 0];
        slot.at = static_cast<int16_t>(8 * (c - 1) + 4 + xscroll);
        slot.data = byte;
        slot.cdata = s_.display ? s_.matrix[s_.vmli] : 0;
        slot.valid = 1;
        if (s_.display) {   // rule 4
            s_.vc = (s_.vc + 1) & 0x3FF;
            s_.vmli = static_cast<uint8_t>((s_.vmli + 1) % 40);
        }
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

// One pixel: graphics sequencer (§3.7.3), border unit (§3.9).
void Vic2::pixel(int px, int x, uint8_t* out) {
    for (auto& p : s_.pend)
        if (p.valid && p.at == px) {
            s_.shData = p.data;
            s_.shC = p.cdata;
            s_.shPixel = 0;
            p.valid = 0;
        }

    const uint8_t r11 = s_.reg[0x11], r16 = s_.reg[0x16];
    const uint8_t* bg = &s_.reg[0x21];
    uint8_t color = bg[0] & 0x0F;
    if (s_.shPixel < 8) {
        const int i = s_.shPixel++;
        const bool bit = (s_.shData >> (7 - i)) & 1;
        const unsigned pair = (s_.shData >> (6 - (i & 6))) & 3;
        const uint16_t cd = s_.shC;
        switch (((r11 & 0x60) >> 4) | ((r16 & 0x10) >> 4)) {
        case 0: color = bit ? (cd >> 8) & 0x0F : bg[0] & 0x0F; break;            // standard text
        case 1:                                                                    // multicolor text
            if (cd & 0x800) color = pair == 3 ? (cd >> 8) & 0x07 : bg[pair] & 0x0F;
            else color = bit ? (cd >> 8) & 0x07 : bg[0] & 0x0F;
            break;
        case 2: color = bit ? (cd >> 4) & 0x0F : cd & 0x0F; break;               // standard bitmap
        case 3:                                                                    // multicolor bitmap
            color = pair == 0 ? bg[0] & 0x0F : pair == 1 ? (cd >> 4) & 0x0F : pair == 2 ? cd & 0x0F : (cd >> 8) & 0x0F;
            break;
        case 4: color = bit ? (cd >> 8) & 0x0F : bg[(cd >> 6) & 3] & 0x0F; break; // ECM text
        default: color = 0; break;                                                 // invalid modes
        }
    }

    const int csel = (r16 >> 3) & 1, rsel = (r11 >> 3) & 1;
    if (x == kRight[csel]) s_.mainBorder = 1;                                      // rule 1
    if (x == kLeft[csel]) {
        if (s_.line == kBottom[rsel]) s_.vertBorder = 1;                          // rule 4
        if (s_.line == kTop[rsel] && (r11 & 0x10)) s_.vertBorder = 0;            // rule 5
        if (!s_.vertBorder) s_.mainBorder = 0;                                    // rule 6
    }
    if (s_.vertBorder) color = bg[0] & 0x0F;   // the sequencer's output is off
    *out = s_.mainBorder ? (s_.reg[0x20] & 0x0F) : color;
}

void Vic2::render() {
    const int c = s_.cycle;
    if (c == 63) {   // rules 2 and 3 (PROVISIONAL on the NTSC chips: literally cycle 63)
        const uint8_t r11 = s_.reg[0x11];
        const int rsel = (r11 >> 3) & 1;
        if (s_.line == kBottom[rsel]) s_.vertBorder = 1;
        if (s_.line == kTop[rsel] && (r11 & 0x10)) s_.vertBorder = 0;
    }
    uint8_t* row = &pixels_[static_cast<size_t>(s_.line) * kMaxRasterWidth];
    const int px0 = 8 * (c - 1);
    for (int p = 0; p < 8; ++p) pixel(px0 + p, xAt(px0 + p), &row[px0 + p]);
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

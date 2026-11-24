// Epson RTC-72421. Rules: Epson application manual (header).
#include "rtc72421.h"

namespace k64 {

namespace {
enum : unsigned { S1, S10, MI1, MI10, H1, H10, D1, D10, MO1, MO10, Y1, Y10, W };
// Bits that exist in each digit register ("*" bits read 0, writes have no effect: p.10).
constexpr uint8_t kMask[13] = {0xF, 0x7, 0xF, 0x7, 0xF, 0x7, 0xF, 0x3, 0xF, 0x1, 0xF, 0xF, 0x7};
int two(const uint8_t* r, unsigned lo) { return r[lo] + 10 * r[lo + 1]; }
void put(uint8_t* r, unsigned lo, int v) { r[lo] = static_cast<uint8_t>(v % 10); r[lo + 1] = static_cast<uint8_t>(v / 10); }
int daysIn(int month, int year) {
    static constexpr int kDays[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    // Leap years are told apart from the two year digits: every fourth (p.11).
    return month == 2 && year % 4 == 0 ? 29 : (month >= 1 && month <= 12 ? kDays[month] : 31);
}
} // namespace

void Rtc72421::setTime(const Time& t) {
    put(s_.r, S1, t.second);
    put(s_.r, MI1, t.minute);
    put(s_.r, D1, t.day);
    put(s_.r, MO1, t.month);
    put(s_.r, Y1, t.year % 100);
    s_.r[W] = static_cast<uint8_t>(t.weekday % 7);
    s_.cf = 0x04;   // 24-hour clock
    put(s_.r, H1, t.hour);
    s_.sub = 0;
}

uint8_t Rtc72421::read(unsigned reg) const {
    reg &= 15;
    if (reg < 13) return s_.r[reg] & kMask[reg];
    if (reg == 13) {
        // BUSY is always 1 while HOLD is 0 (p.12); with HOLD set the counters stand still.
        const uint8_t busy = (s_.cd & 1) ? 0 : 2;
        return static_cast<uint8_t>((s_.cd & 0x0D) | busy);
    }
    return reg == 14 ? s_.ce : s_.cf;
}

void Rtc72421::write(unsigned reg, uint8_t v) {
    reg &= 15;
    v &= 0x0F;
    if (reg < 13) { s_.r[reg] = v & kMask[reg]; return; }
    if (reg == 13) {
        const bool holdWas = s_.cd & 1;
        // IRQ FLAG: writing 0 clears it (and ends a pulse); writing 1 has no effect (p.10).
        uint8_t irq = s_.cd & 4;
        if (!(v & 4)) { irq = 0; s_.stdpLow = 0; }
        s_.cd = static_cast<uint8_t>((v & 1) | irq);
        // 30-second adjust: to the nearest minute (seconds 30-59 carry into the minute).
        if (v & 8) {
            if (two(s_.r, S1) >= 30) { put(s_.r, S1, 59); incrementSecond(); }
            else put(s_.r, S1, 0);
            s_.sub = 0;
        }
        if (holdWas && !(v & 1) && s_.heldIncrement) { s_.heldIncrement = 0; incrementSecond(); }
        return;
    }
    if (reg == 14) { s_.ce = v; s_.periodSub = 0; return; }
    // F: TEST, 24/12, STOP, RESET. RESET clears the part below one second.
    if (v & 1) s_.sub = 0;
    if ((v ^ s_.cf) & 4) {
        // Switching between 12 and 24 hours: the manual has the user set the hour again (p.15).
    }
    s_.cf = v;
}

uint32_t Rtc72421::periodCycles() const {
    switch ((s_.ce >> 2) & 3) {
    case 0: return cps_ / 64;
    case 1: return cps_;
    case 2: return cps_ * 60;
    default: return cps_ * 3600;
    }
}

void Rtc72421::tick() {
    if (s_.cf & 2) return;   // STOP: the count below one second stands still
    if (++s_.sub >= cps_) {
        s_.sub = 0;
        if (s_.cf & 1) { /* RESET held: the second never completes */ }
        else if (s_.cd & 1) s_.heldIncrement = 1;   // HOLD puts it off, once (p.12)
        else incrementSecond();
    }
    // Fixed-period output (p.10): MASK (E bit 0) off; interrupt mode (bit 1) latches the flag,
    // pulse mode lowers STD.P for a moment and the flag follows it.
    if (!(s_.ce & 1) && ++s_.periodSub >= periodCycles()) {
        s_.periodSub = 0;
        s_.cd |= 4;
        s_.stdpLow = 1;
    } else if (!(s_.ce & 2) && s_.stdpLow && s_.periodSub >= cps_ / 128) {
        s_.stdpLow = 0;   // pulse mode: the pulse ends by itself (its length: no figure; 1/128 s here)
        s_.cd &= static_cast<uint8_t>(~4);
    }
}

void Rtc72421::incrementSecond() {
    uint8_t* r = s_.r;
    int s = two(r, S1) + 1;
    if (s < 60) { put(r, S1, s); return; }
    put(r, S1, 0);
    int m = two(r, MI1) + 1;
    if (m < 60) { put(r, MI1, m); return; }
    put(r, MI1, 0);
    // Hours: 24-hour 0-23; 12-hour 12, 1..11 with PM/AM in H10 bit 2.
    if (twentyFour()) {
        int h = r[H1] + 10 * (r[H10] & 3) + 1;
        if (h < 24) { r[H1] = static_cast<uint8_t>(h % 10); r[H10] = static_cast<uint8_t>(h / 10); return; }
        r[H1] = 0; r[H10] = 0;
    } else {
        const bool pm = r[H10] & 4;
        int h = r[H1] + 10 * (r[H10] & 1) + 1;
        if (h == 12) {   // 11:59 -> 12:00 flips AM/PM; only the PM -> AM flip starts a new day
            r[H1] = 2; r[H10] = static_cast<uint8_t>(1 | (pm ? 0 : 4));
            if (!pm) return;
        } else {
            if (h == 13) h = 1;
            r[H1] = static_cast<uint8_t>(h % 10); r[H10] = static_cast<uint8_t>((h / 10) | (pm ? 4 : 0));
            return;
        }
    }
    r[W] = static_cast<uint8_t>((r[W] + 1) % 7);
    const int year = two(r, Y1), month = two(r, MO1);
    int d = two(r, D1) + 1;
    if (d <= daysIn(month, year)) { put(r, D1, d); return; }
    put(r, D1, 1);
    int mo = month + 1;
    if (mo <= 12) { put(r, MO1, mo); return; }
    put(r, MO1, 1);
    put(r, Y1, (year + 1) % 100);
}

} // namespace k64

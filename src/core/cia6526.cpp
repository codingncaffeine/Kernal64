#include "cia6526.h"

namespace k64 {

namespace {
// The cycle pipeline (hw/cia.md rules 9-13). Each bit is one stage of a delayed event; at
// every clock all stages move up by one and the "0" stages are refilled from `feed`.
//   count:     a count for timer A (bits 0-3) / B (bits 4-7) in stage 0..3;
//              the counter is decremented when the count reaches stage 3
//   load:      force load in stage 0/1 (A: 8-9, B: 10-11); the counter loads at stage 1
//   pulse end: PB6 (12-13) / PB7 (14-15) pulse output drops at stage 1
//   interrupt: 16-17; the interrupt line goes low at stage 1
//   one-shot:  18 (A) / 19 (B): one-shot mode was set in the previous clock
constexpr uint32_t count(int t, int stage) { return 1u << (4 * t + stage); }
constexpr uint32_t load(int t, int stage) { return 1u << (8 + 2 * t + stage); }
constexpr uint32_t pulseEnd(int t, int stage) { return 1u << (12 + 2 * t + stage); }
constexpr uint32_t kInt0 = 1u << 16, kInt1 = 1u << 17;
constexpr uint32_t oneShot(int t) { return 1u << (18 + t); }
// Stages that only ever come from `feed` (or from a register write), never from the shift.
constexpr uint32_t kStage0 = count(0, 0) | count(1, 0) | load(0, 0) | load(1, 0) | pulseEnd(0, 0) |
                             pulseEnd(1, 0) | kInt0 | oneShot(0) | oneShot(1);
constexpr uint32_t kShiftMask = ~(kStage0 | (1u << 20));

// Serial port pipeline (hw/cia.md rule 7a): events a few clocks ahead, one bit per stage,
// moving one stage per clock; stage 0 is when it happens.
//   toggle CNT (stages 2..0, -1), toggle suppressed (2..0, -1), ICR bit 3 (3..0),
//   CNT output history (0..3), shifter load from SDR (3..0).
constexpr uint32_t kTog2 = 1u << 0, kTog1 = 1u << 1, kTog0 = 1u << 2;
constexpr uint32_t kTogAll = 0xFu;
constexpr uint32_t kNog1 = 1u << 5, kNog0 = 1u << 6;
constexpr uint32_t kNogAll = 0xF0u;
constexpr uint32_t kIrq2 = 1u << 9, kIrq0 = 1u << 11;
constexpr uint32_t kCnt0 = 1u << 12, kCnt1 = 1u << 13, kCnt2 = 1u << 14;
constexpr uint32_t kCntAll = 0xF000u;
constexpr uint32_t kSet1 = 1u << 18, kSet0 = 1u << 19;
// Bits that would receive the previous group's last stage when shifting.
constexpr uint32_t kSerialClear = (1u << 4) | (1u << 8) | kCnt0 | (1u << 16) | (1u << 20);
} // namespace

void Cia6526::reset() {
    s_ = State{};
    s_.serial = kCntAll;   // CNT has been high for a while
}

uint8_t Cia6526::portAOut() const { return static_cast<uint8_t>(s_.pra | ~s_.ddra); }

uint8_t Cia6526::portBOut() const {
    uint8_t v = static_cast<uint8_t>(s_.prb | ~s_.ddrb);
    for (int t = 0; t < 2; ++t) {
        if (!(s_.cr[t] & 0x02)) continue;   // PB on: the timer drives PB6 (A) / PB7 (B)
        const uint8_t bit = static_cast<uint8_t>(0x40 << t);
        const bool high = (s_.cr[t] & 0x04) ? (s_.pbToggle & bit) : (s_.pbTimerOut & bit);
        v = static_cast<uint8_t>(high ? (v | bit) : (v & ~bit));
    }
    return v;
}

void Cia6526::setIcr(uint8_t bits) {
    s_.icr |= bits;
    if (s_.imr & bits) s_.delay |= kInt0;
}

void Cia6526::underflow(int t) {
    const uint8_t bit = static_cast<uint8_t>(1 << t);
    setIcr(bit);
    s_.pbToggle ^= static_cast<uint8_t>(0x40 << t);
    if ((s_.cr[t] & 0x06) == 0x02) {   // pulse mode on PB: high for one clock
        s_.pbTimerOut |= static_cast<uint8_t>(0x40 << t);
        s_.delay |= pulseEnd(t, 0);
        s_.delay &= ~pulseEnd(t, 1);
    }
    if ((s_.delay | s_.feed) & oneShot(t)) {   // rule 12
        s_.cr[t] &= static_cast<uint8_t>(~0x01);
        s_.delay &= ~(count(t, 0) | count(t, 1) | count(t, 2));
        s_.feed &= ~count(t, 0);
    }
    if (t == 0) {
        // Timer B counting timer A underflows (CNT is always high on the C64, rule 15).
        const uint8_t src = s_.cr[1] & 0x61;
        if (src == 0x41 || src == 0x61) s_.delay |= count(1, 1);
        // Serial output: each underflow toggles CNT about a clock and a half later; two
        // underflows in a row too close together are not seen (rule 7a).
        if ((s_.cr[0] & 0x40) && (s_.srBits || s_.sdrValid))
            s_.serial |= (s_.serial & (kTog0 | kNog0)) ? kNog1 : kTog1;
    }
    s_.delay |= load(t, 1);
}

void Cia6526::clock() {
    const uint8_t reloadNow = s_.reloadHeld;
    s_.reloadHeld = 0;
    for (int t = 0; t < 2; ++t) {
        if (s_.delay & count(t, 3)) --s_.counter[t];
        // A load is held for a second clock: the counter takes the latch again, so a latch
        // write in between still lands, and the zero detector sees that value (rule 10).
        if (reloadNow & (1 << t)) s_.counter[t] = s_.latch[t];
        if (s_.counter[t] == 0 && (s_.delay & count(t, 2))) underflow(t);
        if (s_.delay & load(t, 1)) {
            s_.counter[t] = s_.latch[t];
            s_.delay &= ~count(t, 2);   // a reload takes the next count out of the pipeline
            s_.reloadHeld |= static_cast<uint8_t>(1 << t);
        }
    }
    for (int t = 0; t < 2; ++t)
        if (s_.delay & pulseEnd(t, 1)) s_.pbTimerOut &= static_cast<uint8_t>(~(0x40 << t));
    if (s_.delay & kInt1) s_.irqLow = 1;
    s_.delay = ((s_.delay << 1) & kShiftMask) | s_.feed;
    serialClock();
}

// The serial port, output mode: the shifter holds the byte one bit left of its low 8 bits;
// srBits counts CNT edges (16 per byte, 17 when a waiting byte was appended).
void Cia6526::serialClock() {
    uint32_t feed = 0;
    if (s_.serial & kSet0) {   // SDR written two clocks ago
        if (s_.srBits == 0) {
            s_.srBits = 16;
            s_.shifter = static_cast<uint16_t>(s_.sdr << 1);
        } else if (s_.srBits == 1) {
            s_.shifter = static_cast<uint16_t>(s_.shifter | s_.sdr);
            s_.srBits = 17;
        } else {
            s_.sdrValid = 1;
        }
    }
    if (s_.serial & kTog0) {   // CNT edge
        if (s_.srBits && (--s_.srBits & 1)) {
            s_.cntOut = 0;
            if (s_.srBits == 1) {
                feed |= kIrq2;   // byte done: ICR bit 3 two clocks later
                if (s_.sdrValid) {
                    s_.shifter = static_cast<uint16_t>(s_.shifter | s_.sdr);
                    s_.sdrValid = 0;
                    s_.srBits = 17;
                }
            }
        } else {
            s_.shifter = static_cast<uint16_t>(s_.shifter << 1);
            s_.cntOut = 1;
        }
    }
    if (s_.serial & kIrq0) setIcr(0x08);
    s_.serial = ((s_.serial | feed) << 1) & ~kSerialClear;
    if (s_.cntOut) s_.serial |= kCnt0;
}

// Changing the serial direction in CRA (rule 7a): a byte half sent may still finish.
void Cia6526::serialDirection(uint8_t newCra) {
    if (!(newCra & 0x40)) {   // output -> input
        constexpr uint32_t wanted = kCnt0 | kCnt1 | kCnt2;   // the 6526 (old chip)
        bool finish = (s_.serial & wanted) != wanted;
        if (!finish && s_.srBits != 2 && !(s_.serial & kTog2) && !(s_.serial & kTog1) && (s_.serial & kTog0))
            finish = true;
        s_.forceFinish = finish ? 1 : 0;
    } else {                  // input -> output
        if (!s_.cntOut && s_.srBits) s_.shifter = static_cast<uint16_t>(s_.shifter << 1);
        if (s_.forceFinish) {
            s_.serial |= kIrq2;   // ICR bit 3 two clocks later
            s_.forceFinish = 0;
        }
    }
    s_.srBits = 0;
    s_.sdrValid = 0;
    s_.serial &= ~(kTogAll | kNogAll);
    s_.cntOut = 1;
}

void Cia6526::setFlag(bool level) {
    if (s_.flag && !level) setIcr(0x10);
    s_.flag = level ? 1 : 0;
}

// ---- time of day (rule 6) -----------------------------------------------------------

// The 50/60 Hz input is divided to 10 Hz by a 6-state ring counter that matches at its
// 5th state (50 Hz) or 6th (60 Hz); a match resets it and advances the tenths on that
// tick. It is cleared when a stopped clock is started again (VICE suite: CIA/tod
// readme, hzsync0-6).
void Cia6526::todTick() {
    if (s_.todStopped) return;
    const uint8_t match = (s_.cr[0] & 0x80) ? 4 : 5;
    if (s_.todDivider == match) {
        s_.todDivider = 0;
        todIncrement();
        checkAlarm();
    } else if (++s_.todDivider > 5) {
        s_.todDivider = 0;
    }
}

// Each digit is a binary counter of its width; a carry happens when a digit reaches the
// value one past its last (10 or 6), so invalid values written count on (fix-* programs).
void Cia6526::todIncrement() {
    uint8_t* t = s_.tod;
    unsigned ts = t[0] & 0x0F, sl = t[1] & 0x0F, sh = (t[1] >> 4) & 7, ml = t[2] & 0x0F, mh = (t[2] >> 4) & 7;
    unsigned hl = t[3] & 0x0F, hh = (t[3] >> 4) & 1, pm = t[3] & 0x80;
    ts = (ts + 1) & 0x0F;
    if (ts == 10) {
        ts = 0;
        sl = (sl + 1) & 0x0F;
        if (sl == 10) {
            sl = 0;
            sh = (sh + 1) & 7;
            if (sh == 6) {
                sh = 0;
                ml = (ml + 1) & 0x0F;
                if (ml == 10) {
                    ml = 0;
                    mh = (mh + 1) & 7;
                    if (mh == 6) {
                        mh = 0;
                        if ((hh == 1 && hl == 2) || (hh == 0 && hl == 9)) {   // 12 -> 1, 9 -> 10
                            hl = hh;
                            hh ^= 1;
                        } else {
                            hl = (hl + 1) & 0x0F;
                            if (hh == 1 && hl == 2) pm ^= 0x80;            // reaching 12 flips AM/PM
                        }
                    }
                }
            }
        }
    }
    t[0] = static_cast<uint8_t>(ts);
    t[1] = static_cast<uint8_t>(sl | (sh << 4));
    t[2] = static_cast<uint8_t>(ml | (mh << 4));
    t[3] = static_cast<uint8_t>(hl | (hh << 4) | pm);
}

void Cia6526::checkAlarm() {
    if (s_.tod[0] == s_.alarm[0] && s_.tod[1] == s_.alarm[1] && s_.tod[2] == s_.alarm[2] && s_.tod[3] == s_.alarm[3])
        setIcr(0x04);
}

// ---- registers --------------------------------------------------------------------------

uint8_t Cia6526::peek(unsigned r) const {
    switch (r & 0x0F) {
    case 0x0: return host_.ciaPortA(id_, portAOut());
    case 0x1: return host_.ciaPortB(id_, portBOut());
    case 0x2: return s_.ddra;
    case 0x3: return s_.ddrb;
    case 0x4: return static_cast<uint8_t>(s_.counter[0]);
    case 0x5: return static_cast<uint8_t>(s_.counter[0] >> 8);
    case 0x6: return static_cast<uint8_t>(s_.counter[1]);
    case 0x7: return static_cast<uint8_t>(s_.counter[1] >> 8);
    case 0x8: case 0x9: case 0xA: case 0xB: {
        const unsigned i = (r & 0x0F) - 8;
        return s_.todLatched ? s_.todLatch[i] : s_.tod[i];
    }
    case 0xC: return s_.sdr;
    case 0xD: return static_cast<uint8_t>((s_.icr & 0x1F) | (s_.irqLow ? 0x80 : 0));
    case 0xE: return static_cast<uint8_t>(s_.cr[0] & ~0x10);
    default: return static_cast<uint8_t>(s_.cr[1] & ~0x10);
    }
}

uint8_t Cia6526::read(unsigned r) {
    r &= 0x0F;
    const uint8_t v = peek(r);
    if (r == 0xB && !s_.todLatched) {   // reading hours latches the time ...
        for (int i = 0; i < 4; ++i) s_.todLatch[i] = s_.tod[i];
        s_.todLatched = 1;
    } else if (r == 0x8) {              // ... until 10ths is read
        s_.todLatched = 0;
    } else if (r == 0xD) {
        s_.icr = 0;
        s_.irqLow = 0;
        s_.delay &= ~(kInt0 | kInt1);
    }
    return v;
}

void Cia6526::write(unsigned r, uint8_t v) {
    r &= 0x0F;
    switch (r) {
    case 0x0: s_.pra = v; return;
    case 0x1: s_.prb = v; return;
    case 0x2: s_.ddra = v; return;
    case 0x3: s_.ddrb = v; return;
    case 0x4: case 0x6: {
        const int t = (r - 4) / 2;
        s_.latch[t] = static_cast<uint16_t>((s_.latch[t] & 0xFF00) | v);
        return;
    }
    case 0x5: case 0x7: {
        const int t = (r - 5) / 2;
        s_.latch[t] = static_cast<uint16_t>((s_.latch[t] & 0x00FF) | (v << 8));
        if (!(s_.cr[t] & 0x01)) s_.delay |= load(t, 0);   // stopped: the counter loads too
        return;
    }
    case 0x8: case 0x9: case 0xA: case 0xB: {
        const unsigned i = r - 8;
        static constexpr uint8_t kMask[4] = {0x0F, 0x7F, 0x7F, 0x9F};
        v &= kMask[i];
        bool changed;
        if (s_.cr[1] & 0x80) {
            changed = s_.alarm[i] != v;
            s_.alarm[i] = v;
        } else {
            if (i == 3 && (v & 0x1F) == 0x12) v ^= 0x80;   // writing hour 12 flips AM/PM (hour-test)
            if (i == 0 && s_.todStopped) {                   // restart: the divider starts at 0
                s_.todDivider = 0;
                s_.todStopped = 0;
            }
            if (i == 3) s_.todStopped = 1;
            changed = s_.tod[i] != v;
            s_.tod[i] = v;
        }
        if (changed) checkAlarm();   // alarm-cond, alarm-cond2
        return;
    }
    case 0xC:
        s_.sdr = v;
        if (s_.cr[0] & 0x40) s_.serial |= kSet1;   // taken into the shifter two clocks later
        return;
    case 0xD:
        if (v & 0x80) s_.imr |= v & 0x1F;
        else s_.imr &= static_cast<uint8_t>(~(v & 0x1F));
        if ((s_.imr & s_.icr & 0x1F) && !s_.irqLow) s_.delay |= kInt0;
        return;
    default: {   // CRA / CRB
        const int t = r - 0xE;
        const uint8_t inputMask = t == 0 ? 0x21 : 0x61;   // start + input mode = phi2
        if ((v & inputMask) == 0x01) {
            s_.delay |= count(t, 1) | count(t, 0);
            s_.feed |= count(t, 0);
        } else {
            s_.delay &= ~(count(t, 1) | count(t, 0));
            s_.feed &= ~count(t, 0);
        }
        if (v & 0x08) s_.feed |= oneShot(t);
        else s_.feed &= ~oneShot(t);
        if (v & 0x10) s_.delay |= load(t, 0);
        if ((v & 0x01) && !(s_.cr[t] & 0x01)) s_.pbToggle |= static_cast<uint8_t>(0x40 << t);   // rule 14
        if (t == 0 && ((v ^ s_.cr[0]) & 0x40)) serialDirection(v);
        s_.cr[t] = static_cast<uint8_t>(v & ~0x10);
        return;
    }
    }
}

} // namespace k64

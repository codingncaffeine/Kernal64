#include "via6522.h"

namespace k64 {

namespace {
enum : uint8_t { kCa2 = 0x01, kCa1 = 0x02, kSr = 0x04, kCb2 = 0x08, kCb1 = 0x10, kT2 = 0x20, kT1 = 0x40 };
// CA2/CB2 mode (three bits of the peripheral control register): 1 and 3 are the
// "independent" interrupt inputs, whose flag an access of the port does not clear.
bool independent(unsigned mode) { return (mode & 5) == 1; }
}

void Via6522::reset() {
    const State old = s_;
    s_ = State{};
    s_.t1c = old.t1c;
    s_.t1l = old.t1l;
    s_.t2c = old.t2c;
    s_.t2ll = old.t2ll;
    s_.sr = old.sr;
    s_.ca1 = old.ca1;
    s_.ca2 = old.ca2;
    s_.cb1 = old.cb1;
    s_.cb2 = old.cb2;
    s_.pb6 = old.pb6;
}

uint8_t Via6522::outB() const {
    // Timer 1's output replaces bit 7 of the output register when ACR bit 7 says so.
    if (s_.acr & 0x80) return static_cast<uint8_t>((s_.orb & 0x7F) | (s_.pb7 ? 0x80 : 0));
    return s_.orb;
}

// A read or write of a port's register: clears the port's interrupt flags (the second
// line's unless it is an independent input) and moves a handshake or pulse output: CA2
// on reads and writes of port A, CB2 on writes of port B only.
void Via6522::portAccess(bool portA, bool write) {
    const unsigned mode = (portA ? s_.pcr >> 1 : s_.pcr >> 5) & 7;
    s_.ifr &= static_cast<uint8_t>(~(portA ? kCa1 : kCb1));
    if (!independent(mode)) s_.ifr &= static_cast<uint8_t>(~(portA ? kCa2 : kCb2));
    if ((mode == 4 || mode == 5) && (portA || write)) {
        (portA ? s_.ca2Low : s_.cb2Low) = 1;
        (portA ? s_.ca2Pulse : s_.cb2Pulse) = mode == 5;
        host_.viaOutputs(id_);
    }
}

uint8_t Via6522::peek(unsigned reg) const {
    switch (reg & 15) {
    case 0: {
        // Pins that are outputs read the output register whatever the pin's level;
        // inputs the pin, or what CB1 latched.
        const uint8_t pins = (s_.acr & 0x02) && (s_.ifr & kCb1) ? s_.irb : host_.viaPortB(id_, outB(), s_.ddrb);
        return static_cast<uint8_t>((outB() & s_.ddrb) | (pins & ~s_.ddrb));
    }
    case 1: case 15:
        // Port A reads its pins, outputs too; or what CA1 latched.
        return (s_.acr & 0x01) && (s_.ifr & kCa1) ? s_.ira : host_.viaPortA(id_, s_.ora, s_.ddra);
    case 2: return s_.ddrb;
    case 3: return s_.ddra;
    case 4: return static_cast<uint8_t>(s_.t1c);
    case 5: return static_cast<uint8_t>(s_.t1c >> 8);
    case 6: return static_cast<uint8_t>(s_.t1l);
    case 7: return static_cast<uint8_t>(s_.t1l >> 8);
    case 8: return static_cast<uint8_t>(s_.t2c);
    case 9: return static_cast<uint8_t>(s_.t2c >> 8);
    case 10: return s_.sr;
    case 11: return s_.acr;
    case 12: return s_.pcr;
    case 13: return static_cast<uint8_t>((s_.ifr & 0x7F) | (irq() ? 0x80 : 0));
    default: return static_cast<uint8_t>(s_.ier | 0x80);
    }
}

uint8_t Via6522::read(unsigned reg) {
    const uint8_t v = peek(reg);
    switch (reg & 15) {
    case 0: portAccess(false, false); break;
    case 1: portAccess(true, false); break;
    case 4: s_.ifr &= static_cast<uint8_t>(~kT1); break;
    case 8: s_.ifr &= static_cast<uint8_t>(~kT2); break;
    case 10: s_.ifr &= static_cast<uint8_t>(~kSr); break;
    default: break;
    }
    return v;
}

void Via6522::write(unsigned reg, uint8_t v) {
    switch (reg & 15) {
    case 0:
        s_.orb = v;
        portAccess(false, true);
        host_.viaOutputs(id_);
        break;
    case 1:
        s_.ora = v;
        portAccess(true, true);
        host_.viaOutputs(id_);
        break;
    case 15:
        s_.ora = v;
        host_.viaOutputs(id_);
        break;
    case 2: s_.ddrb = v; host_.viaOutputs(id_); break;
    case 3: s_.ddra = v; host_.viaOutputs(id_); break;
    case 4: case 6:
        s_.t1l = static_cast<uint16_t>((s_.t1l & 0xFF00) | v);
        break;
    case 5:
        // Both latches go into the counter; the flag is cleared; with its output switched
        // on, PB7 goes low until the time-out.
        s_.t1l = static_cast<uint16_t>((s_.t1l & 0x00FF) | (v << 8));
        s_.t1c = s_.t1l;
        s_.t1Loaded = 1;
        s_.t1Reload = 0;
        s_.t1Armed = 1;
        s_.ifr &= static_cast<uint8_t>(~kT1);
        s_.pb7 = 0;
        host_.viaOutputs(id_);
        break;
    case 7:
        s_.t1l = static_cast<uint16_t>((s_.t1l & 0x00FF) | (v << 8));
        s_.ifr &= static_cast<uint8_t>(~kT1);
        break;
    case 8: s_.t2ll = v; break;
    case 9:
        s_.t2c = static_cast<uint16_t>((v << 8) | s_.t2ll);
        s_.t2Loaded = 1;
        s_.t2Armed = 1;
        s_.ifr &= static_cast<uint8_t>(~kT2);
        break;
    case 10:
        s_.sr = v;
        s_.ifr &= static_cast<uint8_t>(~kSr);
        break;
    case 11: s_.acr = v; host_.viaOutputs(id_); break;
    case 12:
        s_.pcr = v;
        // A line switched to handshake or pulse output starts high.
        if (((v >> 1) & 6) != 4) s_.ca2Low = s_.ca2Pulse = 0;
        if (((v >> 5) & 6) != 4) s_.cb2Low = s_.cb2Pulse = 0;
        host_.viaOutputs(id_);
        break;
    case 13: s_.ifr &= static_cast<uint8_t>(~(v & 0x7F)); break;
    default:
        if (v & 0x80) s_.ier |= v & 0x7F;
        else s_.ier &= static_cast<uint8_t>(~(v & 0x7F));
        break;
    }
}

// CA1 and CB1: the edge chosen in the peripheral control register sets the flag,
// latches the port when latching is on, and ends a handshake on the second line.
void Via6522::setCa1(bool level) {
    const bool rising = level && !s_.ca1, falling = !level && s_.ca1;
    s_.ca1 = level ? 1 : 0;
    if (!((s_.pcr & 0x01) ? rising : falling)) return;
    if (s_.acr & 0x01) s_.ira = host_.viaPortA(id_, s_.ora, s_.ddra);
    setIfr(kCa1);
    if (((s_.pcr >> 1) & 7) == 4 && s_.ca2Low) {
        s_.ca2Low = 0;
        host_.viaOutputs(id_);
    }
}

void Via6522::setCb1(bool level) {
    const bool rising = level && !s_.cb1, falling = !level && s_.cb1;
    s_.cb1 = level ? 1 : 0;
    if (!((s_.pcr & 0x10) ? rising : falling)) return;
    if (s_.acr & 0x02) s_.irb = host_.viaPortB(id_, outB(), s_.ddrb);
    setIfr(kCb1);
    if (((s_.pcr >> 5) & 7) == 4 && s_.cb2Low) {
        s_.cb2Low = 0;
        host_.viaOutputs(id_);
    }
}

// CA2 and CB2 as inputs (modes 0-3): bit 1 of the mode is the edge.
void Via6522::setCa2(bool level) {
    const bool rising = level && !s_.ca2, falling = !level && s_.ca2;
    s_.ca2 = level ? 1 : 0;
    const unsigned mode = (s_.pcr >> 1) & 7;
    if (mode < 4 && ((mode & 2) ? rising : falling)) setIfr(kCa2);
}

void Via6522::setCb2(bool level) {
    const bool rising = level && !s_.cb2, falling = !level && s_.cb2;
    s_.cb2 = level ? 1 : 0;
    const unsigned mode = (s_.pcr >> 5) & 7;
    if (mode < 4 && ((mode & 2) ? rising : falling)) setIfr(kCb2);
}

void Via6522::clock() {
    bool outputs = false;
    // A pulse on CA2/CB2 lasts the cycle after the access.
    if (s_.ca2Pulse) {
        if (s_.ca2Pulse == 2) { s_.ca2Low = 0; s_.ca2Pulse = 0; outputs = true; }
        else s_.ca2Pulse = 2;
    }
    if (s_.cb2Pulse) {
        if (s_.cb2Pulse == 2) { s_.cb2Low = 0; s_.cb2Pulse = 0; outputs = true; }
        else s_.cb2Pulse = 2;
    }

    // Timer 1. A counter written in this cycle shows its value through the next one and
    // counts from there: N, N-1, ... 0, $FFFF. The flag is set in the cycle that shows
    // $FFFF, a cycle and a half after the one that shows 0 began. Free-running, the
    // latch then goes in again (a period of N + 2); in the one-shot mode the counter
    // goes on down from $FFFF and sets no more flags.
    if (s_.t1Loaded) {
        s_.t1Loaded = 0;
    } else if (s_.t1Reload) {
        s_.t1Reload = 0;
        s_.t1c = s_.t1l;
    } else if (s_.t1c-- == 0) {
        const bool freeRun = (s_.acr & 0x40) != 0;
        if (s_.t1Armed) {
            setIfr(kT1);
            s_.pb7 = freeRun ? !s_.pb7 : 1;
            outputs = outputs || (s_.acr & 0x80);
        }
        if (freeRun) s_.t1Reload = 1;
        else s_.t1Armed = 0;
    }

    // Timer 2: one shot only; with ACR bit 5 it counts falling edges on PB6 instead of
    // cycles. After its time-out it goes on counting.
    const uint8_t pb6 = host_.viaPortB(id_, outB(), s_.ddrb) & 0x40 ? 1 : 0;
    const bool pulse = s_.pb6 && !pb6;
    s_.pb6 = pb6;
    if (s_.t2Loaded) {
        s_.t2Loaded = 0;
    } else if (!(s_.acr & 0x20) || pulse) {
        if (s_.t2c-- == 0 && s_.t2Armed) {
            setIfr(kT2);
            s_.t2Armed = 0;
        }
    }
    if (outputs) host_.viaOutputs(id_);
}

} // namespace k64

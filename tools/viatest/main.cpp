// viatest: the 6522's rules as its data sheet states them, checked on the chip model
// alone (the timing against real chips comes with the test programs that run in the
// drive).
//
//   viatest
//
// A cycle here is: the CPU's access, then the chip's clock. "Cycle W + k" below counts
// from the cycle of a write.
// Exit status 0 when every test passes.
#include "via6522.h"

#include <cstdio>

using namespace k64;

namespace {

int checks = 0, failures = 0;
const char* current = "";

void equal(long got, long want, const char* what) {
    ++checks;
    if (got == want) return;
    ++failures;
    std::printf("  FAIL %s: %s (got %ld, expected %ld)\n", current, what, got, want);
}

// What is outside the chip: levels pulled onto the pins, and a count of output changes.
struct Board : ViaHost {
    uint8_t pullA = 0xFF, pullB = 0xFF;   // 0 bits: something outside holds the pin low
    int changes = 0;
    uint8_t viaPortA(int, uint8_t out, uint8_t ddr) override { return static_cast<uint8_t>((out | ~ddr) & pullA); }
    uint8_t viaPortB(int, uint8_t out, uint8_t ddr) override { return static_cast<uint8_t>((out | ~ddr) & pullB); }
    void viaOutputs(int) override { ++changes; }
};

enum Reg { ORB, ORA, DDRB, DDRA, T1CL, T1CH, T1LL, T1LH, T2CL, T2CH, SR, ACR, PCR, IFR, IER, ORAX };

// Timer 1, one shot (Figure 15): counter N, N-1, ..., 0, $FFFF from the cycle after the
// write; the flag in the cycle that shows $FFFF; no second flag.
void testTimer1OneShot() {
    current = "timer 1, one shot";
    Board b;
    Via6522 v(b, 0);
    v.reset();
    v.write(IER, 0xC0);
    v.write(T1CL, 5);
    v.write(T1CH, 0);       // cycle W
    v.clock();
    for (int k = 1; k <= 6; ++k) {   // cycles W+1 .. W+6 show 5 .. 0
        equal(v.peek(T1CL), 6 - k, "counter");
        equal(v.irq(), 0, "no interrupt before the time-out");
        v.clock();
    }
    equal(v.peek(T1CL) | (v.peek(T1CH) << 8), 0xFFFF, "counter in the cycle after zero");
    equal(v.irq(), 1, "interrupt in the cycle after zero");
    equal(v.peek(IFR), 0xC0, "flag register");
    v.clock();
    equal(v.peek(T1CL), 0xFE, "the counter goes on down");
    equal(v.read(T1CL), 0xFE, "reading the low counter");
    equal(v.irq(), 0, "that read clears the flag");
    for (int i = 0; i < 70000; ++i) v.clock();
    equal(v.irq(), 0, "no second interrupt without a new write");
    const int before = v.peek(T1CL) | (v.peek(T1CH) << 8);
    v.write(T1LH, 0x12);
    equal(v.peek(T1CL) | (v.peek(T1CH) << 8), before, "a write of the high latch moves nothing into the counter");
    equal(v.peek(T1LH), 0x12, "the high latch reads back");
}

// Free-running (Figure 16): a period of N + 2 cycles; a new latch value counts from the
// next time-out on.
void testTimer1FreeRun() {
    current = "timer 1, free-running";
    Board b;
    Via6522 v(b, 0);
    v.reset();
    v.write(ACR, 0x40);
    v.write(IER, 0xC0);
    v.write(T1CL, 3);
    v.write(T1CH, 0);
    v.clock();
    int last = -1, period = 0, count = 0;
    for (int c = 1; c <= 40; ++c) {
        if (v.irq()) {
            if (last >= 0) { period = c - last; ++count; equal(period, 5, "period"); }
            last = c;
            v.write(IFR, 0x40);
            if (count >= 3) break;
        }
        v.clock();
    }
    equal(count, 3, "interrupts seen");
    v.clock();              // the latch has gone into the counter again
    v.write(T1LL, 7);       // into the latch only
    int next = 0;
    for (int c = 2; c <= 40 && !next; ++c) { v.clock(); if (v.irq()) next = c; }
    equal(next, 5, "the time-out in progress keeps its length");
    v.write(IFR, 0x40);
    next = 0;
    for (int c = 1; c <= 40 && !next; ++c) { v.clock(); if (v.irq()) next = c; }
    equal(next, 9, "the next one has the new latch's length");
}

// PB7 as timer 1's output: low from the write to the time-out (one shot), inverted at
// every time-out (free-running); only with ACR bit 7.
void testPb7() {
    current = "timer 1 on PB7";
    Board b;
    Via6522 v(b, 0);
    v.reset();
    v.write(DDRB, 0x80);
    v.write(ORB, 0x80);
    v.write(ACR, 0x80);
    v.write(T1CL, 2);
    v.write(T1CH, 0);
    equal(v.outB() & 0x80, 0, "low after the write");
    for (int i = 0; i < 3; ++i) v.clock();
    equal(v.outB() & 0x80, 0, "still low while it counts");
    v.clock();
    equal(v.outB() & 0x80, 0x80, "high at the time-out");
    v.write(ACR, 0xC0);
    v.write(T1CH, 0);
    int toggles = 0, level = v.outB() & 0x80;
    for (int i = 0; i < 16; ++i) { v.clock(); if ((v.outB() & 0x80) != level) { ++toggles; level = v.outB() & 0x80; } }
    equal(toggles, 4, "inverted at every time-out");
    v.write(ACR, 0x40);
    v.write(ORB, 0x00);
    equal(v.outB() & 0x80, 0, "without ACR bit 7 the output register's bit");
}

// Timer 2 (Figures 18, 19): one shot; counting falling edges on PB6 with ACR bit 5.
void testTimer2() {
    current = "timer 2";
    Board b;
    Via6522 v(b, 0);
    v.reset();
    v.write(IER, 0xA0);
    const int before = v.peek(T2CL);
    v.write(T2CL, 4);
    equal(v.peek(T2CL), before, "the low byte goes to the latch, not the counter");
    v.write(T2CH, 0);
    v.clock();
    for (int k = 1; k <= 5; ++k) { equal(v.peek(T2CL), 5 - k, "counter"); equal(v.irq(), 0, "no interrupt yet"); v.clock(); }
    equal(v.irq(), 1, "interrupt in the cycle after zero");
    equal(v.read(T2CL), 0xFF, "the counter goes on down");
    equal(v.irq(), 0, "reading the low counter clears the flag");
    for (int i = 0; i < 70000; ++i) v.clock();
    equal(v.irq(), 0, "no second interrupt without a new write");

    v.write(ACR, 0x20);
    v.write(T2CL, 2);
    v.write(T2CH, 0);
    for (int i = 0; i < 50; ++i) v.clock();
    equal(v.peek(T2CL), 2, "pulse counting: cycles alone do not count");
    for (int p = 0; p < 3; ++p) {
        equal(v.irq(), 0, "no interrupt before the count is passed");
        b.pullB = 0xBF; v.clock();
        b.pullB = 0xFF; v.clock();
    }
    equal(v.irq(), 1, "interrupt when the count passes zero");
}

// Ports: output pins of port B read the output register whatever the pin does; port A
// reads its pins; inputs read the pin.
void testPorts() {
    current = "ports";
    Board b;
    Via6522 v(b, 0);
    v.reset();
    v.write(DDRA, 0x0F);
    v.write(ORA, 0xA5);
    v.write(DDRB, 0x0F);
    v.write(ORB, 0xA5);
    b.pullA = 0xFE;         // an output pin of each port held low from outside
    b.pullB = 0xFE;
    equal(v.read(ORA), 0xF4, "port A: the pins");
    equal(v.read(ORB), 0xF5, "port B: the output register for output pins");
    b.pullA = b.pullB = 0x7F;
    equal(v.read(ORA) & 0x80, 0, "port A input pin");
    equal(v.read(ORB) & 0x80, 0, "port B input pin");
    equal(v.outA(), 0xA5, "the output register stays");
}

// Input latching: with ACR bit 0 (1) the port reads as it was at CA1's (CB1's) active
// edge, until the port is read.
void testLatching() {
    current = "input latching";
    Board b;
    Via6522 v(b, 0);
    v.reset();
    v.write(ACR, 0x03);
    v.write(PCR, 0x00);     // falling edges
    b.pullA = 0x12;
    b.pullB = 0x34;
    v.setCa1(false);
    v.setCb1(false);
    b.pullA = 0x56;
    b.pullB = 0x78;
    equal(v.peek(IFR) & 0x12, 0x12, "both flags");
    equal(v.read(ORA), 0x12, "port A as latched");
    equal(v.read(ORB), 0x34, "port B as latched");
    equal(v.peek(IFR) & 0x12, 0, "the reads clear the flags");
    equal(v.read(ORA), 0x56, "then the pins again");
    v.setCa1(true);
    equal(v.peek(IFR) & 0x02, 0, "the other edge does nothing");
    v.write(PCR, 0x11);     // rising edges
    v.read(ORB);
    v.setCa1(false);
    equal(v.peek(IFR) & 0x12, 0, "falling edges do nothing then");
    v.setCa1(true);
    v.setCb1(true);         // (it was low)
    equal(v.peek(IFR) & 0x12, 0x12, "rising edges when chosen");
}

// CA2 and CB2: as inputs with either edge; "independent" inputs keep their flag through
// a port access; as outputs held low or high, handshake (low on the access, high on the
// first line's edge) and pulse (low for one cycle). CB2 moves on writes only.
void testControlLines() {
    current = "CA2 and CB2";
    Board b;
    Via6522 v(b, 0);
    v.reset();
    v.write(PCR, 0x00);
    v.setCa2(false);
    v.setCb2(false);
    equal(v.peek(IFR) & 0x09, 0x09, "falling edges set the flags");
    v.read(ORA);
    v.read(ORB);
    equal(v.peek(IFR) & 0x09, 0, "a port access clears them");
    v.write(PCR, 0x66);     // rising edges, independent
    v.setCa2(true);
    v.setCb2(true);
    v.read(ORA);
    v.write(ORB, 0);
    equal(v.peek(IFR) & 0x09, 0x09, "independent inputs keep their flags");
    v.write(IFR, 0x09);
    equal(v.peek(IFR) & 0x09, 0, "until they are written to the flag register");

    v.write(PCR, 0xCC);
    equal(v.ca2Out() || v.cb2Out(), 0, "held low");
    v.write(PCR, 0xEE);
    equal(v.ca2Out() && v.cb2Out(), 1, "held high");

    v.write(PCR, 0x88);     // handshake
    equal(v.ca2Out() && v.cb2Out(), 1, "handshake: high until an access");
    v.read(ORA);
    v.read(ORB);
    equal(v.ca2Out(), 0, "CA2 low on a read of port A");
    equal(v.cb2Out(), 1, "CB2 not on a read of port B");
    v.write(ORB, 0);
    equal(v.cb2Out(), 0, "CB2 low on a write of port B");
    v.clock();
    v.clock();
    equal(v.ca2Out() || v.cb2Out(), 0, "they stay low");
    v.setCa1(true);
    v.setCa1(false);
    v.setCb1(true);
    v.setCb1(false);
    equal(v.ca2Out() && v.cb2Out(), 1, "high again on the first line's active edge");
    v.read(ORAX);
    equal(v.ca2Out(), 1, "register 15: no handshake");

    v.write(PCR, 0xAA);     // pulse
    v.write(ORA, 0);
    v.write(ORB, 0);
    equal(v.ca2Out() || v.cb2Out(), 0, "pulse: low on the access");
    v.clock();
    equal(v.ca2Out() || v.cb2Out(), 0, "low through the next cycle");
    v.clock();
    equal(v.ca2Out() && v.cb2Out(), 1, "then high");
}

// Flags and enables (Figures 29, 30): bit 7 of the flag register is the IRQ output;
// the enable register sets or clears the bits written as ones; it reads with bit 7 set.
void testInterruptRegisters() {
    current = "interrupt registers";
    Board b;
    Via6522 v(b, 0);
    v.reset();
    v.setCa1(false);
    equal(v.peek(IFR), 0x02, "a flag without its enable: bit 7 clear");
    equal(v.irq(), 0, "and no interrupt");
    v.write(IER, 0x82);
    equal(v.peek(IFR), 0x82, "enabled: bit 7 set");
    equal(v.irq(), 1, "and the interrupt");
    equal(v.read(IER), 0x82, "the enable register reads with bit 7 set");
    v.write(IER, 0xFF);
    equal(v.read(IER), 0xFF, "bit 7 set: ones set");
    v.write(IER, 0x7D);
    equal(v.read(IER), 0x82, "bit 7 clear: ones clear");
    v.write(IFR, 0x80);
    equal(v.irq(), 1, "writing bit 7 of the flag register clears nothing");
    v.write(IFR, 0x02);
    equal(v.irq(), 0, "writing the flag's bit clears it");
    v.write(SR, 0x5A);
    equal(v.read(SR), 0x5A, "the shift register holds what was written");
    v.setCa1(true);
    v.setCa1(false);
    v.reset();
    equal(v.peek(IFR) | v.read(IER), 0x80, "reset clears flags and enables");
    equal(v.read(SR), 0x5A, "but not the shift register");
}

} // namespace

int main() {
    testTimer1OneShot();
    testTimer1FreeRun();
    testPb7();
    testTimer2();
    testPorts();
    testLatching();
    testControlLines();
    testInterruptRegisters();
    std::printf("viatest: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}

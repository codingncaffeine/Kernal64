// sidtest: the SID's paddle converters against their rule, cycle by cycle.
//
// The rest of the chip is compared with a reference engine (siddiff); that engine has no
// paddle inputs, so these rules are checked here against their description: a conversion
// takes 512 cycles; for the first 256 the chip holds the pin low; then it counts cycles
// from 0 while the pin's capacitor charges through whatever is connected; the count at
// which the pin passes the input's threshold is captured, 255 if it never does.
//
//   sidtest [--only NAME]
//
// Exit status 0 when every test passes.
#include "sid.h"

#include <cstdio>
#include <cstring>

using namespace k64;

namespace {

int checks = 0, failures = 0;
const char* current = "";

void check(bool ok, const char* what, long got, long want) {
    ++checks;
    if (ok) return;
    ++failures;
    std::printf("  FAIL %s: %s (got %ld, expected %ld)\n", current, what, got, want);
}
void equal(long got, long want, const char* what) { check(got == want, what, got, want); }

constexpr unsigned kPotX = 0x19, kPotY = 0x1A;
constexpr int kPeriod = 512, kMeasureStart = 256;

void run(Sid& sid, int cycles) { for (int i = 0; i < cycles; ++i) sid.clock(); }
// A resistance that alone reads `value`.
uint32_t r(unsigned value) { return Sid::potChargeFor(value); }

// A chip switched on: its first conversion starts with the first cycle.
void testNothingConnected() {
    Sid sid;
    equal(sid.read(kPotX), 0xFF, "POTX at power on");
    equal(sid.read(kPotY), 0xFF, "POTY at power on");
    bool always = true;
    for (int i = 0; i < 4 * kPeriod; ++i) {
        sid.clock();
        always = always && sid.read(kPotX) == 0xFF && sid.read(kPotY) == 0xFF;
    }
    check(always, "both registers stay $FF with nothing connected", always, 1);
}

// The register takes the value in the cycle the count reaches it, not before, and holds
// it through the rest of the period and the next discharge.
void testCaptureCycle() {
    static const unsigned kValues[] = {0, 1, 2, 63, 64, 127, 128, 200, 254, 255};
    for (unsigned v : kValues) {
        Sid sid;
        const unsigned w = 255 - v;            // the other pin converts something else
        sid.setPotCharge(0, r(v));
        sid.setPotCharge(1, r(w));
        int firstX = -1, firstY = -1;
        const uint8_t beforeX = sid.read(kPotX), beforeY = sid.read(kPotY);
        for (int cycle = 0; cycle < kPeriod; ++cycle) {
            sid.clock();                        // cycle `cycle` of the period is done
            if (firstX < 0 && (sid.read(kPotX) != beforeX || cycle == kPeriod - 1)) firstX = cycle;
            if (firstY < 0 && (sid.read(kPotY) != beforeY || cycle == kPeriod - 1)) firstY = cycle;
        }
        equal(sid.read(kPotX), v, "POTX after one conversion");
        equal(sid.read(kPotY), w, "POTY after one conversion");
        if (v != 255) equal(firstX, kMeasureStart + static_cast<int>(v), "the cycle POTX was captured in");
        if (w != 255) equal(firstY, kMeasureStart + static_cast<int>(w), "the cycle POTY was captured in");
        // Held: through the second conversion's discharge and up to its capture.
        bool held = true;
        for (int cycle = 0; cycle < kMeasureStart + static_cast<int>(v); ++cycle) {
            sid.clock();
            held = held && sid.read(kPotX) == v;
        }
        check(held, "POTX held until the next capture", held, 1);
    }
}

// A pin that never reaches the threshold: the old value stays until the count ends.
void testNeverReached() {
    Sid sid;
    sid.setPotCharge(0, r(10));
    run(sid, kPeriod);
    equal(sid.read(kPotX), 10, "POTX after the first conversion");
    sid.setPotCharge(0, 0);                     // unplugged
    run(sid, kPeriod - 1);
    equal(sid.read(kPotX), 10, "POTX one cycle before the count ends");
    run(sid, 1);
    equal(sid.read(kPotX), 0xFF, "POTX when the count has ended");
}

// The capacitor adds up what flows in: a resistance connected for part of the measuring
// time gives a later capture; two in parallel an earlier one; nothing flows while the
// chip holds the pin low.
void testCharge() {
    {   // Connected from count 32 on: 32 counts later than alone.
        Sid sid;
        run(sid, kMeasureStart + 32);
        sid.setPotCharge(0, r(63));
        run(sid, kPeriod - kMeasureStart - 32);
        equal(sid.read(kPotX), 32 + 63, "connected from count 32 on");
    }
    {   // Connected for the first 32 counts only: half charged, never there.
        Sid sid;
        sid.setPotCharge(0, r(63));
        run(sid, kMeasureStart + 32);
        sid.setPotCharge(0, 0);
        run(sid, kPeriod - kMeasureStart - 32);
        equal(sid.read(kPotX), 0xFF, "connected for the first 32 counts only");
    }
    {   // Half charged through 64 counts' worth, the other half through 32 counts' worth.
        Sid sid;
        sid.setPotCharge(0, r(63));
        run(sid, kMeasureStart + 32);
        sid.setPotCharge(0, r(31));
        run(sid, kPeriod - kMeasureStart - 32);
        equal(sid.read(kPotX), 32 + 16 - 1, "half through one resistance, half through one of half the size");
    }
    {   // In parallel: 1 / (1/64 + 1/192) = 48 counts.
        Sid sid;
        sid.setPotCharge(0, r(63) + r(191));
        run(sid, kPeriod);
        equal(sid.read(kPotX), 47, "two resistances in parallel");
    }
    {   // Connected during the discharge only.
        Sid sid;
        sid.setPotCharge(0, r(0));
        run(sid, kMeasureStart);
        sid.setPotCharge(0, 0);
        run(sid, kPeriod - kMeasureStart);
        equal(sid.read(kPotX), 0xFF, "connected while the pin is held low");
    }
}

// RES leaves the converters alone: the registers keep their value and the conversion
// under way ends in the cycle it would have ended in.
void testReset() {
    Sid sid;
    sid.setPotCharge(0, r(100));
    run(sid, kPeriod);                          // captured once
    sid.setPotCharge(0, r(60));
    run(sid, kMeasureStart + 44);               // 44 counts into the second conversion
    sid.reset();
    equal(sid.read(kPotX), 100, "POTX after RES");
    run(sid, 60 - 44);
    equal(sid.read(kPotX), 100, "one cycle before the second conversion's capture");
    run(sid, 1);
    equal(sid.read(kPotX), 60, "the second conversion's capture");
}

// Reading a paddle register puts its value on the chip's data bus, where the write-only
// registers read it back.
void testBusValue() {
    Sid sid;
    sid.setPotCharge(1, r(0x5A));
    run(sid, kPeriod);
    equal(sid.read(kPotY), 0x5A, "POTY");
    equal(sid.read(0x00), 0x5A, "a write-only register after reading POTY");
}

// The conversion is part of the state: saved in the middle of a count and loaded into
// another chip, it ends in the same cycle with the same value.
void testState() {
    Sid a;
    a.setPotCharge(0, r(77));
    run(a, kMeasureStart + 20);
    Sid b;
    b.setPotCharge(0, r(77));
    b.setState(a.state());
    run(b, 77 - 20);
    equal(b.read(kPotX), 0xFF, "one cycle before the capture, after loading the state");
    run(b, 1);
    equal(b.read(kPotX), 77, "the capture, after loading the state");
}

const struct { const char* name; void (*fn)(); } kTests[] = {
    {"nothing-connected", testNothingConnected}, {"capture-cycle", testCaptureCycle},
    {"never-reached", testNeverReached},         {"charge", testCharge},
    {"reset", testReset},                        {"bus-value", testBusValue},
    {"state", testState},
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
    std::printf("sidtest: %d tests, %d checks, %d failed\n", ran, checks, failures);
    return failures ? 1 : 0;
}

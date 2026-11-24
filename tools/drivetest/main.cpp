// drivetest: the 1541's board on its own, with the DOS ROM from a folder.
//
//   drivetest ROM_FOLDER [--trace]
//
// Checks: after power on the ROM runs its self tests and settles in its idle loop with
// the activity light off; the drive's hardware answers ATN on the DATA line at once,
// without the CPU; the CPU is interrupted by ATN and leaves the idle loop to serve it;
// with ATN let go it comes back; the device number jumpers reach the ROM.
// Exit status 0 when every check passes; 3 when the ROM is not there.
#include "c1541.h"
#include "roms.h"

#include <cstdio>
#include <cstring>
#include <set>

using namespace k64;

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (ok) return;
    ++failures;
    std::printf("  FAIL %s\n", what);
}
// The addresses the CPU starts instructions at over a number of cycles.
std::set<uint16_t> pcs(C1541& d, int cycles) {
    std::set<uint16_t> seen;
    for (int i = 0; i < cycles; ++i) {
        if (d.cpuAtInstructionStart()) seen.insert(d.cpuRegs().pc);
        d.tick();
    }
    return seen;
}
bool within(const std::set<uint16_t>& a, const std::set<uint16_t>& loop) {
    for (uint16_t pc : a)
        if (!loop.count(pc)) return false;
    return true;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: drivetest ROM_FOLDER [--trace]\n"); return 2; }
    const bool trace = argc > 2 && !std::strcmp(argv[2], "--trace");
    const RomSet roms = findRoms(argv[1]);
    static C1541 drive;
    drive.setRoms(roms);
    if (!drive.hasRoms()) { std::fprintf(stderr, "drivetest: the 1541's ROM halves are not in %s\n", argv[1]); return 3; }
    drive.reset();

    // Power on: the self tests take about a second and a half of the drive's time.
    check(drive.led() && drive.motor(), "light and motor are on from power on, until the ROM sets its port up");
    bool lightSeen = false;
    for (int i = 0; i < 2500000; ++i) { drive.tick(); lightSeen = lightSeen || drive.led(); }
    const std::set<uint16_t> idle = pcs(drive, 200000);
    if (trace) {
        std::printf("idle loop: %zu addresses, $%04x-$%04x\n", idle.size(), *idle.begin(), *idle.rbegin());
        std::printf("pc=%04x led=%d motor=%d\n", drive.cpuRegs().pc, drive.led(), drive.motor());
    }
    check(lightSeen, "the activity light is on during the self tests");
    check(!drive.led(), "and off after them");
    check(!drive.motor(), "the motor is off");
    check(idle.size() < 200 && *idle.begin() >= 0xC000, "the CPU is in a loop in the ROM");
    check(within(pcs(drive, 200000), idle), "and stays in it");
    check(!drive.pullsClk() && !drive.pullsData(), "the drive pulls neither bus line when idle");

    // ATN, with CLK held as the computer holds it: the hardware pulls DATA in the same
    // instant; the CPU follows through CA1 and keeps DATA pulled while it waits for the
    // computer to let go of CLK.
    drive.setBus(true, true, false);
    check(drive.pullsData(), "ATN is answered on DATA at once, by the hardware");
    const std::set<uint16_t> serving = pcs(drive, 3000);
    check(!within(serving, idle), "the CPU leaves the idle loop to serve ATN");
    check(drive.pullsData() && !drive.pullsClk(), "DATA stays pulled, CLK is left alone, while the drive listens under ATN");
    for (int i = 0; i < 200000; ++i) drive.tick();
    check(!within(pcs(drive, 20000), idle) && drive.pullsData(), "it waits there for as long as ATN and CLK are held");
    // The computer goes away again (no bytes sent): the drive gives up and idles.
    drive.setBus(false, false, false);
    for (int i = 0; i < 200000; ++i) drive.tick();
    check(within(pcs(drive, 200000), idle), "with ATN let go the CPU is back in the idle loop");
    check(!drive.pullsData() && !drive.pullsClk(), "and the lines are let go");

    // The jumpers: the ROM computes its two addresses from them at power on ($77 = the
    // number + $20 for listening, $78 = + $40 for talking).
    for (int number = 8; number <= 11; ++number) {
        drive.setDeviceNumber(number);
        drive.reset();
        for (int i = 0; i < 2500000; ++i) drive.tick();
        check(drive.ram()[0x77] == number + 0x20 && drive.ram()[0x78] == number + 0x40, "the device number from the jumpers");
    }
    // The stepper (hw/drive.md R2, R2a): the coils hold the head at absolute positions, track 1 =
    // coil 0, so track 18 (half-track 36) is coil 2; they are powered only with the motor on.
    {
        drive.setDeviceNumber(8);
        drive.reset();
        for (int i = 0; i < 2500000; ++i) drive.tick();
        // The ROM writes $F7 to port B while its lines are still inputs (floating high): coil 3 with
        // the motor on pulls the head from track 18 a half-track in, as the reference does and as
        // real drives can (its note); the DOS's first read finds the track and seeks back.
        check(drive.halfTrack() == 37, "after power on the head is at 18.5, pulled by the port's set-up");
        const uint8_t keep = drive.via2().outB() & 0x98;    // the bits other than coils, motor, zone
        auto coils = [&](int coil, bool motor) { drive.busWrite(0x1C00, static_cast<uint8_t>(keep | 0x40 | (motor ? 4 : 0) | coil)); };
        coils(2, true);
        check(drive.halfTrack() == 36, "coil 2 with the motor on: back over track 18");
        coils(1, true);
        check(drive.halfTrack() == 35, "coil 1: a half-track out, to 17.5");
        coils(2, true);
        check(drive.halfTrack() == 36, "coil 2 again: back on track 18 (a loader writing coils 1 then 2 there stays on 18)");
        coils(3, true);
        coils(0, true);
        check(drive.halfTrack() == 38, "coils 3 then 0: in to track 19");
        coils(2, true);
        check(drive.halfTrack() == 38, "the opposite coil leaves the head where it is");
        coils(1, false);
        check(drive.halfTrack() == 38, "with the motor off the coils do not move the head");
        coils(1, true);
        check(drive.halfTrack() == 39, "and switching the motor on with coil 1 selected pulls it in to 19.5");
    }

    std::printf("drivetest: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}

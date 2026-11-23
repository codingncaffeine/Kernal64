#!/usr/bin/env python3
"""Compares what the reference suite's freeze test program (C64/carts/rr-freeze) found on
our machine with a dump file the same program saved on a real cartridge.

    tools/freezecmp.py K64RUN-LOG DUMP.prg

K64RUN-LOG: the output of k64run with --dump 0800:534 (the program keeps its results from
$0800: a 32-byte header, then four sections: after reset, after writing the second
register, in the freeze, after the freeze was acknowledged; each one detected state per
memory area and one per area, bank and register value).
A dump of the program's older format lacks two flag bits per value (ours are dropped for
the comparison) and of its header only the second register's value is compared. The two header bytes that hold the status register after the freeze are
compared without the "button held" bit: it says how long a person held the button.
Prints every difference by section, area and bank; exit status 1 when there is one.
"""
import re
import sys

AREAS = ["$9Exx", "$BExx", "$DExx", "$DFxx", "$FExx"]
SECTIONS = ["after reset", "configured", "frozen", "acknowledged"]
SECTION = 5 + 4 * 2 * 5 * 8


def where(off):
    if off < 0x20:
        return f"header byte ${off:02x}"
    sec, off = divmod(off - 0x20, SECTION)
    if off < 5:
        return f"{SECTIONS[sec]}, detected state of {AREAS[off]}"
    k, off = divmod(off - 5, 80)
    rom, off = divmod(off, 40)
    area, bank = divmod(off, 8)
    return f"{SECTIONS[sec]}, register bits 1-0 = {k >> 1}{k & 1}, {'ROM' if rom else 'RAM'} selected, {AREAS[area]}, bank {bank}"


def main():
    ram = bytearray()
    for line in open(sys.argv[1], encoding="latin-1"):
        m = re.match(r"^[0-9a-f]{4}:((?: [0-9a-f]{2})+)\s*$", line)
        if m:
            ram += bytes(int(v, 16) for v in m.group(1).split())
    ref = open(sys.argv[2], "rb").read()[2:]
    if len(ram) < len(ref):
        print(f"the log holds {len(ram)} bytes of memory, the dump {len(ref)}")
        return 1
    if not ref.startswith(b"RR-FREEZE"):
        print("not a dump of the freeze test")
        return 1
    older = ref[0x0F] == 0
    bad = 0
    for i, want in enumerate(ref):
        got = ram[i]
        if i < 0x10 or (older and 0x10 < i < 0x20):
            continue            # the program's name and revision; the older header's other bytes
        if i in (0x16, 0x17):
            got &= ~0x04 & 0xFF
            want &= ~0x04 & 0xFF
        elif i >= 0x20 and older and got < 0xFE:
            got &= 0xCF
        if got != want:
            bad += 1
            if bad <= 12:
                print(f"  {where(i)}: ours ${got:02x}, the cartridge ${want:02x}")
    if bad > 12:
        print(f"  ... {bad} differences")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

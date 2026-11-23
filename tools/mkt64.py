#!/usr/bin/env python3
"""Wraps program files (two bytes of load address, then the bytes) into the containers
the core also takes, for the project's own tests.

    tools/mkt64.py --p00 OUT.p00 PROGRAM.prg
    tools/mkt64.py --t64 OUT.t64 [--bad-end] [--empty-first] PROGRAM.prg...

P00: "C64File", a zero, 16 bytes of name, a zero, a record size, then the program file.
T64: a 64-byte tape record, one 32-byte file record per program (entry type 1, file type
$82, start and end address, offset of the bytes), then the programs' bytes without their
load addresses. --bad-end writes $C3C6 as every end address, as one old tool did;
--empty-first puts a free record before the first program.
"""
import argparse
import struct


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--p00")
    ap.add_argument("--t64")
    ap.add_argument("--bad-end", action="store_true")
    ap.add_argument("--empty-first", action="store_true")
    ap.add_argument("programs", nargs="+")
    a = ap.parse_args()
    programs = [open(p, "rb").read() for p in a.programs]
    if a.p00:
        with open(a.p00, "wb") as f:
            f.write(b"C64File\0" + b"TEST".ljust(16, b"\0") + b"\0\0" + programs[0])
    if a.t64:
        records = len(programs) + (1 if a.empty_first else 0)
        out = bytearray(b"C64S tape image file".ljust(32, b"\0"))
        out += struct.pack("<HHHH", 0x0100, records, len(programs), 0) + b"TEST".ljust(24, b" ")
        offset = 64 + 32 * records
        body = bytearray()
        if a.empty_first:
            out += bytes(32)
        for i, p in enumerate(programs):
            start = p[0] | (p[1] << 8)
            end = 0xC3C6 if a.bad_end else start + len(p) - 2
            out += struct.pack("<BBHHHIxxxx", 1, 0x82, start, end, 0, offset + len(body))
            out += f"TEST{i}".encode("ascii").ljust(16, b" ")
            body += p[2:]
        with open(a.t64, "wb") as f:
            f.write(out + body)


if __name__ == "__main__":
    main()

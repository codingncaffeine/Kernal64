#!/usr/bin/env python3
"""Wraps ROM images into a .crt cartridge file, for the project's own cartridge tests.

    tools/mkcrt.py -o OUT.crt --type N [--exrom 0|1] [--game 0|1] [--name TEXT]
                   [--header-length N] CHIP...

CHIP = FILE[@BANK[:LOAD]]   LOAD in hex (default 8000). A FILE ending in .prg carries a
two-byte load address, which is dropped (and used as LOAD when none is given).
The file format: a 64-byte header (signature, header length, version, hardware type, the
levels of the EXROM and GAME lines, name), then per chip a 16-byte packet header
("CHIP", packet length, chip type, bank, load address, size) and the bytes.
"""
import argparse
import struct


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--type", type=int, required=True)
    ap.add_argument("--exrom", type=int, default=0)
    ap.add_argument("--game", type=int, default=1)
    ap.add_argument("--name", default="TEST")
    ap.add_argument("--header-length", type=lambda v: int(v, 0), default=0x40,
                    help="the value of the header's length field (some files in the wild say 0x20)")
    ap.add_argument("chips", nargs="+")
    a = ap.parse_args()
    out = bytearray(b"C64 CARTRIDGE   ")
    out += struct.pack(">IHHBBB", a.header_length, 0x0100, a.type, a.exrom, a.game, 0) + bytes(5)
    out += a.name.encode("ascii")[:32].ljust(32, b"\0")
    for spec in a.chips:
        path, _, rest = spec.partition("@")
        bank, _, load = rest.partition(":")
        data = open(path, "rb").read()
        address = None
        if path.endswith(".prg"):
            address = data[0] | (data[1] << 8)
            data = data[2:]
        if load:
            address = int(load, 16)
        if address is None:
            address = 0x8000
        out += b"CHIP" + struct.pack(">IHHHH", len(data) + 0x10, 0, int(bank or 0), address, len(data)) + data
    with open(a.out, "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main()

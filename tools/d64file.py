#!/usr/bin/env python3
# Reads a D64 (35 tracks) the way the DOS lays it out, independently of the emulator: the
# directory chain from track 18 sector 1, then a file's sector chain.
#   d64file.py IMAGE            prints the program files' names, one per line
#   d64file.py IMAGE NAMEPREFIX prints the first matching program file's bytes as hex
#                               (with its two-byte load address)
#   d64file.py IMAGE NAMEPREFIX --relinked   the same as BASIC's LOAD leaves it at $0801: the
#                               line links rebuilt as the BASIC ROM does after a load ($A533:
#                               each link = the address after its line's terminating zero,
#                               until a link whose high byte is zero)
import sys
img = open(sys.argv[1], 'rb').read()
spt = lambda t: 21 if t < 18 else 19 if t < 25 else 18 if t < 31 else 17
def sec(t, s):
    off = sum(spt(x) for x in range(1, t)) * 256 + s * 256
    return img[off:off + 256]
t, s = 18, 1
while t:
    d = sec(t, s)
    for e in range(8):
        ent = d[e * 32:(e + 1) * 32]
        name = ent[5:21].rstrip(b'\xa0').decode('latin-1')
        if ent[2] & 7 == 2 and len(sys.argv) < 3:
            print(name)
        elif ent[2] & 7 == 2 and name.startswith(sys.argv[2]):
            ft, fs, out = ent[3], ent[4], b''
            while ft:
                b = sec(ft, fs)
                out += b[2:] if b[0] else b[2:b[1] + 1]
                ft, fs = b[0], b[1]
            if '--relinked' in sys.argv:
                mem = bytearray(out[2:])
                p = 0
                while p + 1 < len(mem) and mem[p + 1]:
                    q = p + 4
                    while q < len(mem) and mem[q]:
                        q += 1
                    link = 0x801 + q + 1
                    mem[p], mem[p + 1] = link & 255, link >> 8
                    p = q + 1
                out = out[:2] + bytes(mem)
            sys.stdout.write(out.hex()); sys.exit(0)
    t, s = d[0], d[1]
sys.exit(0 if len(sys.argv) < 3 else 1)

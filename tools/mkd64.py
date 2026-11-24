#!/usr/bin/env python3
# Writes a freshly formatted 35-track D64 (BAM layout from the 1541 manual), name K64, id 9A,
# empty or holding program files.
#   tools/mkd64.py OUT.d64 [NAME=FILE.prg ...]
# Each file's sectors are chained the DOS's way (a link to the next track/sector, the last
# sector's link = 0 and the index of its last byte), placed from track 17 down then 19 up at
# the 1541's interleave of 10, and listed in the directory chain from 18/1 (8 entries per
# sector) with its block count; the BAM marks them used. NAME is taken as PETSCII (upper case).
import sys
spt = lambda t: 21 if t <= 17 else 19 if t <= 24 else 18 if t <= 30 else 17
img = bytearray(sum(spt(t) for t in range(1, 36)) * 256)
off = lambda t, s: (sum(spt(x) for x in range(1, t)) + s) * 256
free = {t: set(range(spt(t))) for t in range(1, 36)}
free[18] -= {0, 1}

order = [t for t in range(17, 0, -1)] + [t for t in range(19, 36)]
def allocate(prev=None):
    tracks = order if prev is None else [prev[0]] + [t for t in order if t != prev[0]]
    for t in tracks:
        if not free[t]: continue
        n = spt(t)
        start = 0 if prev is None or t != prev[0] else (prev[1] + 10) % n
        for k in range(n):
            s = (start + k) % n
            if s in free[t]:
                free[t].discard(s)
                return t, s
    sys.exit("mkd64: disk full")

entries = []
for arg in sys.argv[2:]:
    name, path = arg.split('=', 1)
    data = open(path, 'rb').read()
    chunks = [data[i:i + 254] for i in range(0, len(data), 254)] or [b'']
    secs = []
    for _ in chunks: secs.append(allocate(secs[-1] if secs else None))
    for i, (t, s) in enumerate(secs):
        o = off(t, s)
        if i + 1 < len(secs): img[o:o + 2] = bytes(secs[i + 1])
        else: img[o:o + 2] = bytes([0, len(chunks[i]) + 1])
        img[o + 2:o + 2 + len(chunks[i])] = chunks[i]
    entries.append((name.upper().encode('latin-1')[:16], secs[0], len(secs)))

# Directory: 18/1, then further sectors of track 18 at the DOS's interleave of 3.
dsecs = [1]
while len(dsecs) * 8 < len(entries): dsecs.append((dsecs[-1] + 3) % 19)
for i, s in enumerate(dsecs):
    free[18].discard(s)
    o = off(18, s)
    img[o:o + 2] = bytes([18, dsecs[i + 1]]) if i + 1 < len(dsecs) else bytes([0, 0xFF])
    for j, (name, (t, ss), blocks) in enumerate(entries[i * 8:(i + 1) * 8]):
        e = o + j * 32
        img[e + 2] = 0x82   # PRG, closed
        img[e + 3:e + 5] = bytes([t, ss])
        img[e + 5:e + 21] = name.ljust(16, b'\xa0')
        img[e + 30:e + 32] = bytes([blocks & 255, blocks >> 8])

b = off(18, 0)
img[b:b + 4] = bytes([18, 1, 0x41, 0])
for t in range(1, 36):
    bits = sum(1 << s for s in free[t])
    img[b + 4 * t:b + 4 * t + 4] = bytes([len(free[t]), bits & 255, (bits >> 8) & 255, (bits >> 16) & 255])
img[b + 0x90:b + 0xab] = b"K64".ljust(16, b"\xa0") + b"\xa0\xa0" + b"9A" + b"\xa0" + b"2A" + b"\xa0" * 4
open(sys.argv[1], "wb").write(img)

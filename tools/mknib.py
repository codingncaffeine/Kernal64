#!/usr/bin/env python3
# Makes a NIB from a G64 the way a raw reader records a disk: per track, 8192 bytes read from an
# arbitrary point of the revolution, running on round it (for testing the NIB mount). Each
# track is first made 2 % longer in its longest gap, as on a disk mastered at another speed
# (the user's own NIB shows that), so a revolution is not the nominal length.
#   mknib.py IN.g64 OUT.nib [SEED]
import random, sys
g = open(sys.argv[1], 'rb').read()
rng = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
n = g[9]
header = bytearray(b'MNIB-1541-RAW' + bytes([1, 0, 0]))
entries, body = bytearray(), bytearray()
for i in range(0, n, 2):   # whole tracks only
    off = int.from_bytes(g[12 + 4 * i:16 + 4 * i], 'little')
    if not off:
        continue
    ln = g[off] | g[off + 1] << 8
    t = g[off + 2:off + 2 + ln]
    zone = int.from_bytes(g[12 + 4 * n + 4 * i:16 + 4 * n + 4 * i], 'little') & 3
    best, run, at = (0, 0), 0, 0
    for k, b in enumerate(t):
        run = run + 1 if b == 0x55 else 0
        if run > best[0]: best = (run, k)
    extra = ln * 2 // 100
    pos = best[1] - best[0] // 2
    t = t[:pos] + bytes([0x55]) * extra + t[pos:]
    ln = len(t)
    start = rng.randrange(ln)
    raw = (t[start:] + t * 3)[:8192]
    entries += bytes([i + 2, zone])
    body += raw
out = header + entries + bytes(0x100 - len(header) - len(entries)) + body
open(sys.argv[2], 'wb').write(out)

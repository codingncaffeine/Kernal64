#!/usr/bin/env python3
# Decodes every sector of a G64: one line per sector found, "track sector <sha1 of its 256
# bytes>", sorted (finds SYNCs bit by bit, decodes GCR itself; independent of the emulator).
import hashlib, sys
DEC = {0x0A:0,0x0B:1,0x12:2,0x13:3,0x0E:4,0x0F:5,0x16:6,0x17:7,0x09:8,0x19:9,0x1A:10,0x1B:11,0x0D:12,0x1D:13,0x1E:14,0x15:15}
def gcr(bits, at, nbytes):
    out = []
    for k in range(nbytes * 2):
        v = DEC.get(int(bits[at + 5 * k:at + 5 * k + 5], 2))
        if v is None: return None
        out.append(v)
    return bytes(out[k] << 4 | out[k + 1] for k in range(0, len(out), 2))
d = open(sys.argv[1], 'rb').read()
n = d[9]; found = set()
for i in range(0, n):
    off = int.from_bytes(d[12+4*i:16+4*i], 'little')
    if not off: continue
    ln = d[off] | d[off+1] << 8
    bits = ''.join(f'{b:08b}' for b in d[off+2:off+2+ln]); bits2 = bits + bits
    ends = []; j = 1
    while j < len(bits) + 1:
        if bits2[j] == '0' and bits2[j-10:j] == '1' * 10: ends.append(j)
        j += 1
    for k, e in enumerate(ends):
        h = gcr(bits2, e, 8)
        if not h or h[0] != 8: continue
        nxt = [x for x in ends if x > e] + [x + len(bits) for x in ends]
        de = min(x for x in nxt if x > e)
        blk = gcr(bits2 + bits2, de, 260)
        if blk and blk[0] == 7:
            data = blk[1:257]
            ok = blk[257] == (lambda x: x)(__import__('functools').reduce(lambda a, b: a ^ b, data, 0))
            found.add(f"{h[3]:2} {h[2]:2} {hashlib.sha1(data).hexdigest()[:12]}{'' if ok else ' BADSUM'}")
print('\n'.join(sorted(found)))

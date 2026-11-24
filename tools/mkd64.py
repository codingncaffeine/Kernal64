#!/usr/bin/env python3
# Writes a freshly formatted, empty 35-track D64 (BAM layout from the 1541 manual), name K64, id 9A.
#   tools/mkd64.py OUT.d64
import sys
spt=lambda t: 21 if t<=17 else 19 if t<=24 else 18 if t<=30 else 17
img=bytearray(sum(spt(t) for t in range(1,36))*256)
off=lambda t,s: (sum(spt(x) for x in range(1,t))+s)*256
b=off(18,0)
img[b:b+4]=bytes([18,1,0x41,0])
for t in range(1,36):
    n=spt(t); free=set(range(n))
    if t==18: free-={0,1}
    bits=sum(1<<s for s in free)
    img[b+4*t:b+4*t+4]=bytes([len(free),bits&255,(bits>>8)&255,(bits>>16)&255])
img[b+0x90:b+0xab]=b"K64".ljust(16,b"\xa0")+b"\xa0\xa0"+b"9A"+b"\xa0"+b"2A"+b"\xa0"*4
d=off(18,1); img[d:d+2]=bytes([0,0xff])
open(sys.argv[1],"wb").write(img)

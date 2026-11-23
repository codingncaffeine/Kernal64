#!/usr/bin/env python3
"""Builds the color calibration program for the reference emulator: a tokenized BASIC
program that puts a reversed space in each of the first 16 screen cells, colored 0-15,
then loops.

    calib.py OUT.prg
"""
import sys
# 10 FORI=0TO15:POKE1024+I,160:POKE55296+I,I:NEXT
# 20 GOTO20
FOR, TO, POKE, NEXT, GOTO = 0x81, 0xA4, 0x97, 0x82, 0x89  # "=" is token $B2, "+" $AA
def line(addr, num, body):
    nxt = addr + 4 + len(body) + 1
    return bytes([nxt & 255, nxt >> 8, num & 255, num >> 8]) + body + b'\0', nxt
b1 = bytes([FOR]) + b'I' + bytes([0xB2]) + b'0' + bytes([TO]) + b'15:' + bytes([POKE]) + b'1024' + bytes([0xAA]) + b'I,160:' + \
     bytes([POKE]) + b'55296' + bytes([0xAA]) + b'I,I:' + bytes([NEXT])
b2 = bytes([GOTO]) + b'20'
l1, a = line(0x0801, 10, b1)
l2, a = line(a, 20, b2)
open(sys.argv[1], 'wb').write(bytes([0x01, 0x08]) + l1 + l2 + b'\0\0')

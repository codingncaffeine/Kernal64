#!/usr/bin/env python3
# Tokenizes a BASIC V2 program (plain text, one numbered line per line) into a PRG at $0801,
# as the C64's BASIC stores it: per line a link to the next line, the line number, the
# tokens, a zero; two zeros at the end. Keywords are crunched as the ROM's tokenizer does
# (longest match from the keyword table's order, not inside quotes or after REM/DATA).
# {N} in the text = the byte N (decimal), for PETSCII codes that have no ASCII form.
#   tools/basic2prg.py IN.bas OUT.prg        (or "-" for standard input / output)
#   As a module: tokenize(text) -> bytes (with the load address).
import re
import sys

KEYWORDS = ("END FOR NEXT DATA INPUT# INPUT DIM READ LET GOTO RUN IF RESTORE GOSUB RETURN REM STOP ON "
            "WAIT LOAD SAVE VERIFY DEF POKE PRINT# PRINT CONT LIST CLR CMD SYS OPEN CLOSE GET NEW TAB( TO FN "
            "SPC( THEN NOT STEP + - * / ^ AND OR > = < SGN INT ABS USR FRE POS SQR RND LOG EXP COS SIN TAN ATN "
            "PEEK LEN STR$ VAL ASC CHR$ LEFT$ RIGHT$ MID$ GO").split()


def crunch(body: str) -> bytes:
    out = bytearray()
    i, quote, literal = 0, False, False
    while i < len(body):
        m = re.match(r'\{(\d+)\}', body[i:])
        if m:
            out.append(int(m.group(1)))
            i += len(m.group(0))
            continue
        c = body[i]
        if c == '"':
            quote = not quote
        if not quote and not literal:
            # The ROM tries the keywords in table order; the first that matches wins.
            for n, k in enumerate(KEYWORDS):
                if body.startswith(k, i):
                    out.append(0x80 + n)
                    i += len(k)
                    if k in ('REM', 'DATA'):
                        literal = True
                    break
            else:
                out.append(ord(c))
                i += 1
            continue
        out.append(ord(c))
        i += 1
    return bytes(out)


def tokenize(text: str, base: int = 0x0801) -> bytes:
    lines = []
    for raw in text.splitlines():
        raw = raw.strip()
        if not raw:
            continue
        m = re.match(r'(\d+)\s*(.*)', raw)
        if not m:
            sys.exit(f"basic2prg: not a numbered line: {raw}")
        lines.append((int(m.group(1)), crunch(m.group(2))))
    mem = bytearray()
    addr = base
    for number, body in lines:
        nxt = addr + 2 + 2 + len(body) + 1
        mem += bytes([nxt & 255, nxt >> 8, number & 255, number >> 8]) + body + b'\0'
        addr = nxt
    mem += b'\0\0'
    return bytes([base & 255, base >> 8]) + bytes(mem)


if __name__ == '__main__':
    src = sys.stdin.read() if sys.argv[1] == '-' else open(sys.argv[1]).read()
    prg = tokenize(src)
    if sys.argv[2] == '-':
        sys.stdout.buffer.write(prg)
    else:
        open(sys.argv[2], 'wb').write(prg)

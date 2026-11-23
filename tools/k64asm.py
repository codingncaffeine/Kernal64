#!/usr/bin/env python3
"""A small two-pass 6502 assembler for the project's own test programs.

    tools/k64asm.py SOURCE -o OUT.prg [-D NAME=VALUE]...

The output is a C64 program file: two bytes of load address, then the code.

Syntax, one statement per line, `;` starts a comment:
    name = expression        constant
    * = expression           set the program counter (the first one is the load address)
    label                    a name at the start of a line, optionally followed by `:`
    lda #$10 / sta $d020,x / jmp (vec) / bne label / asl a / asl      instructions
    .byte e, "text", ...     bytes; strings give their character codes
    .word e, ...             16-bit little-endian values
    .fill count, value       `count` copies of `value` (default 0)
    .basic                   a one-line BASIC program that starts the code after it (SYS)
    .if e / .else / .endif   conditional assembly (nesting allowed)
Expressions: numbers ($hex, %binary, decimal, 'c'), names, `*` (program counter),
+ - * / & | ^ << >>, unary - < > (low and high byte), parentheses.
An operand that is known in the first pass and fits a byte uses the zero-page form;
a forward reference uses the absolute form.
"""
import argparse
import re
import sys

IMP, ACC, IMM, ZP, ZPX, ZPY, ABS, ABX, ABY, IND, IZX, IZY, REL = range(13)

# mnemonic -> {mode: opcode}; the documented instruction set.
OPS = {}


def _add(table):
    for line in table.strip().splitlines():
        name, *pairs = line.split()
        OPS[name] = {globals()[m]: int(o, 16) for m, o in (p.split(":") for p in pairs)}


_add("""
adc IMM:69 ZP:65 ZPX:75 ABS:6d ABX:7d ABY:79 IZX:61 IZY:71
and IMM:29 ZP:25 ZPX:35 ABS:2d ABX:3d ABY:39 IZX:21 IZY:31
asl ACC:0a ZP:06 ZPX:16 ABS:0e ABX:1e
bcc REL:90
bcs REL:b0
beq REL:f0
bit ZP:24 ABS:2c
bmi REL:30
bne REL:d0
bpl REL:10
brk IMP:00
bvc REL:50
bvs REL:70
clc IMP:18
cld IMP:d8
cli IMP:58
clv IMP:b8
cmp IMM:c9 ZP:c5 ZPX:d5 ABS:cd ABX:dd ABY:d9 IZX:c1 IZY:d1
cpx IMM:e0 ZP:e4 ABS:ec
cpy IMM:c0 ZP:c4 ABS:cc
dec ZP:c6 ZPX:d6 ABS:ce ABX:de
dex IMP:ca
dey IMP:88
eor IMM:49 ZP:45 ZPX:55 ABS:4d ABX:5d ABY:59 IZX:41 IZY:51
inc ZP:e6 ZPX:f6 ABS:ee ABX:fe
inx IMP:e8
iny IMP:c8
jmp ABS:4c IND:6c
jsr ABS:20
lda IMM:a9 ZP:a5 ZPX:b5 ABS:ad ABX:bd ABY:b9 IZX:a1 IZY:b1
ldx IMM:a2 ZP:a6 ZPY:b6 ABS:ae ABY:be
ldy IMM:a0 ZP:a4 ZPX:b4 ABS:ac ABX:bc
lsr ACC:4a ZP:46 ZPX:56 ABS:4e ABX:5e
nop IMP:ea
ora IMM:09 ZP:05 ZPX:15 ABS:0d ABX:1d ABY:19 IZX:01 IZY:11
pha IMP:48
php IMP:08
pla IMP:68
plp IMP:28
rol ACC:2a ZP:26 ZPX:36 ABS:2e ABX:3e
ror ACC:6a ZP:66 ZPX:76 ABS:6e ABX:7e
rti IMP:40
rts IMP:60
sbc IMM:e9 ZP:e5 ZPX:f5 ABS:ed ABX:fd ABY:f9 IZX:e1 IZY:f1
sec IMP:38
sed IMP:f8
sei IMP:78
sta ZP:85 ZPX:95 ABS:8d ABX:9d ABY:99 IZX:81 IZY:91
stx ZP:86 ZPY:96 ABS:8e
sty ZP:84 ZPX:94 ABS:8c
tax IMP:aa
tay IMP:a8
tsx IMP:ba
txa IMP:8a
txs IMP:9a
tya IMP:98
""")

TOKEN = re.compile(r"\s*(\$[0-9a-fA-F]+|%[01]+|\d+|'.'|[A-Za-z_][A-Za-z0-9_]*|<<|>>|[-+*/&|^<>()])")


class AsmError(Exception):
    pass


class Undefined(Exception):
    pass


class Expr:
    """Recursive-descent evaluation of one expression."""

    def __init__(self, text, symbols, pc):
        self.toks = []
        pos = 0
        text = text.strip()
        while pos < len(text):
            m = TOKEN.match(text, pos)
            if not m:
                raise AsmError(f"cannot read expression: {text!r}")
            self.toks.append(m.group(1))
            pos = m.end()
        self.i = 0
        self.symbols = symbols
        self.pc = pc

    def peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else None

    def take(self):
        t = self.peek()
        self.i += 1
        return t

    def value(self):
        v = self.binary(0)
        if self.peek() is not None:
            raise AsmError(f"unexpected {self.peek()!r} in expression")
        return v

    LEVELS = [("|",), ("^",), ("&",), ("<<", ">>"), ("+", "-"), ("*", "/")]

    def binary(self, level):
        if level == len(self.LEVELS):
            return self.unary()
        v = self.binary(level + 1)
        while self.peek() in self.LEVELS[level]:
            op = self.take()
            r = self.binary(level + 1)
            v = {"|": v | r, "^": v ^ r, "&": v & r, "<<": v << r, ">>": v >> r, "+": v + r, "-": v - r,
                 "*": v * r, "/": v // r if r else 0}[op]
        return v

    def unary(self):
        t = self.take()
        if t is None:
            raise AsmError("expression ends early")
        if t == "-":
            return -self.unary()
        if t == "<":
            return self.unary() & 0xFF
        if t == ">":
            return (self.unary() >> 8) & 0xFF
        if t == "(":
            v = self.binary(0)
            if self.take() != ")":
                raise AsmError("missing )")
            return v
        if t == "*":
            return self.pc
        if t[0] == "$":
            return int(t[1:], 16)
        if t[0] == "%":
            return int(t[1:], 2)
        if t[0] == "'":
            return ord(t[1])
        if t[0].isdigit():
            return int(t)
        if t in self.symbols:
            return self.symbols[t]
        raise Undefined(t)


def split_args(text):
    """Splits on commas outside strings and parentheses."""
    out, depth, quote, cur = [], 0, False, ""
    for ch in text:
        if ch == '"':
            quote = not quote
        if not quote:
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            elif ch == "," and depth == 0:
                out.append(cur.strip())
                cur = ""
                continue
        cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def strip_comment(line):
    quote = False
    for i, ch in enumerate(line):
        if ch == '"':
            quote = not quote
        elif ch == ";" and not quote:
            return line[:i]
    return line


def assemble(source, defines):
    lines = source.splitlines()
    symbols = dict(defines)
    sizes = {}   # line number -> addressing mode chosen in pass 1 (must not change in pass 2)
    for final in (False, True):
        pc, origin, out = None, None, bytearray()
        cond = []   # stack of (active, seen_else)

        def ev(text):
            try:
                return Expr(text, symbols, pc).value()
            except Undefined as u:
                if final:
                    raise AsmError(f"undefined name {u}")
                raise

        def emit(*bs):
            nonlocal pc
            if pc is None:
                raise AsmError("no program counter: start with `* = address`")
            for b in bs:
                out.append(b & 0xFF)
            pc += len(bs)

        for n, raw in enumerate(lines, 1):
            try:
                line = strip_comment(raw).rstrip()
                if not line.strip():
                    continue
                stripped = line.strip()
                low = stripped.lower()
                if low.startswith(".if"):
                    active = all(a for a, _ in cond) and ev(stripped[3:]) != 0
                    cond.append((active, False))
                    continue
                if low == ".else":
                    a, _ = cond.pop()
                    cond.append((not a and all(x for x, _ in cond), True))
                    continue
                if low == ".endif":
                    cond.pop()
                    continue
                if not all(a for a, _ in cond):
                    continue
                m = re.match(r"^\*\s*=\s*(.+)$", stripped)
                if m:
                    target = ev(m.group(1))
                    if pc is None:
                        origin = target
                    else:
                        if target < pc:
                            raise AsmError("program counter moved backwards")
                        out.extend(bytes(target - pc))
                    pc = target
                    continue
                m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$", stripped)
                if m:
                    symbols[m.group(1)] = ev(m.group(2))
                    continue
                # A label: a name in column 0 that is not an instruction.
                if not line[0].isspace():
                    m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):?\s*(.*)$", stripped)
                    if m and m.group(1).lower() not in OPS:
                        if not final and m.group(1) in symbols and symbols[m.group(1)] != pc:
                            raise AsmError(f"name defined twice: {m.group(1)}")
                        symbols[m.group(1)] = pc
                        stripped = m.group(2)
                        low = stripped.lower()
                        if not stripped:
                            continue
                word, _, rest = stripped.partition(" ")
                word, rest = word.lower(), rest.strip()
                if word == ".byte":
                    for a in split_args(rest):
                        if a.startswith('"') and a.endswith('"'):
                            emit(*a[1:-1].encode("latin-1"))
                        else:
                            try:
                                emit(ev(a))
                            except Undefined:
                                emit(0)
                    continue
                if word == ".word":
                    for a in split_args(rest):
                        try:
                            v = ev(a)
                        except Undefined:
                            v = 0
                        emit(v, v >> 8)
                    continue
                if word == ".fill":
                    args = split_args(rest)
                    emit(*([ev(args[1]) if len(args) > 1 else 0] * ev(args[0])))
                    continue
                if word == ".basic":
                    # 10 SYS <address of the code after this line>
                    start = pc + 12
                    digits = str(start).encode()
                    if len(digits) != 4:
                        raise AsmError(".basic needs a four-digit start address")
                    link = pc + 10   # the end-of-program marker
                    emit(link, link >> 8, 10, 0, 0x9E, *digits, 0, 0, 0)
                    continue
                if word not in OPS:
                    raise AsmError(f"unknown instruction {word!r}")
                modes = OPS[word]
                operand = rest
                lowop = operand.lower().replace(" ", "")
                if not operand or lowop == "a":
                    mode = ACC if ACC in modes and (lowop == "a" or IMP not in modes) else IMP
                    if mode not in modes:
                        raise AsmError(f"{word} needs an operand")
                    emit(modes[mode])
                    continue
                if operand.startswith("#"):
                    try:
                        v = ev(operand[1:])
                    except Undefined:
                        v = 0
                    if final and not -128 <= v <= 255:
                        raise AsmError(f"immediate value out of range: {v}")
                    emit(modes[IMM], v)
                    continue
                if REL in modes:
                    try:
                        d = ev(operand) - (pc + 2)
                    except Undefined:
                        d = 0
                    if final and not -128 <= d <= 127:
                        raise AsmError(f"branch out of range ({d})")
                    emit(modes[REL], d)
                    continue
                m = re.match(r"^\((.+),\s*[xX]\s*\)$", operand)
                if m:
                    emit(modes[IZX], ev(m.group(1)) if final else 0)
                    continue
                m = re.match(r"^\((.+)\)\s*,\s*[yY]$", operand)
                if m:
                    emit(modes[IZY], ev(m.group(1)) if final else 0)
                    continue
                m = re.match(r"^\((.+)\)$", operand)
                if m and IND in modes:
                    v = ev(m.group(1)) if final else 0
                    emit(modes[IND], v, v >> 8)
                    continue
                index = None
                m = re.match(r"^(.+),\s*([xXyY])$", operand)
                if m:
                    operand, index = m.group(1), m.group(2).lower()
                zp, ab = {None: (ZP, ABS), "x": (ZPX, ABX), "y": (ZPY, ABY)}[index]
                try:
                    v = ev(operand)
                    known = True
                except Undefined:
                    v, known = 0, False
                if final:
                    mode = sizes[n]
                else:
                    mode = zp if known and 0 <= v <= 0xFF and zp in modes else ab
                    if mode not in modes:
                        mode = zp if zp in modes else None
                    if mode is None:
                        raise AsmError(f"{word} has no such addressing mode")
                    sizes[n] = mode
                if mode in (ZP, ZPX, ZPY):
                    if final and not 0 <= v <= 0xFF:
                        raise AsmError(f"zero-page operand out of range: ${v:x}")
                    emit(modes[mode], v)
                else:
                    emit(modes[mode], v, v >> 8)
            except Undefined:
                raise AsmError(f"line {n}: a name must be defined before this use: {raw.strip()}") from None
            except AsmError as e:
                raise AsmError(f"line {n}: {e}") from None
            except (IndexError, KeyError):
                raise AsmError(f"line {n}: cannot read: {raw.strip()}") from None
        if cond:
            raise AsmError("missing .endif")
    if origin is None:
        raise AsmError("empty program")
    return origin, bytes(out)


def main():
    ap = argparse.ArgumentParser(description="Assemble a 6502 source file into a C64 program file.")
    ap.add_argument("source")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("-D", action="append", default=[], metavar="NAME=VALUE")
    args = ap.parse_args()
    defines = {}
    for d in args.D:
        name, _, value = d.partition("=")
        defines[name] = Expr(value or "1", {}, 0).value()
    try:
        with open(args.source, encoding="utf-8") as f:
            origin, code = assemble(f.read(), defines)
    except AsmError as e:
        print(f"{args.source}: {e}", file=sys.stderr)
        return 1
    with open(args.output, "wb") as f:
        f.write(bytes([origin & 0xFF, origin >> 8]) + code)
    return 0


if __name__ == "__main__":
    sys.exit(main())

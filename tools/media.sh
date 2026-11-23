#!/usr/bin/env bash
# What a frontend can hand the core, checked through the machine (k64run) and through the
# built core's API (k64host): a program file, the same in a P00 wrapper and in T64
# containers (also one with the wrong end address an old tool wrote, one with a free
# record first, and one with two programs: the first is started), a cartridge file; and
# what must be refused: a cartridge of hardware that is not built, a cartridge file cut
# short, a file that is nothing.
# The program is tests/probes/autostart.s (it checks what LOAD leaves and writes
# "PRG PASS" to the screen); the cartridge is one of tools/carts.sh's (run that first).
#
#   tools/media.sh [BUILD-FOLDER]      default: bin
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$(realpath "${1:-$ROOT/bin}")"
OUT="$ROOT/scratch/media"
mkdir -p "$OUT"
fails=0
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
python3 -I "$ROOT/tools/k64asm.py" "$ROOT/tests/probes/autostart.s" -o "$OUT/a.prg" || { echo "FAIL the program does not assemble"; exit 1; }
# A second program that reports another value: the container's first program must win.
printf '\x01\x08\x0b\x08\x0a\x00\x9e2061\x00\x00\x00\xa9\x07\x8d\xff\xd7\x4c\x12\x08' >"$OUT/b.prg"
python3 -I "$ROOT/tools/mkt64.py" --p00 "$OUT/a.p00" "$OUT/a.prg"
python3 -I "$ROOT/tools/mkt64.py" --t64 "$OUT/a.t64" "$OUT/a.prg"
python3 -I "$ROOT/tools/mkt64.py" --t64 "$OUT/bad-end.t64" --bad-end "$OUT/a.prg"
python3 -I "$ROOT/tools/mkt64.py" --t64 "$OUT/free-first.t64" --empty-first "$OUT/a.prg"
python3 -I "$ROOT/tools/mkt64.py" --t64 "$OUT/two.t64" "$OUT/a.prg" "$OUT/b.prg"
python3 -I "$ROOT/tools/mkt64.py" --t64 "$OUT/two-reversed.t64" "$OUT/b.prg" "$OUT/a.prg"
CRT="$ROOT/scratch/carts/t19-b16.crt"
[ -f "$CRT" ] || { echo "FAIL no test cartridge: run tools/carts.sh first"; exit 1; }
# A header that gives its own length as $20, as some files do: the chips still start at $40.
bankfiles=(); for b in $(seq 0 15); do bankfiles+=("$ROOT/scratch/carts/t19-b16-$b.prg@$b"); done
python3 -I "$ROOT/tools/mkcrt.py" -o "$OUT/short-header.crt" --type 19 --header-length 0x20 "${bankfiles[@]}"
# Refused: hardware type 999; the cartridge file without its last 100 bytes; noise.
python3 -I "$ROOT/tools/mkcrt.py" -o "$OUT/unknown-type.crt" --type 999 "$ROOT/scratch/carts/t19-b16-0.prg"
head -c $(( $(stat -c %s "$CRT") - 100 )) "$CRT" >"$OUT/cut.crt"
printf 'C64 CARTRIDGE   ' >"$OUT/header-only.crt"
: >"$OUT/empty.prg"

# The machine: each form of the program runs and reports 0; the reversed container 7.
for f in a.prg a.p00 a.t64 bad-end.t64 free-first.t64 two.t64; do
    "$BIN/k64run" "$ROOT/gitignore" --autostart "$OUT/$f" --debugcart --cycles 6000000 >"$OUT/run.log" 2>&1
    got=$?; [ "$got" = 0 ] && echo "ok   machine: $f" || bad "machine: $f reports $got"
done
"$BIN/k64run" "$ROOT/gitignore" --autostart "$OUT/two-reversed.t64" --debugcart --cycles 6000000 >"$OUT/run.log" 2>&1
got=$?; [ "$got" = 7 ] && echo "ok   machine: two-reversed.t64 starts its first program" || bad "machine: two-reversed.t64 reports $got, expected 7"
"$BIN/k64run" "$ROOT/gitignore" --cart "$OUT/short-header.crt" --debugcart --cycles 3000000 >"$OUT/run.log" 2>&1
got=$?; [ "$got" = 0 ] && echo "ok   machine: short-header.crt" || bad "machine: short-header.crt reports $got"
for f in unknown-type.crt cut.crt header-only.crt; do
    "$BIN/k64run" "$ROOT/gitignore" --cart "$OUT/$f" --cycles 1000 >"$OUT/run.log" 2>&1
    got=$?; [ "$got" = 3 ] && echo "ok   machine: $f refused" || bad "machine: $f not refused (exit $got)"
done

# The core through its API: the same files as content; the conformance checks run with
# them (a state saved half-way must replay to the same state, cartridge banks included).
CORE="$BIN/kernal64_libretro.so"
for f in a.prg a.p00 a.t64 bad-end.t64; do
    "$BIN/k64host" "$CORE" --system "$ROOT/gitignore" --content "$OUT/$f" --frames 300 --check --expect-screen "PRG PASS" >"$OUT/host.log" 2>&1 \
        && echo "ok   core: $f" || { bad "core: $f"; tail -3 "$OUT/host.log"; }
done
"$BIN/k64host" "$CORE" --system "$ROOT/gitignore" --content "$CRT" --frames 120 --check --expect-screen "CART PASS" >"$OUT/host.log" 2>&1 \
    && echo "ok   core: cartridge" || { bad "core: cartridge"; tail -3 "$OUT/host.log"; }
# A cartridge that writes to its flash chips: a state saved in the middle of its erasing
# and programming holds the chips' contents, and the replay from it ends in the same state.
"$BIN/k64host" "$CORE" --system "$ROOT/gitignore" --content "$ROOT/scratch/carts/t32-b0.crt" --frames 100 --check >"$OUT/host.log" 2>&1 \
    && echo "ok   core: flash cartridge, state in the middle of its writes" || { bad "core: flash cartridge state"; tail -3 "$OUT/host.log"; }
# Without content the same checks must fail: the texts come from the content.
"$BIN/k64host" "$CORE" --system "$ROOT/gitignore" --frames 300 --expect-screen "PRG PASS" >"$OUT/host.log" 2>&1 \
    && bad "core: PRG PASS on the screen without a program" || echo "ok   core: no program, no text"
for f in unknown-type.crt cut.crt header-only.crt empty.prg; do
    "$BIN/k64host" "$CORE" --system "$ROOT/gitignore" --content "$OUT/$f" --frames 10 --expect-load-failure >"$OUT/host.log" 2>&1 \
        && echo "ok   core: $f refused" || { bad "core: $f not refused"; tail -3 "$OUT/host.log"; }
done
[ "$fails" = 0 ]

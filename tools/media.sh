#!/usr/bin/env bash
# What a frontend can hand the core, checked through the machine (k64run) and through the
# built core's API (k64host): a program file, the same in a P00 wrapper and in T64
# containers (also one with the wrong end address an old tool wrote, one with a free
# record first, and one with two programs: the first is started), a cartridge file; and
# what must be refused: a cartridge of hardware that is not built, a cartridge file cut
# short, a file that is nothing.
# A cartridge with a freeze button: the button through the API (a click of the RetroPad's
# right stick), and a state saved with a freeze on its way.
# The checks run side by side.
# The program is tests/probes/autostart.s (it checks what LOAD leaves and writes
# "PRG PASS" to the screen); the cartridge is one of tools/carts.sh's (run that first).
#
#   tools/media.sh [BUILD-FOLDER]      default: bin
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$(realpath "${1:-$ROOT/bin}")"
OUT="$ROOT/scratch/media"
mkdir -p "$OUT"
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

. "$ROOT/tools/sidebyside.sh"
CORE="$BIN/kernal64_libretro.so"
# machine WANT LABEL K64RUN-ARGUMENTS...: the run's exit status must be WANT.
machine() {
    local want="$1" label="$2" got; shift 2
    "$BIN/k64run" "$ROOT/gitignore" "$@" >/dev/null 2>&1
    got=$?
    if [ "$got" = "$want" ]; then echo "ok   machine: $label"; else echo "FAIL machine: $label (exit $got, expected $want)"; fi
}
# core WANT LABEL K64HOST-ARGUMENTS...: WANT = 0, the run must pass; 1, it must fail.
core() {
    local want="$1" label="$2" log got; shift 2
    log="$OUT/host-$(printf '%s' "$label" | tr -c 'A-Za-z0-9.-' '_').log"
    "$BIN/k64host" "$CORE" --system "$ROOT/gitignore" "$@" >"$log" 2>&1
    got=$?
    [ "$got" = 0 ] || got=1
    if [ "$got" = "$want" ]; then echo "ok   core: $label"; else echo "FAIL core: $label"; grep -m3 'FAIL' "$log"; fi
}

# The machine: each form of the program runs and reports 0; the reversed container 7.
for f in a.prg a.p00 a.t64 bad-end.t64 free-first.t64 two.t64; do
    sbs_start machine 0 "$f" --autostart "$OUT/$f" --debugcart --cycles 6000000
done
sbs_start machine 7 "two-reversed.t64 starts its first program" --autostart "$OUT/two-reversed.t64" --debugcart --cycles 6000000
sbs_start machine 0 "short-header.crt" --cart "$OUT/short-header.crt" --debugcart --cycles 3000000
for f in unknown-type.crt cut.crt header-only.crt; do
    sbs_start machine 3 "$f refused" --cart "$OUT/$f" --cycles 1000
done

# The core through its API: the same files as content; the conformance checks run with
# them (a state saved half-way must replay to the same state, cartridge banks included,
# also in a core that was shut down and started again).
for f in a.prg a.p00 a.t64 bad-end.t64; do
    sbs_start core 0 "$f" --content "$OUT/$f" --frames 300 --check --expect-screen "PRG PASS"
done
sbs_start core 0 "cartridge" --content "$CRT" --frames 120 --check --expect-screen "CART PASS"
# A cartridge that writes to its flash chips: a state saved in the middle of its erasing
# and programming holds the chips' contents, and the replay from it ends in the same state.
sbs_start core 0 "flash cartridge, state in the middle of its writes" --content "$ROOT/scratch/carts/t32-b0.crt" --frames 100 --check
# A cartridge with a freeze button (tests/carts/freezer.s): the button pressed three
# times, as the program's steps need it; without the presses the program must not pass;
# a state saved between the second press and the third replays to the same end.
FRZ="$ROOT/scratch/carts/t1-b4.crt"
sbs_start core 0 "freeze button" --content "$FRZ" --frames 60 --freeze-frames 3,9,15 --expect-screen "CART PASS"
sbs_start core 1 "no freeze without the button" --content "$FRZ" --frames 60 --expect-screen "CART PASS"
sbs_start core 0 "freezer cartridge, state between two freezes" --content "$FRZ" --frames 24 --freeze-frames 3,9,15 --check
# Without content the same checks must fail: the texts come from the content.
sbs_start core 1 "no program, no text" --frames 300 --expect-screen "PRG PASS"
for f in unknown-type.crt cut.crt header-only.crt empty.prg; do
    sbs_start core 0 "$f refused" --content "$OUT/$f" --frames 10 --expect-load-failure
done
sbs_finish

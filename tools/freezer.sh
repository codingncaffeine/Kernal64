#!/usr/bin/env bash
# The freeze of hardware type 36 against real cartridges. The reference suite has a test
# program for it (C64/carts/rr-freeze: it maps what each memory area shows for every bank
# and register value after reset, after the second register was written, in the freeze and
# after it) and the dumps people saved with it on their cartridges. This runs the program
# on our machine for the four values of the second register the dumps cover, presses the
# freeze button when the program asks for it, and compares our results with the dumps of
# both boards byte by byte (tools/freezecmp.py): the first board (one dump set in each of
# the program's two formats) and the later one with the extra RAM mode (three sets).
# The eight runs go side by side.
#
#   tools/freezer.sh [K64RUN]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
K64RUN="$(realpath "${1:-$ROOT/bin/k64run}")"
T="$ROOT/gitignore/external/testprogs/C64/carts/rr-freeze"
OUT="$ROOT/scratch/freezer"
mkdir -p "$OUT"
[ -f "$T/rr-freeze.crt" ] || { echo "FAIL no test program: run tools/oracle/build-vice.sh"; exit 1; }
. "$ROOT/tools/sidebyside.sh"
# KEY: what is typed in the program's menu to choose VALUE for the second register. The
# program asks for the button after 1.5 million cycles and is done 1.5 million after it.
one_run() {
    local key="$1" value="$2" board="$3" log dumps d name
    name=first; [ "$board" = nr ] && name=later   # the suite's file names for the two boards
    log="$OUT/$board-$value.log"
    "$K64RUN" "$ROOT/gitignore/bios" --cart "$T/$board-freeze.crt" --cycles 5000000 --type-at 500000 "$key\\r" \
        --freeze-at 2500000 --freeze-hold 2000 --dump 0800:534 >"$log" 2>&1
    grep -q '^ACKD' "$log" && grep -q 'SAVE TO DISK' "$log" || { echo "FAIL $name board, second register \$$value: the program did not get through its freeze"; return; }
    if [ "$board" = rr ]; then dumps="RR-FRZ${value}_cz0 dump_rr_${value}_impetigo"; else dumps="RR-FRZ${value}_gpz_nr dump_nr_${value}_gpz dump_nr_${value}_cz"; fi
    for d in $dumps; do
        if python3 -I "$ROOT/tools/freezecmp.py" "$log" "$T/dumps/$d.prg" >"$OUT/$board-$value-$d.txt" 2>&1; then
            echo "ok   $name board, second register \$$value: as the cartridge behind $d"
        else
            echo "FAIL $name board, second register \$$value: not as the cartridge behind $d"
            head -13 "$OUT/$board-$value-$d.txt"
        fi
    done
}
for pair in "1 40" "2 00" "3 42" "4 02"; do
    read -r key value <<<"$pair"
    for board in rr nr; do sbs_start one_run "$key" "$value" "$board"; done
done
sbs_finish

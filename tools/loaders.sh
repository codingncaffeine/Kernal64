#!/usr/bin/env bash
# Open-source fast loaders through drive 8, on PAL and NTSC: a disk built with the loader's own
# tool loads seeded data in several jobs and a checker sums what arrived (exit status = the
# verdict). Loader releases live in gitignore/external/loaders/ (Spindle 3.1, MIT, from
# tools/fetch.sh; Bitfire, BSD-3, built by tools/loaders/build.sh).
#   tools/loaders.sh [K64RUN]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
K64RUN="${1:-$ROOT/bin/k64run}"
SPIN="$ROOT/gitignore/external/loaders/spindle-3.1/spindle-3.1/prebuilt-binaries/linux-x86_64/spin"
[ -x "$SPIN" ] || { echo "loaders: $SPIN missing"; exit 2; }
OUT="$(mktemp -d "${TMPDIR:-/tmp}/k64loaders.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
python3 -I "$ROOT/tests/loaders/mkspindle.py" "$OUT" || exit 2
python3 -I "$ROOT/tools/k64asm.py" -o "$OUT/checker.prg" "$OUT/checker.s" || exit 2
(cd "$OUT" && "$SPIN" -o spindle.d64 script >spin.log 2>&1) || { echo "loaders: spin failed"; cat "$OUT/spin.log"; exit 2; }
rc=0
for model in pal ntsc; do
    "$K64RUN" "$ROOT/gitignore/bios" --model "$model" --disk "$OUT/spindle.d64" --debugcart --cycles 40000000 \
        --type-at 4000000 'LOAD"*",8' --type-at 4300000 $'\r' --type-at 9000000 $'RUN\r' >"$OUT/$model.log" 2>&1
    r=$?
    if [ $r = 0 ]; then echo "spindle ($model): every job intact"
    else echo "FAIL spindle ($model): exit $r"; grep -v '^ *$' "$OUT/$model.log" | tail -4; rc=1; fi
done
# Bitfire (BSD-3, built by tools/loaders/build.sh): its author's checksum benchmark loads its
# 22 files over and over; a file whose checksum differs stops it. Verdict: at least two whole
# runs counted (runs at $10-$11) and no error counted ($24-$25).
BF="$ROOT/gitignore/external/loaders/bitfire/benchmark"
if [ -s "$BF/k64-bench.d64" ] && [ -s "$BF/benchmark.prg" ]; then
    for model in pal ntsc; do
        "$K64RUN" "$ROOT/gitignore/bios" --model "$model" --drive --disk "$BF/k64-bench.d64" --autostart "$BF/benchmark.prg" \
            --cycles 160000000 --type-at 6000000 $'SYS2304\r' --dump 0010:2 --dump 0024:2 >"$OUT/bf-$model.log" 2>&1
        read -r _ hi lo < <(grep '^0010:' "$OUT/bf-$model.log") || true
        runs=$(( 16#${hi:-0} * 256 + 16#${lo:-0} ))
        errs=$(grep '^0024:' "$OUT/bf-$model.log" | awk '{print $2 $3}')
        if [ "${runs:-0}" -ge 2 ] && [ "$errs" = 0000 ]; then echo "bitfire ($model): $runs runs of 22 files, checksums intact"
        else echo "FAIL bitfire ($model): runs ${runs:-?}, errors ${errs:-?}"; rc=1; fi
    done
else
    echo "FAIL bitfire: not built (tools/loaders/build.sh)"; rc=1
fi
exit $rc

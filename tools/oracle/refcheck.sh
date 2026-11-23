#!/usr/bin/env bash
# Compares the start-up screen with the reference emulator on all three machine models:
# our whole raster (k64run --raster) against its whole-raster screenshot after the same
# number of cycles from power on.
#
#   tools/oracle/refcheck.sh K64RUN_BINARY OUT_DIR
#
# Needs tools/oracle/build-vice.sh to have run. Each model is compared at several moments,
# far enough apart that the KERNAL's cursor (driven by the CIA timer interrupt) has blinked
# many times: every pixel must be identical at each one.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
K64RUN="$1"; OUT="$2"
mkdir -p "$OUT"
rel="${OUT#"$ROOT"/}"
[ "$rel" != "$OUT" ] || { echo "refcheck: OUT_DIR must be inside the repository" >&2; exit 2; }
fails=0
# model | reference model | alignment dx dy
for row in "pal|c64|492|0" "ntsc|ntsc|508|20" "ntsc-old|oldntsc|500|20"; do
    IFS='|' read -r model vmodel dx dy <<<"$row"
    for frames in 150 171 203 260 333 401; do
        cycles="$("$K64RUN" "$ROOT/gitignore" --frames "$frames" --model "$model" --raster "$OUT/$model-$frames.raw" |
                  sed -n 's/^k64run: [0-9]* frames, \([0-9]*\) cycles.*/\1/p')"
        rm -f "$OUT/ref-$model-$frames.png"
        K64_VICE_MODEL="$vmodel" timeout 300 "$ROOT/tools/oracle/vice.sh" -limitcycles "$cycles" \
            -exitscreenshot "/k64/$rel/ref-$model-$frames.png" -VICIIborders 2 >"$OUT/vice-$model.log" 2>&1 || true
        [ -s "$OUT/ref-$model-$frames.png" ] || { echo "refcheck: no reference frame for $model at $cycles cycles"; fails=$((fails + 1)); continue; }
        if python3 -I "$ROOT/tools/oracle/framecmp.py" "$OUT/$model-$frames.raw" "$OUT/ref-$model-$frames.png" --dx "$dx" --dy "$dy" \
            --colors "vice-colors-$model.txt" --diff "$OUT/diff-$model-$frames.png" >"$OUT/cmp-$model-$frames.log"; then
            echo "refcheck: $model after $frames frames ($cycles cycles) identical"
        else
            echo "refcheck: $model after $frames frames ($cycles cycles):"; head -4 "$OUT/cmp-$model-$frames.log"; fails=$((fails + 1))
        fi
    done
done
exit $((fails > 0))

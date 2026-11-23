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
# The reference's frame for a model and cycle count only changes with the pinned reference
# build: it is kept in gitignore/external/suite-ref-shots/ (K64_SUITE_REFRESH=1 asks the
# reference again, as for tools/suite.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
K64RUN="$1"; OUT="$2"
mkdir -p "$OUT"
rel="${OUT#"$ROOT"/}"
[ "$rel" != "$OUT" ] || { echo "refcheck: OUT_DIR must be inside the repository" >&2; exit 2; }
SHOTS="$ROOT/gitignore/external/suite-ref-shots"
mkdir -p "$SHOTS"
fails=0
# model | reference model | alignment dx dy
for row in "pal|c64|492|0" "ntsc|ntsc|508|20" "ntsc-old|oldntsc|500|20"; do
    IFS='|' read -r model vmodel dx dy <<<"$row"
    for frames in 150 171 203 260 333 401; do
        cycles="$("$K64RUN" "$ROOT/gitignore" --frames "$frames" --model "$model" --raster "$OUT/$model-$frames.raw" |
                  sed -n 's/^k64run: [0-9]* frames, \([0-9]*\) cycles.*/\1/p')"
        rm -f "$OUT/ref-$model-$frames.png"
        kept="$SHOTS/startup-$vmodel-$cycles.png"
        if [ ! -s "$kept" ] || [ "${K64_SUITE_REFRESH:-0}" = 1 ]; then
            K64_VICE_MODEL="$vmodel" timeout 300 "$ROOT/tools/oracle/vice.sh" -limitcycles "$cycles" \
                -exitscreenshot "/k64/$rel/ref-$model-$frames.png" -VICIIborders 2 >"$OUT/vice-$model.log" 2>&1 || true
            [ -s "$OUT/ref-$model-$frames.png" ] && cp "$OUT/ref-$model-$frames.png" "$kept"
        fi
        [ -s "$kept" ] || { echo "refcheck: no reference frame for $model at $cycles cycles"; fails=$((fails + 1)); continue; }
        cp "$kept" "$OUT/ref-$model-$frames.png"
        if python3 -I "$ROOT/tools/oracle/framecmp.py" "$OUT/$model-$frames.raw" "$OUT/ref-$model-$frames.png" --dx "$dx" --dy "$dy" \
            --colors "vice-colors-$model.txt" --diff "$OUT/diff-$model-$frames.png" >"$OUT/cmp-$model-$frames.log"; then
            echo "refcheck: $model after $frames frames ($cycles cycles) identical"
        else
            echo "refcheck: $model after $frames frames ($cycles cycles):"; head -4 "$OUT/cmp-$model-$frames.log"; fails=$((fails + 1))
        fi
    done
done
exit $((fails > 0))

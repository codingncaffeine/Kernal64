#!/usr/bin/env bash
# Compares the start-up screen with the reference emulator on all three machine models:
# our whole raster (k64run --raster) against its whole-raster screenshot after the same
# number of cycles from power on.
#
#   tools/oracle/refcheck.sh K64RUN_BINARY OUT_DIR
#
# Needs tools/oracle/build-vice.sh to have run. Known deviation until Phase 4 (the CIAs):
# the KERNAL's cursor does not blink here, so the cursor cell (the line under READY.) is
# masked; every other pixel must be identical.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
K64RUN="$1"; OUT="$2"
mkdir -p "$OUT"
rel="${OUT#"$ROOT"/}"
[ "$rel" != "$OUT" ] || { echo "refcheck: OUT_DIR must be inside the repository" >&2; exit 2; }
fails=0
# model | reference model | cycles (150 frames) | alignment dx dy | cursor cell in reference pixels
for row in "pal|c64|2947644|492|0|136,99,143,106" "ntsc|ntsc|2548000|508|20|136,79,143,86" \
           "ntsc-old|oldntsc|2499264|500|20|136,79,143,86"; do
    IFS='|' read -r model vmodel cycles dx dy cursor <<<"$row"
    "$K64RUN" "$ROOT/gitignore" --frames 150 --model "$model" --raster "$OUT/$model.raw" >/dev/null
    rm -f "$OUT/ref-$model.png"
    K64_VICE_MODEL="$vmodel" timeout 300 "$ROOT/tools/oracle/vice.sh" -limitcycles "$cycles" \
        -exitscreenshot "/k64/$rel/ref-$model.png" -VICIIborders 2 >"$OUT/vice-$model.log" 2>&1 || true
    [ -s "$OUT/ref-$model.png" ] || { echo "refcheck: no reference frame for $model"; fails=$((fails + 1)); continue; }
    colors="vice-colors-$model.txt"
    if python3 -I "$ROOT/tools/oracle/framecmp.py" "$OUT/$model.raw" "$OUT/ref-$model.png" --dx "$dx" --dy "$dy" \
        --colors "$colors" --mask "$cursor" --diff "$OUT/diff-$model.png" >"$OUT/cmp-$model.log"; then
        echo "refcheck: $model identical"
    else
        head -4 "$OUT/cmp-$model.log"; fails=$((fails + 1))
    fi
done
exit $((fails > 0))

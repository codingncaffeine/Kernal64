#!/usr/bin/env bash
# Assembles the project's own raster-effect programs (tests/effects/*.s, one per effect of
# the VIC-II article's section 3.14), runs each variant listed in tests/effects/expected.txt
# on our machine and compares the whole raster with the one recorded when it was identical
# to the reference emulator's picture.
#
#   tools/effects.sh [K64RUN]
#
# K64_EFFECTS_REF=1 also takes the reference emulator's picture again and compares pixel by
# pixel (needed whenever a line is added or changed; a line with the hash "?" prints the
# hash to record once the reference agrees).
#
# expected.txt: SOURCE  DEFINES(comma separated, or -)  MODEL  CYCLES-TO-RUN  HASH  [own]
# "own": not compared with the reference, which does not show what the line tests.
# The assembler gets CYCLES (cycles per raster line of the model) besides the defines.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
K64RUN="$(realpath "${1:-$ROOT/bin/k64run}")"
OUT="$ROOT/scratch/effects"
mkdir -p "$OUT"
. "$ROOT/tools/sidebyside.sh"
one_line() {
    local src="$1" defs="$2" model="$3" cycles="$4" want="$5" own="$6"
    local tag per dx dy fam vm vextra args dv d got line
    tag="$(printf "%s" "$src-$defs-$model" | tr -c 'A-Za-z0-9.-' '_')"
    case "$model" in
        pal|pal-new|pal-old) per=63; dx=492; dy=0; fam=pal; vm=c64 ;;
        ntsc|ntsc-new) per=65; dx=508; dy=20; fam=ntsc; vm=ntsc ;;
        ntsc-old) per=64; dx=500; dy=20; fam=ntsc-old; vm=oldntsc ;;
        *) echo "FAIL $src: unknown model $model"; return ;;
    esac
    vextra=()
    case "$model" in pal-new) vextra=(-VICIImodel 8565) ;; ntsc-new) vextra=(-VICIImodel 8562) ;; pal-old) vextra=(-VICIImodel 6569r1) ;; esac
    args=(-D "CYCLES=$per")
    if [ "$defs" != "-" ]; then IFS=',' read -ra dv <<<"$defs"; for d in "${dv[@]}"; do args+=(-D "$d"); done; fi
    if ! python3 -I "$ROOT/tools/k64asm.py" "$ROOT/tests/effects/$src" -o "$OUT/$tag.prg" --labels "$OUT/$tag.labels" "${args[@]}"; then
        echo "FAIL $src [$defs]: does not assemble"; return
    fi
    rm -f "$OUT/$tag.raw"
    "$K64RUN" "$ROOT/gitignore/bios" --model "$model" --autostart "$OUT/$tag.prg" --cycles "$cycles" --raster "$OUT/$tag.raw" \
        --png "$OUT/$tag.png" >/dev/null 2>&1
    got="$(sha256sum "$OUT/$tag.raw" 2>/dev/null | cut -c1-16)"
    line="$src [$defs] $model: picture ${got:-none}, recorded $want"
    if [ "${K64_EFFECTS_REF:-0}" = 1 ] && [ "$own" != own ]; then
        rm -f "$OUT/$tag.ref.png"
        K64_VICE_MODEL="$vm" timeout 300 "$ROOT/tools/oracle/vice.sh" "${vextra[@]}" -ciamodel 0 -jamaction 1 -warp -VICIIborders 2 \
            -limitcycles "$cycles" -autostartprgmode 1 -autostart "/k64/${OUT#"$ROOT"/}/$tag.prg" \
            -exitscreenshot "/k64/${OUT#"$ROOT"/}/$tag.ref.png" >"$OUT/$tag.ref.log" 2>&1
        if [ ! -s "$OUT/$tag.ref.png" ]; then echo "FAIL $line (no picture from the reference)"; return; fi
        if ! python3 -I "$ROOT/tools/oracle/framecmp.py" "$OUT/$tag.raw" "$OUT/$tag.ref.png" --dx $dx --dy $dy \
               --colors "vice-colors-$fam.txt" --diff "$OUT/$tag.diff.png" >"$OUT/$tag.cmp.log" 2>&1; then
            echo "FAIL $line (reference: $(grep -o '[0-9]* pixels differ' "$OUT/$tag.cmp.log" | tail -1))"; return
        fi
        line="$line, identical to the reference"
    fi
    if [ "$got" = "$want" ]; then echo "ok   $line"; else echo "FAIL $line"; fi
}
while read -r src defs model cycles want own; do
    case "$src" in ''|'#'*) continue ;; esac
    sbs_start one_line "$src" "$defs" "$model" "$cycles" "$want" "$own"
done <"$ROOT/tests/effects/expected.txt"
sbs_finish

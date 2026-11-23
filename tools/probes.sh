#!/usr/bin/env bash
# Assembles the project's own probe programs (tests/probes/*.s, tools/k64asm.py), runs each
# variant listed in tests/probes/expected.txt on our machine and compares the value the
# program reports through the debug cartridge with the value the reference emulator gave
# when the line was written (K64_PROBES_REF=1 asks the reference again and compares too).
#
#   tools/probes.sh [K64RUN]
#
# expected.txt: SOURCE  DEFINES(comma separated, or -)  MODEL  CIA  EXPECTED  [K64RUN OPTIONS]
# A line with options for our machine (something plugged into a port) has no counterpart
# in the reference run and is not asked there.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
K64RUN="$(realpath "${1:-$ROOT/bin/k64run}")"
OUT="$ROOT/scratch/probes"
mkdir -p "$OUT"
. "$ROOT/tools/sidebyside.sh"
one_line() {
    local src="$1" defs="$2" model="$3" cia="$4" want="$5" extra="$6"
    local tag args dv d got line vextra vm ref
    tag="$(echo "$src-$defs-$model-$cia" | tr -c 'A-Za-z0-9.-' '_')"
    args=()
    if [ "$defs" != "-" ]; then IFS=',' read -ra dv <<<"$defs"; for d in "${dv[@]}"; do args+=(-D "$d"); done; fi
    if ! python3 -I "$ROOT/tools/k64asm.py" "$ROOT/tests/probes/$src" -o "$OUT/$tag.prg" "${args[@]}"; then
        echo "FAIL $src [$defs]: does not assemble"; return
    fi
    # shellcheck disable=SC2086
    "$K64RUN" "$ROOT/gitignore" --model "$model" --cia "$cia" $extra --autostart "$OUT/$tag.prg" --debugcart --cycles 20000000 >/dev/null 2>&1
    got=$?
    line="$src [$defs] $model cia-$cia${extra:+ $extra}: ours $got, expected $want"
    if [ "${K64_PROBES_REF:-0}" = 1 ] && [ -z "$extra" ]; then
        vextra=()
        case "$model" in
            pal) vm=c64 ;; pal-new) vm=c64; vextra=(-VICIImodel 8565) ;; pal-old) vm=c64; vextra=(-VICIImodel 6569r1) ;;
            ntsc) vm=ntsc ;; ntsc-new) vm=ntsc; vextra=(-VICIImodel 8562) ;; *) vm=oldntsc ;;
        esac
        K64_VICE_MODEL="$vm" timeout 300 "$ROOT/tools/oracle/vice.sh" "${vextra[@]}" -ciamodel "$([ "$cia" = new ] && echo 1 || echo 0)" \
            -debugcart -jamaction 1 -warp -limitcycles 20000000 -autostartprgmode 1 \
            -autostart "/k64/${OUT#"$ROOT"/}/$tag.prg" >/dev/null 2>&1
        ref=$?
        line="$line, reference $ref"
        [ "$ref" = "$want" ] || { echo "FAIL $line (the reference no longer gives the expected value)"; return; }
    fi
    if [ "$got" = "$want" ]; then echo "ok   $line"; else echo "FAIL $line"; fi
}
while read -r src defs model cia want extra; do
    case "$src" in ''|'#'*) continue ;; esac
    sbs_start one_line "$src" "$defs" "$model" "$cia" "$want" "$extra"
done <"$ROOT/tests/probes/expected.txt"
sbs_finish

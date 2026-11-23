#!/usr/bin/env bash
# Builds the project's own test cartridges (tests/carts/banks.s, one image per bank, packed
# by tools/mkcrt.py), runs each line of tests/carts/expected.txt on our machine and
# compares the value the cartridge reports through the debug cartridge register with the
# expected one (K64_CARTS_REF=1 also asks the reference emulator, which must report the
# same).
#
#   tools/carts.sh [K64RUN]
#
# expected.txt: TYPE  BANKS  EXPECTED  [HEADER-TYPE EXROM GAME [REFERENCE]]
# TYPE is the hardware the test program is written for; a line that names another type for
# the file's header plugs the same chips into other hardware, where the check must fail
# at a known step. REFERENCE = "refuses": the reference emulator does not take a file with
# these chips for that type (nothing to compare).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
K64RUN="$(realpath "${1:-$ROOT/bin/k64run}")"
OUT="$ROOT/scratch/carts"
mkdir -p "$OUT"
asm() { python3 -I "$ROOT/tools/k64asm.py" "$ROOT/tests/carts/banks.s" -o "$1" -D "TYPE=$2" -D "BANK=$3" -D "NBANKS=$4" -D "PART=$5"; }
fails=0
while read -r type banks want htype exrom game refwant; do
    case "$type" in ''|'#'*) continue ;; esac
    tag="t$type-b$banks${htype:+-as$htype}"
    chips=()
    ok=1
    for ((b = 0; b < banks; b++)); do
        case "$type" in
            4|18) [ "$b" = 0 ] || break ;;   # one chip at $8000
        esac
        asm "$OUT/$tag-$b.prg" "$type" "$b" "$banks" 0 || ok=0
        n=$b; load=8000
        [ "$type" = 7 ] && n=$(( ((b & 7) << 3) | (b >> 3) ))   # the file names the register value
        [ "$type" = 5 ] && [ "$banks" = 32 ] && [ "$b" -ge 16 ] && load=a000
        chips+=("$OUT/$tag-$b.prg@$n:$load")
    done
    case "$type" in
        4) asm "$OUT/$tag-h0.prg" "$type" 0 1 1 || ok=0; chips+=("$OUT/$tag-h0.prg@0:a000") ;;
        18) for b in 0 1; do asm "$OUT/$tag-h$b.prg" "$type" "$b" 2 1 || ok=0; chips+=("$OUT/$tag-h$b.prg@$b:a000"); done ;;
    esac
    if [ "$ok" = 0 ]; then echo "FAIL type $type, $banks bank(s): does not assemble"; fails=$((fails + 1)); continue; fi
    python3 -I "$ROOT/tools/mkcrt.py" -o "$OUT/$tag.crt" --type "${htype:-$type}" --exrom "${exrom:-0}" --game "${game:-1}" "${chips[@]}"
    "$K64RUN" "$ROOT/gitignore" --cart "$OUT/$tag.crt" --debugcart --cycles 3000000 >"$OUT/$tag.log" 2>&1
    got=$?
    line="type $type, $banks bank(s)${htype:+ in type $htype hardware}: ours $got, expected $want"
    if [ "${K64_CARTS_REF:-0}" = 1 ]; then
        timeout 300 "$ROOT/tools/oracle/vice.sh" -debugcart -jamaction 1 -warp -limitcycles 3000000 \
            -cartcrt "/k64/${OUT#"$ROOT"/}/$tag.crt" >"$OUT/$tag.ref.log" 2>&1
        # Its exit status 1 is also its cycle limit: the reported value is in its log.
        ref="$(tr -d '\r' <"$OUT/$tag.ref.log" | sed -n 's/.*DBGCART: exit(\([0-9]*\)).*/\1/p' | tail -1)"
        [ -n "$ref" ] || ref=124
        grep -q 'could not attach' "$OUT/$tag.ref.log" && ref=refuses
        line="$line, reference $ref"
        [ "$ref" = "${refwant:-$want}" ] || { echo "FAIL $line (the reference does not give the expected value)"; fails=$((fails + 1)); continue; }
    fi
    if [ "$got" = "$want" ]; then echo "ok   $line"; else echo "FAIL $line"; fails=$((fails + 1)); fi
done <"$ROOT/tests/carts/expected.txt"
[ "$fails" = 0 ]

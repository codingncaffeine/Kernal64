#!/usr/bin/env bash
# Builds the project's own test cartridges (tests/carts/banks.s, one image per bank, and
# tests/carts/flash.s for the flash type; packed by tools/mkcrt.py), runs each line of tests/carts/expected.txt on our machine and
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
# these chips for that type (nothing to compare); a number: the value the reference
# reports where it departs from the chip's data sheet.
# TYPE 32 (flash chips): BANKS is 0 for the checks both agree on, 1 to add the data-sheet-only
# checks (flash.s STRICT).
# TYPE 1, 9, 36 (a freeze button; tests/carts/freezer.s): the button is pressed when the
# program reaches its labels press1.. and held for 2000 cycles. BANKS 4 = the freeze
# checks, 0 = type 36 with the function switched off. The reference emulator is not asked:
# its button cannot be pressed at a chosen moment. (Type 3's line in banks.s ends with its
# button too: there the reference is asked, and stops at that step.)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
K64RUN="$(realpath "${1:-$ROOT/bin/k64run}")"
OUT="$ROOT/scratch/carts"
mkdir -p "$OUT"
asm() { python3 -I "$ROOT/tools/k64asm.py" "$ROOT/tests/carts/banks.s" -o "$1" -D "TYPE=$2" -D "BANK=$3" -D "NBANKS=$4" -D "PART=$5" --labels "$1.labels"; }
. "$ROOT/tools/sidebyside.sh"
one_line() {
    local type="$1" banks="$2" want="$3" htype="$4" exrom="$5" game="$6" refwant="$7"
    local tag chips ok cycles extra noref nofreeze strict fbanks b n load at label got line ref
    tag="t$type-b$banks${htype:+-as$htype}"
    chips=()
    ok=1
    cycles=3000000
    extra=()
    noref=0
    if [ "$type" = 1 ] || [ "$type" = 9 ] || [ "$type" = 36 ]; then
        nofreeze=0; [ "$banks" = 0 ] && nofreeze=1
        for b in 0 1 2 3; do
            python3 -I "$ROOT/tools/k64asm.py" "$ROOT/tests/carts/freezer.s" -o "$OUT/$tag-$b.prg" -D "TYPE=$type" -D "BANK=$b" \
                -D "NOFREEZE=$nofreeze" --labels "$OUT/$tag.labels" || ok=0
            chips+=("$OUT/$tag-$b.prg@$b:8000")
        done
        for label in press1 press2 press3; do
            at="$(awk -v l="$label" '$1 == l { print $2 }' "$OUT/$tag.labels" 2>/dev/null)"
            [ -n "$at" ] && extra+=(--freeze-pc "$at")
        done
        extra+=(--freeze-hold 2000)
        [ "${#extra[@]}" -ge 4 ] || ok=0   # no label: nothing would press the button
        cycles=6000000
        noref=1
        fbanks=$banks; banks=0
    fi
    if [ "$type" = 32 ]; then
        # The code in the ROMH chip's banks 0 and 8, a marked bank 0 and 8 in the ROML chip.
        fasm() { python3 -I "$ROOT/tools/k64asm.py" "$ROOT/tests/carts/flash.s" -o "$1" -D "PART=$2" -D "BANK=$3" -D "STRICT=$banks"; }
        fasm "$OUT/$tag-code.prg" 0 0 && fasm "$OUT/$tag-l0.prg" 1 0 && fasm "$OUT/$tag-l8.prg" 1 8 || ok=0
        chips=("$OUT/$tag-l0.prg@0:8000" "$OUT/$tag-code.prg@0:a000" "$OUT/$tag-l8.prg@8:8000" "$OUT/$tag-code.prg@8:a000")
        cycles=16000000
        exrom=1; game=0
        strict=$banks; banks=0
    fi
    for ((b = 0; b < banks; b++)); do
        case "$type" in
            4|18) [ "$b" = 0 ] || break ;;   # one chip at $8000
        esac
        asm "$OUT/$tag-$b.prg" "$type" "$b" "$banks" 0 || ok=0
        case "$type" in
            3|21|53|70)   # banks of 16K: one chip of both halves
                asm "$OUT/$tag-h$b.prg" "$type" "$b" "$banks" 1 || ok=0
                { tail -c +3 "$OUT/$tag-$b.prg"; tail -c +3 "$OUT/$tag-h$b.prg"; } >"$OUT/$tag-$b.bin"
                chips+=("$OUT/$tag-$b.bin@$b:8000")
                continue ;;
        esac
        n=$b; load=8000
        [ "$type" = 7 ] && n=$(( ((b & 7) << 3) | (b >> 3) ))   # the file names the register value
        [ "$type" = 5 ] && [ "$banks" = 32 ] && [ "$b" -ge 16 ] && load=a000
        chips+=("$OUT/$tag-$b.prg@$n:$load")
    done
    case "$type" in
        4) asm "$OUT/$tag-h0.prg" "$type" 0 1 1 || ok=0; chips+=("$OUT/$tag-h0.prg@0:a000") ;;
        18) for b in 0 1; do asm "$OUT/$tag-h$b.prg" "$type" "$b" 2 1 || ok=0; chips+=("$OUT/$tag-h$b.prg@$b:a000"); done ;;
    esac
    # A program with a label press1 waits there for its cartridge's button.
    at="$(awk '$1 == "press1" { print $2 }' "$OUT/$tag-0.prg.labels" 2>/dev/null)"
    [ -n "$at" ] && extra+=(--freeze-pc "$at" --freeze-hold 2000)
    if [ "$ok" = 0 ]; then echo "FAIL type $type, $banks bank(s): does not assemble"; return; fi
    python3 -I "$ROOT/tools/mkcrt.py" -o "$OUT/$tag.crt" --type "${htype:-$type}" --exrom "${exrom:-0}" --game "${game:-1}" "${chips[@]}"
    "$K64RUN" "$ROOT/gitignore/bios" --cart "$OUT/$tag.crt" --debugcart --cycles "$cycles" "${extra[@]}" >"$OUT/$tag.log" 2>&1
    got=$?
    line="type $type, $banks bank(s)${htype:+ in type $htype hardware}: ours $got, expected $want"
    [ "$type" = 32 ] && line="type 32, flash chips$([ "$strict" = 1 ] && echo ", data sheet checks too"): ours $got, expected $want"
    [ "$noref" = 1 ] && line="type $type, freeze button$([ "$fbanks" = 0 ] && echo ", function switched off")${htype:+ in type $htype hardware}: ours $got, expected $want"
    if [ "${K64_CARTS_REF:-0}" = 1 ] && [ "$noref" = 0 ]; then
        timeout 300 "$ROOT/tools/oracle/vice.sh" -debugcart -jamaction 1 -warp -limitcycles "$cycles" \
            -cartcrt "/k64/${OUT#"$ROOT"/}/$tag.crt" >"$OUT/$tag.ref.log" 2>&1
        # Its exit status 1 is also its cycle limit: the reported value is in its log.
        ref="$(tr -d '\r' <"$OUT/$tag.ref.log" | sed -n 's/.*DBGCART: exit(\([0-9]*\)).*/\1/p' | tail -1)"
        [ -n "$ref" ] || ref=124
        grep -q 'could not attach' "$OUT/$tag.ref.log" && ref=refuses
        line="$line, reference $ref"
        [ "$ref" = "${refwant:-$want}" ] || { echo "FAIL $line (the reference does not give the expected value)"; return; }
    fi
    if [ "$got" = "$want" ]; then echo "ok   $line"; else echo "FAIL $line"; fi
}
while read -r type banks want htype exrom game refwant; do
    case "$type" in ''|'#'*) continue ;; esac
    sbs_start one_line "$type" "$banks" "$want" "$htype" "$exrom" "$game" "$refwant"
done <"$ROOT/tests/carts/expected.txt"
sbs_finish

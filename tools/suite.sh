#!/usr/bin/env bash
# Runs test programs of the VICE test-program repository (gitignore/external/testprogs, see
# tools/oracle/build-vice.sh) on our machine and on the reference emulator with the same
# machine options, and prints a scoreboard.
#
#   tools/suite.sh PATTERN [K64RUN]       PATTERN: extended regex on the test-list line
#
# "exitcode" tests report through the debug cartridge at $D7FF. "screenshot" tests are
# judged by the screen: our whole raster after the test's cycle budget must equal the
# reference's whole-raster screenshot at the same cycle (tools/oracle/framecmp.py);
# reference screenshots are cached in gitignore/external/suite-ref-shots/. The suite's
# "interactive" programs that show a still picture when left alone are judged the same way
# (tests/suite-interactive.txt).
# A test with a cartridge file (the list's mountcrt: option) runs with it plugged in, on
# both; lines the suite's own list lacks are added from tests/suite-extra.txt.
# Tests that need hardware we do not have yet (a cartridge type not built, disk
# images, expansions) are listed as skipped with the reason. Results:
#   same-pass  both pass            same-fail  both fail (the reference fails too)
#   WORSE      reference passes, we do not       better  we pass, the reference does not
#   known      WORSE, and listed with its reason in tests/suite-known.txt (hardware of a
#              later phase); FIXED = listed there but no longer worse: remove the line
# The SID directory's exit-code programs were checked on real chips: failing one is WORSE
# even where the reference emulator fails it too.
# Exit status 0 when no test is WORSE or FIXED. A quarter of the machine's cores run tests at a time
# (K64_SUITE_JOBS overrides), at low priority. The seconds each of our runs took are in
# scratch/suite/times.txt, slowest first.
# The reference's result for a test is cached in gitignore/external/suite-ref.cache (it only
# changes with the pinned reference build); K64_SUITE_REFRESH=1 runs the reference again.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PATTERN="${1:?pattern}"
K64RUN="$(realpath "${2:-$ROOT/bin/k64run}")"
TP="$ROOT/gitignore/external/testprogs"
LIST="$TP/testbench/c64-testlist.in"
[ -f "$LIST" ] || { echo "suite: no test list; run tools/oracle/build-vice.sh" >&2; exit 2; }
OUT="$ROOT/scratch/suite"
mkdir -p "$OUT"

run_one() {
    local line="$1" root="$2" k64run="$3" out="$4"
    IFS=',' read -r dir prg kind cycles opts <<<"$line"
    dir="${dir%/}/"   # some list entries omit the directory's trailing slash
    local name="${dir#../}$prg${opts:+ [$opts]}"
    local model=pal vmodel=c64 cia=old ciaref=0 ckey="" skip="" o fam=pal newvic=0 vkey sid=old sidref=resid6581 crt=""
    local -a vextra=()
    IFS=',' read -ra optv <<<"$opts"
    for o in "${optv[@]}"; do
        case "$o" in
            vicii-pal|cia-old|vicii-old|"") ;;
            vicii-ntsc) model=ntsc; vmodel=ntsc; fam=ntsc ;;
            vicii-ntscold) model=ntsc-old; vmodel=oldntsc; fam=ntsc-old ;;
            cia-new) cia=new; ciaref=1; ckey="+cianew" ;;
            sid-old) ;;
            sid-new) sid=new; sidref=resid8580; ckey="$ckey+sidnew" ;;
            vicii-new) newvic=1 ;;
            mountcrt:*) crt="${o#mountcrt:}" ;;
            comment:*|expect:*|warn:*) ;;   # notes for people, not requirements
            *) skip="needs $o" ;;
        esac
    done
    vkey="$vmodel"
    if [ "$newvic" = 1 ]; then   # the later chip generation on the same TV system
        case "$fam" in
            pal) model=pal-new; vextra=(-VICIImodel 8565); vkey="c64+8565" ;;
            ntsc) model=ntsc-new; vextra=(-VICIImodel 8562); vkey="ntsc+8562" ;;
            *) skip="no later chip for $fam" ;;
        esac
    fi
    # An "interactive" program listed in tests/suite-interactive.txt shows a still picture
    # when left alone: it is judged like a screenshot test.
    if [ "$kind" = interactive ] && grep -qxF -- "${dir#../}$prg" "$root/tests/suite-interactive.txt" 2>/dev/null; then
        kind=screenshot; cycles=10000000
    fi
    [ "$kind" = exitcode ] || [ "$kind" = screenshot ] || skip="${kind} test"
    if [ -n "$skip" ]; then printf 'skip      %s (%s)\n' "$name" "$skip"; return; fi
    local base="$root/gitignore/external/testprogs/testbench/$dir"
    local file="$base$prg"
    local tag; tag="$(echo "$name" | tr -c 'A-Za-z0-9.-' '_')"
    # What goes into the machine: a program to start, a cartridge, or both. The cache key
    # is the program's hash (as before cartridges) or, without a program, the cartridge's.
    local -a media=() refmedia=()
    if [ -n "$prg" ]; then
        media+=(--autostart "$file")
        refmedia+=(-autostartprgmode 1 -autostart "/k64/${file#"$root"/}")
    fi
    if [ -n "$crt" ]; then
        media+=(--cart "$base$crt")
        refmedia+=(-cartcrt "/k64/${base#"$root"/}$crt")
        if [ -n "$prg" ]; then ckey="$ckey+crt$(sha1sum "$base$crt" | cut -c1-8)"; else file="$base$crt"; ckey="$ckey+crt"; fi
        # A cartridge whose hardware is not built yet is refused by our machine at once.
        if "$k64run" "$root/gitignore" --cart "$base$crt" --cycles 1 2>&1 | grep -q 'is not supported yet'; then
            printf 'skip      %s (%s)\n' "$name" "cartridge hardware not built yet"; return
        fi
    fi
    local t0=${EPOCHREALTIME/[.,]/}
    trap 'printf "%d.%d %s\n" $(( (${EPOCHREALTIME/[.,]/} - t0) / 1000000 )) $(( (${EPOCHREALTIME/[.,]/} - t0) / 100000 % 10 )) "$name" >>"$out/times.raw"' RETURN
    if [ "$kind" = screenshot ]; then
        local dx dy shots="$root/gitignore/external/suite-ref-shots" key2
        case "$fam" in pal) dx=492; dy=0 ;; ntsc) dx=508; dy=20 ;; *) dx=500; dy=20 ;; esac
        mkdir -p "$shots"
        key2="$(sha1sum "$file" | cut -c1-16)-$vkey$ckey-$cycles"
        nice -n 10 "$k64run" "$root/gitignore" --model "$model" --cia "$cia" --sid "$sid" "${media[@]}" --cycles "$cycles" \
            --raster "$out/$tag.raw" >"$out/$tag.k64.log" 2>&1
        if [ ! -s "$shots/$key2.png" ] || [ "${K64_SUITE_REFRESH:-0}" = 1 ]; then
            K64_VICE_MODEL="$vmodel" timeout 600 "$root/tools/oracle/vice.sh" "${vextra[@]}" -sidenginemodel $sidref -ciamodel $ciaref -jamaction 1 -warp -VICIIborders 2 \
                -limitcycles "$cycles" "${refmedia[@]}" \
                -exitscreenshot "/k64/${shots#"$root"/}/$key2.png" >"$out/$tag.ref.log" 2>&1
        fi
        if [ ! -s "$shots/$key2.png" ]; then printf '%-9s %s (no reference screenshot)\n' WORSE "$name"; return; fi
        if python3 -I "$root/tools/oracle/framecmp.py" "$out/$tag.raw" "$shots/$key2.png" --dx $dx --dy $dy \
               --colors "vice-colors-$fam.txt" --diff "$out/$tag.diff.png" >"$out/$tag.cmp.log" 2>&1; then
            printf '%-9s %s (screen identical)\n' same-pass "$name"
        else
            printf '%-9s %s (screen: %s)\n' WORSE "$name" "$(grep -o '[0-9]* pixels differ' "$out/$tag.cmp.log" | tail -1)"
        fi
        return
    fi
    nice -n 10 "$k64run" "$root/gitignore" --model "$model" --cia "$cia" --sid "$sid" "${media[@]}" --debugcart --cycles "$cycles" \
        >"$out/$tag.k64.log" 2>&1
    local ours=$?
    local cache="$root/gitignore/external/suite-ref.cache" key ref
    key="$(sha1sum "$file" | cut -c1-16) $vkey$ckey $cycles"
    ref="$(grep -F -- "$key " "$cache" 2>/dev/null | tail -1 | awk '{print $NF}')"
    if [ -z "$ref" ] || [ "${K64_SUITE_REFRESH:-0}" = 1 ]; then
        K64_VICE_MODEL="$vmodel" timeout 600 "$root/tools/oracle/vice.sh" "${vextra[@]}" -sidenginemodel $sidref -ciamodel $ciaref -debugcart -jamaction 1 -warp \
            -limitcycles "$cycles" "${refmedia[@]}" >"$out/$tag.ref.log" 2>&1
        ref=$?
        flock "$cache.lock" sh -c "echo '$key $ref' >> '$cache'"
    fi
    local r
    # The SID directory's programs carry their own verdict, checked on real chips: failing
    # one is WORSE even where the reference emulator fails it too.
    if [ "${dir#../SID/}" != "$dir" ] && [ $ours != 0 ]; then r=WORSE
    elif [ $ours = 0 ] && [ $ref = 0 ]; then r=same-pass
    elif [ $ours != 0 ] && [ $ref != 0 ]; then r=same-fail
    elif [ $ref = 0 ]; then r=WORSE
    else r=better; fi
    printf '%-9s %s (ours %s, reference %s)\n' "$r" "$name" "$ours" "$ref"
}
export -f run_one

JOBS="${K64_SUITE_JOBS:-$(( $(nproc --all) / 4 > 0 ? $(nproc --all) / 4 : 1 ))}"
: >"$OUT/times.raw"
{ cat "$LIST"; cat "$ROOT/tests/suite-extra.txt" 2>/dev/null; } | grep -v '^#' | grep -E -- "$PATTERN" | grep -v '^\s*$' |
    xargs -d '\n' -P "$JOBS" -I{} bash -c 'run_one "$1" "$2" "$3" "$4"' _ {} "$ROOT" "$K64RUN" "$OUT" |
    sort -k2 >"$OUT/scoreboard.raw"
sort -rn "$OUT/times.raw" >"$OUT/times.txt"; rm -f "$OUT/times.raw"
# Known carry-overs: a line of tests/suite-known.txt is "NAME-PREFIX<TAB>REASON"; it covers
# every test whose name (directory/program, as printed) starts with the prefix.
python3 -I - "$OUT/scoreboard.raw" "$ROOT/tests/suite-known.txt" >"$OUT/scoreboard.txt" <<'PY'
import sys
known = []
try:
    for line in open(sys.argv[2], encoding="utf-8"):
        line = line.rstrip("\n")
        if line and not line.startswith("#"):
            prefix, _, reason = line.partition("\t")
            known.append((prefix, reason.strip()))
except FileNotFoundError:
    pass
for line in open(sys.argv[1], encoding="utf-8"):
    line = line.rstrip("\n")
    status, _, rest = line.partition(" ")
    name = rest.strip()
    hit = next((k for k in known if name.startswith(k[0])), None)
    if hit and status == "WORSE":
        line = "known     " + name + " {" + hit[1] + "}"
    elif hit and status in ("same-pass", "better"):
        line = "FIXED     " + name + " {listed in tests/suite-known.txt: remove it}"
    print(line)
PY
cat "$OUT/scoreboard.txt"
count() { grep -c "^$1" "$OUT/scoreboard.txt"; }
echo "suite: $(count same-pass) same-pass, $(count same-fail) same-fail, $(count WORSE) WORSE, $(count known) known, $(count FIXED) FIXED, $(count better) better, $(count skip) skipped"
! grep -qE '^(WORSE|FIXED)' "$OUT/scoreboard.txt"

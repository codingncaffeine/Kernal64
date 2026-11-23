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
# reference screenshots are cached in gitignore/external/suite-ref-shots/.
# Tests that need hardware we do not have yet (new VIC-II chips, cartridges, disk
# images, expansions) are listed as skipped with the reason. Results:
#   same-pass  both pass            same-fail  both fail (the reference fails too)
#   WORSE      reference passes, we do not       better  we pass, the reference does not
#   known      WORSE, and listed with its reason in tests/suite-known.txt (hardware of a
#              later phase); FIXED = listed there but no longer worse: remove the line
# Exit status 0 when no test is WORSE or FIXED. Four tests run at a time, at low priority.
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
    local model=pal vmodel=c64 cia=old ciaref=0 ckey="" skip="" o
    IFS=',' read -ra optv <<<"$opts"
    for o in "${optv[@]}"; do
        case "$o" in
            vicii-pal|cia-old|vicii-old|"") ;;
            vicii-ntsc) model=ntsc; vmodel=ntsc ;;
            vicii-ntscold) model=ntsc-old; vmodel=oldntsc ;;
            cia-new) cia=new; ciaref=1; ckey="+cianew" ;;
            vicii-new) skip="new VIC-II not emulated yet" ;;
            comment:*|expect:*|warn:*) ;;   # notes for people, not requirements
            *) skip="needs $o" ;;
        esac
    done
    [ "$kind" = exitcode ] || [ "$kind" = screenshot ] || skip="${kind} test"
    if [ -n "$skip" ]; then printf 'skip      %s (%s)\n' "$name" "$skip"; return; fi
    local file="$root/gitignore/external/testprogs/testbench/$dir$prg"
    local tag; tag="$(echo "$name" | tr -c 'A-Za-z0-9.-' '_')"
    if [ "$kind" = screenshot ]; then
        local dx dy shots="$root/gitignore/external/suite-ref-shots" key2
        case "$model" in pal) dx=492; dy=0 ;; ntsc) dx=508; dy=20 ;; *) dx=500; dy=20 ;; esac
        mkdir -p "$shots"
        key2="$(sha1sum "$file" | cut -c1-16)-$vmodel$ckey-$cycles"
        nice -n 10 "$k64run" "$root/gitignore" --model "$model" --cia "$cia" --autostart "$file" --cycles "$cycles" \
            --raster "$out/$tag.raw" >"$out/$tag.k64.log" 2>&1
        if [ ! -s "$shots/$key2.png" ] || [ "${K64_SUITE_REFRESH:-0}" = 1 ]; then
            K64_VICE_MODEL="$vmodel" timeout 600 "$root/tools/oracle/vice.sh" -ciamodel $ciaref -jamaction 1 -warp -VICIIborders 2 \
                -limitcycles "$cycles" -autostartprgmode 1 -autostart "/k64/${file#"$root"/}" \
                -exitscreenshot "/k64/${shots#"$root"/}/$key2.png" >"$out/$tag.ref.log" 2>&1
        fi
        if [ ! -s "$shots/$key2.png" ]; then printf '%-9s %s (no reference screenshot)\n' WORSE "$name"; return; fi
        if python3 -I "$root/tools/oracle/framecmp.py" "$out/$tag.raw" "$shots/$key2.png" --dx $dx --dy $dy \
               --colors "vice-colors-$model.txt" --diff "$out/$tag.diff.png" >"$out/$tag.cmp.log" 2>&1; then
            printf '%-9s %s (screen identical)\n' same-pass "$name"
        else
            printf '%-9s %s (screen: %s)\n' WORSE "$name" "$(grep -o '[0-9]* pixels differ' "$out/$tag.cmp.log" | tail -1)"
        fi
        return
    fi
    nice -n 10 "$k64run" "$root/gitignore" --model "$model" --cia "$cia" --autostart "$file" --debugcart --cycles "$cycles" \
        >"$out/$tag.k64.log" 2>&1
    local ours=$?
    local rel="${file#"$root"/}"
    local cache="$root/gitignore/external/suite-ref.cache" key ref
    key="$(sha1sum "$file" | cut -c1-16) $vmodel$ckey $cycles"
    ref="$(grep -F -- "$key " "$cache" 2>/dev/null | tail -1 | awk '{print $NF}')"
    if [ -z "$ref" ] || [ "${K64_SUITE_REFRESH:-0}" = 1 ]; then
        K64_VICE_MODEL="$vmodel" timeout 600 "$root/tools/oracle/vice.sh" -ciamodel $ciaref -debugcart -jamaction 1 -warp \
            -limitcycles "$cycles" -autostartprgmode 1 -autostart "/k64/$rel" >"$out/$tag.ref.log" 2>&1
        ref=$?
        flock "$cache.lock" sh -c "echo '$key $ref' >> '$cache'"
    fi
    local r
    if [ $ours = 0 ] && [ $ref = 0 ]; then r=same-pass
    elif [ $ours != 0 ] && [ $ref != 0 ]; then r=same-fail
    elif [ $ref = 0 ]; then r=WORSE
    else r=better; fi
    printf '%-9s %s (ours %s, reference %s)\n' "$r" "$name" "$ours" "$ref"
}
export -f run_one

grep -v '^#' "$LIST" | grep -E -- "$PATTERN" | grep -v '^\s*$' |
    xargs -d '\n' -P 4 -I{} bash -c 'run_one "$1" "$2" "$3" "$4"' _ {} "$ROOT" "$K64RUN" "$OUT" |
    sort -k2 >"$OUT/scoreboard.raw"
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

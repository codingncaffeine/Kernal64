#!/usr/bin/env bash
# Runs test programs of the VICE test-program repository (gitignore/external/testprogs, see
# tools/oracle/build-vice.sh) on our machine and on the reference emulator with the same
# machine options, and prints a scoreboard.
#
#   tools/suite.sh PATTERN [K64RUN]       PATTERN: extended regex on the test-list line
#
# Only "exitcode" tests run (the program reports through the debug cartridge at $D7FF).
# Tests that need hardware we do not have yet (new CIA or VIC-II chips, cartridges, disk
# images, expansions) are listed as skipped with the reason. Results:
#   same-pass  both pass            same-fail  both fail (the reference fails too)
#   WORSE      reference passes, we do not       better  we pass, the reference does not
# Exit status 0 when no test is WORSE. Four tests run at a time, at low priority.
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
    local name="${dir#../}$prg${opts:+ [$opts]}"
    local model=pal vmodel=c64 skip="" o
    IFS=',' read -ra optv <<<"$opts"
    for o in "${optv[@]}"; do
        case "$o" in
            vicii-pal|cia-old|vicii-old|"") ;;
            vicii-ntsc) model=ntsc; vmodel=ntsc ;;
            vicii-ntscold) model=ntsc-old; vmodel=oldntsc ;;
            cia-new) skip="new CIA not emulated yet" ;;
            vicii-new) skip="new VIC-II not emulated yet" ;;
            comment:*|expect:*) ;;
            *) skip="needs $o" ;;
        esac
    done
    [ "$kind" = exitcode ] || skip="${kind} test"
    if [ -n "$skip" ]; then printf 'skip      %s (%s)\n' "$name" "$skip"; return; fi
    local file="$root/gitignore/external/testprogs/testbench/$dir$prg"
    local tag; tag="$(echo "$name" | tr -c 'A-Za-z0-9.-' '_')"
    nice -n 10 "$k64run" "$root/gitignore" --model "$model" --autostart "$file" --debugcart --cycles "$cycles" \
        >"$out/$tag.k64.log" 2>&1
    local ours=$?
    local rel="${file#"$root"/}"
    local cache="$root/gitignore/external/suite-ref.cache" key ref
    key="$(sha1sum "$file" | cut -c1-16) $vmodel $cycles"
    ref="$(grep -F -- "$key " "$cache" 2>/dev/null | tail -1 | awk '{print $NF}')"
    if [ -z "$ref" ] || [ "${K64_SUITE_REFRESH:-0}" = 1 ]; then
        K64_VICE_MODEL="$vmodel" timeout 600 "$root/tools/oracle/vice.sh" -debugcart -jamaction 1 -warp \
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
    sort -k2 | tee "$OUT/scoreboard.txt"
echo "suite: $(grep -c '^same-pass' "$OUT/scoreboard.txt") same-pass, $(grep -c '^same-fail' "$OUT/scoreboard.txt") same-fail, $(grep -c '^WORSE' "$OUT/scoreboard.txt") WORSE, $(grep -c '^better' "$OUT/scoreboard.txt") better, $(grep -c '^skip' "$OUT/scoreboard.txt") skipped"
! grep -q '^WORSE' "$OUT/scoreboard.txt"

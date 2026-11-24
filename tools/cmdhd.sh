#!/usr/bin/env bash
# The CMD HD (PLAN Phase 11b): its board runs the user's boot ROM, finds no system on a blank
# disk and waits on the bus in install mode; a C64 then reads its status as device 30.
#   tools/cmdhd.sh BUILD_DIR
# Needs gitignore/bios/cmd_hd_bootrom_v280.bin (the user's file, never committed).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:?build dir}"
ROM="$ROOT/gitignore/bios/cmd_hd_bootrom_v280.bin"
[ -f "$ROM" ] || { echo "cmdhd: boot ROM missing: $ROM"; exit 1; }
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
truncate -s 2M "$TMP/blank.dhd"     # 4096 blocks: the search ends after 32 reads
fail=0
# 1. The board alone: power-on test, search, then the idle loop ($CED7-$CEED).
if out=$("$BUILD/hdtest" "$ROM" --disk "$TMP/blank.dhd" --seconds 15 --expect-visit CED7); then
    echo "ok   board: self test, search of a blank disk, idle in install mode"
else echo "FAIL board: $(echo "$out" | tail -2 | tr '\n' ' ')"; fail=1; fi
# 2. A C64 reads its status channel as device 30 (install mode's number).
C=985248; T=$((C * 10))
out=$("$BUILD/k64run" "$ROOT/gitignore/bios" --hd "$ROM" --hd-disk "$TMP/blank.dhd" --cycles $((T + C * 6)) \
      --type-at $T '10OPEN15,3' --type-at $((T + C)) '0,15:INPUT' --type-at $((T + 2 * C)) '#15,A,B$:?' \
      --type-at $((T + 3 * C)) 'A;B$\r' --type-at $((T + 4 * C)) 'RUN\r' --expect '78 SYSTEM ERROR' 2>&1)
if [ $? -eq 0 ]; then echo "ok   device 30 answers its status: 78 SYSTEM ERROR (no system on the disk)"
else echo "FAIL device 30's status: $(echo "$out" | grep -v '^$' | tail -3 | tr '\n' ' ')"; fail=1; fi
exit $fail

#!/usr/bin/env bash
# The CMD HD (PLAN Phase 11b): its board runs the user's boot ROM, finds no system on a blank
# disk and waits on the bus in install mode; a C64 then reads its status as device 30.
#   tools/cmdhd.sh BUILD_DIR
# Needs the user's files (never committed): gitignore/bios/cmd_hd_bootrom_v280.bin and the drive's
# utilities disk (a D81) in gitignore/bios or gitignore/.
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
# 3. A drive made by the core's own make function (src/drive/hdprep.h: the drive's install program
#    from the user's utilities disk, then partitions made and formatted): the DOS boots from the disk
#    as device 12; a program is saved, loaded, listed and run; the directory shows partition 1.
#    Needs the utilities disk (a D64 or D81 image) in gitignore/bios or gitignore/.
if ! "$BUILD/k64run" "$ROOT/gitignore/bios" --make-hd "$TMP/made.dhd" 20 > "$TMP/make.log" 2>&1; then
    echo "FAIL make a drive: $(tail -1 "$TMP/make.log")"; fail=1
elif ! grep -q "make-hd ok: .*, 2 partition(s)" "$TMP/make.log"; then
    echo "FAIL make a drive: $(tail -1 "$TMP/make.log")"; fail=1
else
    echo "ok   make a drive: 20 MB, the DOS installed by the drive's own program, 2 partitions formatted"
    T=$((C * 8))
    out=$("$BUILD/k64run" "$ROOT/gitignore/bios" --hd "$ROM" --hd-disk "$TMP/made.dhd" --cycles $((T + C * 40)) \
          --type-at $T '10?"HELLO ' --type-at $((T + C)) 'FROM HD"\r' --type-at $((T + 2 * C)) 'SAVE"TEST"' \
          --type-at $((T + 3 * C)) ',12\r' --type-at $((T + 10 * C)) 'NEW\r' --type-at $((T + 11 * C)) 'LOAD"TEST"' \
          --type-at $((T + 12 * C)) ',12\r' --type-at $((T + 19 * C)) 'LIST\r' --type-at $((T + 20 * C)) 'RUN\r' \
          --type-at $((T + 22 * C)) 'LOAD"$",12' --type-at $((T + 23 * C)) '\r' --type-at $((T + 30 * C)) 'LIST\r' \
          --expect '10 PRINT"HELLO FROM HD"' --expect 'HELLO FROM HD' --expect '"PARTITION 1     " HD 1H' \
          --expect '"TEST"             PRG' 2>&1)
    if [ $? -eq 0 ]; then echo "ok   made drive: device 12 saves, loads, lists and runs a program; directory shows it"
    else echo "FAIL made drive: $(echo "$out" | grep -v '^$' | tail -4 | tr '\n' ' ')"; fail=1; fi
    # 4. Through the libretro core: the content's drive ("<save>/Kernal64.dhd" with no content), a
    #    save to it, and the conformance checks (a state saved half-way replays exactly, also in a
    #    restarted core).
    mkdir -p "$TMP/save" && cp --sparse=always "$TMP/made.dhd" "$TMP/save/Kernal64.dhd"
    PAD=$(for i in $(seq 40); do printf '\n '; done)
    out=$("$BUILD/k64host" "$BUILD/kernal64_libretro.so" --system "$ROOT/gitignore/bios" --save "$TMP/save" --frames 3600 \
          --type-at 800 --type "10?\"HI\""$'\n'"SAVE\"T\",12${PAD}"$'\n'"LOAD\"\$\",12${PAD}"$'\n'"LIST"$'\n' \
          --expect-screen '"T"' --expect-screen 'HD 1H' --check 2>&1)
    if [ $? -eq 0 ]; then echo "ok   core: the content's hard drive saves through libretro; a state replays exactly"
    else echo "FAIL core: $(echo "$out" | grep -E 'FAIL' | head -2 | tr '\n' ' ')"; fail=1; fi
fi
# 4b. Option kernal64_hard_drive=off: the drive stays off the bus although the content has one.
if [ -f "$TMP/save/Kernal64.dhd" ]; then
    out=$("$BUILD/k64host" "$BUILD/kernal64_libretro.so" --system "$ROOT/gitignore/bios" --save "$TMP/save" --frames 1200 \
          --option kernal64_hard_drive=off --option kernal64_folder_drive=off --type-at 800 --type 'LOAD"$",12'$'\n' --expect-screen 'DEVICE NOT PRESENT' 2>&1)
    if [ $? -eq 0 ]; then echo "ok   core, option off: device 12 not present"
    else echo "FAIL core, option off: $(echo "$out" | grep FAIL | head -1)"; fail=1; fi
fi
# 5. The two BASIC programs in the core are the sources beside them.
"$ROOT/tools/hdprep-programs.sh" --check | sed 's/^/ok   /' || fail=1
exit $fail

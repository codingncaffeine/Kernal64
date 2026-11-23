#!/usr/bin/env bash
# The one gate. Exit code 0 only if every step passed.
#
#   tools/gate.sh [--quick]     --quick skips the RetroArch run
#
# Steps: nothing forbidden is tracked; a clean build with warnings as errors;
# the libretro conformance checks on every machine model; the PLA against a chip
# read-out and the KERNAL booting to BASIC on every model; the CPU against the
# per-opcode bus traces, the functional and undocumented-opcode test programs and
# the transistor-level chip; the SID's digital part against the reference engine cycle by
# cycle, its DACs code by code, its combined waveforms against chip samples and its filter,
# mixer and volume stage in bands, the chip-variant option on the programs of both kinds,
# its paddle converters cycle by cycle against their rule; the audio output path as numbers and tones through the
# API; the VIC-II against the article's rules and the start-up
# screen against the reference emulator (tools/oracle/build-vice.sh); the reference
# suite's CPU, CIA, interrupt, general and VIC-II programs on both CIA types and both VIC-II
# generations, its cartridge programs with the cartridge plugged in; our own probe,
# cartridge and raster-effect programs; the freeze of a freezer cartridge against dumps
# measured on real ones; cartridge and program files as content through the
# API; the 1541's 6522 against its data sheet's rules and its board booting the DOS ROM;
# the core loading in RetroArch behind the wall.
# The third-party suites come from tools/fetch.sh.
# Runs on a quarter of the cores at low priority (independent single-threaded runs go side
# by side in batches of that size); its build folder is removed at exit.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
QUICK=0; [ "${1:-}" = "--quick" ] && QUICK=1
JOBS="$(( $(nproc --all) / 4 > 0 ? $(nproc --all) / 4 : 1 ))"
BUILD="$(mktemp -d "$ROOT/build-gate.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
fails=0
step() { echo "== $* [${SECONDS}s]"; }
bad() { echo "GATE FAIL: $*"; fails=$((fails + 1)); }
# Independent single-threaded runs side by side, $JOBS at a time (a full batch runs to its
# end before the next run starts):
#   side LABEL LOG COMMAND...   starts one;   sides   waits for all and reports the failed.
side_pids=(); side_labels=(); side_logs=(); side_waited=0
side_reap() {
    local i
    for ((i = side_waited; i < ${#side_pids[@]}; i++)); do
        wait "${side_pids[$i]}" || {
            grep -A7 -E 'DIFFERS|FAIL' "${side_logs[$i]}" | head -8 | grep . || tail -5 "${side_logs[$i]}"
            bad "${side_labels[$i]}"
        }
    done
    side_waited=${#side_pids[@]}
}
side() {
    local label="$1" log="$2"; shift 2
    if [ $(( ${#side_pids[@]} - side_waited )) -ge "$JOBS" ]; then side_reap; fi
    "$@" >"$log" 2>&1 &
    side_pids+=($!); side_labels+=("$label"); side_logs+=("$log")
}
sides() { side_reap; side_pids=(); side_labels=(); side_logs=(); side_waited=0; }

step "tracked files"
# ROMs, media, documents and plans never enter the repository.
forbidden="$(git ls-files --cached --others --exclude-standard | grep -iE '\.(bin|rom|d64|g64|d71|d81|t64|tap|prg|p00|crt|nib|nbz|zip|pdf)$|^gitignore/|^local-notes/' || true)"
[ -z "$forbidden" ] || bad "would be committed: $(echo "$forbidden" | head -5 | tr '\n' ' ')"
idents="$(git log --format='%an <%ae>%n%cn <%ce>' | sort -u | grep -vE '^codingncaffeine <(19158626\+)?codingncaffeine@users\.noreply\.github\.com>$|^GitHub <noreply@github\.com>$' || true)"
[ -z "$idents" ] || bad "foreign identity in history: $idents"
[ "$(git config user.email)" = "codingncaffeine@users.noreply.github.com" ] || bad "repository identity is not the noreply address"

step "build (warnings are errors)"
if cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DK64_WERROR=ON >/dev/null &&
   nice -n 10 cmake --build "$BUILD" -j "$JOBS" >"$BUILD/build.log" 2>&1; then
    grep -qi "warning" "$BUILD/build.log" && bad "build printed warnings"
else
    tail -20 "$BUILD/build.log"; bad "build failed"
fi
CORE="$BUILD/kernal64_libretro.so"
[ -f "$CORE" ] || { echo "GATE: no core, stopping"; exit 1; }
extra="$(nm -D --defined-only "$CORE" | awk '{print $3}' | grep -v '^retro_' || true)"
[ -z "$extra" ] || bad "core exports non-libretro symbols: $(echo "$extra" | head -3 | tr '\n' ' ')"

# The machine needs the user's system ROMs (never in the repository): the gitignore folder.
ROMS="$ROOT/gitignore"
EXT="$ROOT/gitignore/external"

step "libretro conformance"
for model in pal ntsc ntsc-old pal-new ntsc-new pal-old; do
    # Typing at the READY prompt through the API: BASIC must answer. Nothing has touched the
    # SID: every audio sample is zero.
    side "conformance ($model)" "$BUILD/host-$model.log" "$BUILD/k64host" "$CORE" --system "$ROMS" --frames 400 --check \
        --option kernal64_model=$model --type 'PRINT 6*7\n' --expect-screen ' 42' --expect-silence
done
# Drive 8 over the serial bus: BASIC reads the 1541's error channel, which holds the DOS's
# power-on message; a state saved half-way through must replay (the drive is in it).
for model in pal ntsc; do
    side "1541 error channel over the serial bus ($model)" "$BUILD/host-drive-$model.log" "$BUILD/k64host" "$CORE" \
        --system "$ROMS" --frames 1030 --check --option kernal64_model=$model --type-at 200 \
        --type '10 OPEN1,8,15\n20 INPUT#1,A,B$,C,D\n30 ?A;B$;C;D\nRUN\n' --expect-screen '73 CBM DOS V2.6 1541'
done
# A disk as content: inserted, started with LOAD"*",8,1 and RUN; the suite's program reads
# the drive's ROM over the bus with the DOS's memory-read command.
side "disk as content, started by itself" "$BUILD/host-disk.log" "$BUILD/k64host" "$CORE" --system "$ROMS" \
    --content "$EXT/testprogs/vdrive/test.d64" --frames 900 --check --expect-screen 'ROM:  254' --expect-screen 'ROM:  103'
sides
grep -q "missing system files" "$BUILD"/host-*.log && bad "system ROMs not found in $ROMS"
grep -q "no 1541 ROMs" "$BUILD"/host-*.log && bad "1541 ROMs not found in $ROMS"
# The border option: the picture handed to the frontend has the size asked for.
for want in "none 320x200" "small 384x270"; do
    "$BUILD/k64host" "$CORE" --system "$ROMS" --frames 60 --check --option kernal64_border="${want% *}" >"$BUILD/host-border.log" 2>&1
    grep -q "last frame ${want#* } " "$BUILD/host-border.log" || bad "border option ${want% *}: not ${want#* }"
done

step "audio"
# The rate conversion and the board's output network, as numbers.
side "audio output path" "$BUILD/audiotest.log" "$BUILD/audiotest"
# Through the API: a triangle on voice 1 (frequency register 7493, full volume and sustain)
# comes out at register * clock / 2^24 Hz, one voice's level, on both chips and clocks.
for run in "pal 440.02 6581" "ntsc 456.76 6581" "ntsc-old 456.76 8580" "pal-new 440.02 8580"; do
    # shellcheck disable=SC2086
    set -- $run
    side "tone ($1, $3)" "$BUILD/tone-$1.log" "$BUILD/k64host" "$CORE" --system "$ROMS" --frames 1000 \
        --option kernal64_model=$1 --option kernal64_sid=$3 \
        --type 'POKE54296,15:POKE54277,0:POKE54278,240:POKE54272,69:POKE54273,29:POKE54276,17\n' --expect-tone $2 -18.5 -15.5
done
# The same tone with the conformance checks: the state saved half-way holds a filter and an
# output network in motion, and the replay from it must end in the same state byte for byte.
side "conformance with a tone playing" "$BUILD/tone-check.log" "$BUILD/k64host" "$CORE" --system "$ROMS" --frames 1600 --check \
    --type 'POKE54296,15:POKE54277,0:POKE54278,240:POKE54272,69:POKE54273,29:POKE54276,17\n' --expect-tone 440.02 -18.5 -15.5
# Through the filter (8580, cutoff at its top, about 20 kHz): with the voice routed into it
# the low-pass lets the 440 Hz tone through at its level, the high-pass leaves about 1/400
# of it. (Two typed lines: BASIC takes 80 characters per line.)
for run in "31 -18.5 -15.5 low-pass" "79 -80 -60 high-pass"; do
    # shellcheck disable=SC2086
    set -- $run
    side "tone through the $4" "$BUILD/tone-$4.log" "$BUILD/k64host" "$CORE" --system "$ROMS" --frames 1300 --option kernal64_sid=8580 \
        --type "POKE54295,1:POKE54294,255:POKE54296,$1\nPOKE54277,0:POKE54278,240:POKE54272,69:POKE54273,29:POKE54276,17\n" --expect-level $2 $3
done

# The 6581 noise + pulse option through the API: voice 3 runs noise + pulse for a moment,
# then noise alone. On the default chip the spell has emptied the noise register and the
# voice stays silent; on the other kind the noise is there at its level.
for run in "drains-it -200 -90" "leaves-it-alone -22.5 -18.5"; do
    # shellcheck disable=SC2086
    set -- $run
    side "noise + pulse option (${1//-/ })" "$BUILD/tone-noise-$1.log" "$BUILD/k64host" "$CORE" --system "$ROMS" --frames 1300 --check \
        --option "kernal64_sid_noise_pulse=${1//-/ }" \
        --type 'POKE54296,15:POKE54292,240:POKE54287,255:POKE54290,193\nPOKE54290,129\n' --expect-level $2 $3
done
sides

step "memory map and boot"
if [ -f "$ROOT/gitignore/external/pla/PLA.BIN" ]; then
    "$BUILD/placheck" "$ROOT/gitignore/external/pla/PLA.BIN" >"$BUILD/pla.log" 2>&1 \
        || { tail -4 "$BUILD/pla.log"; bad "PLA against the chip read-out"; }
else bad "PLA read-out missing: tools/fetch.sh pla"; fi
for model in pal ntsc ntsc-old pal-new ntsc-new pal-old; do
    side "boot to the BASIC prompt ($model)" "$BUILD/boot-$model.log" \
        "$BUILD/k64run" "$ROMS" --frames 150 --model $model --expect "READY." --expect "38911 BASIC BYTES FREE"
done
sides
for model in pal ntsc ntsc-old pal-new ntsc-new pal-old; do
    want=$([ "${model#pal}" != "$model" ] && echo PAL || echo NTSC)
    grep -q "KERNAL detected $want" "$BUILD/boot-$model.log" || bad "KERNAL did not detect $want ($model)"
done

step "start-up screen against the reference emulator"
if [ -x "$ROOT/gitignore/external/vice/build/src/x64sc" ] && command -v podman >/dev/null; then
    tools/oracle/refcheck.sh "$BUILD/k64run" "$BUILD/refcheck" >"$BUILD/refcheck.log" 2>&1 \
        || { tail -6 "$BUILD/refcheck.log"; bad "start-up screen differs from the reference"; }
else bad "reference emulator missing: tools/oracle/build-vice.sh"; fi

step "CPU, CIA, interrupt, general, VIC-II, SID and cartridge test programs against the reference emulator"
# (Lorenz's suite is in "general". Programs waiting for later hardware: tests/suite-known.txt.)
if [ -f "$ROOT/gitignore/external/testprogs/testbench/c64-testlist.in" ]; then
    tools/suite.sh '^(\.\./(CIA|interrupts|general|CPU|VICII|SID|C64)/|\./selftest/)' "$BUILD/k64run" >"$BUILD/suite.log" 2>&1 \
        || { grep -E '^(WORSE|FIXED)' "$BUILD/suite.log" | head -8; bad "test programs worse than the reference"; }
    tail -1 "$BUILD/suite.log"
else bad "test programs missing: tools/oracle/build-vice.sh"; fi

step "own probe, cartridge and raster-effect programs, content files, VIC-II and SID rule tests, CPU tests"
# These scripts run their own lines side by side, so one after the other here.
script() {
    local label="$1" log="$2"; shift 2
    "$@" >"$log" 2>&1 || { grep -A7 FAIL "$log" | head -8 | grep . || tail -5 "$log"; bad "$label"; }
}
script "probe programs" "$BUILD/probes.log" tools/probes.sh "$BUILD/k64run"
# Each cartridge type's switching and the freeze, checked from inside our own test
# cartridges; the freeze of type 36 against dumps from real cartridges; then every kind of
# content file through the machine and through the built core.
script "own cartridge programs" "$BUILD/carts.log" tools/carts.sh "$BUILD/k64run"
script "freeze against real cartridges" "$BUILD/freezer.log" tools/freezer.sh "$BUILD/k64run"
script "content files" "$BUILD/media.log" tools/media.sh "$BUILD"
script "raster-effect programs" "$BUILD/effects.log" tools/effects.sh "$BUILD/k64run"
script "disks through drive 8 (directory and files byte for byte)" "$BUILD/disk.log" tools/disk.sh "$BUILD/k64run"
# Independent of each other: side by side.
side "VIC-II rule tests" "$BUILD/victest.log" "$BUILD/victest"
side "SID paddle converter rules" "$BUILD/sidtest.log" "$BUILD/sidtest"
side "6522 rule tests" "$BUILD/viatest.log" "$BUILD/viatest"
side "1541 board with its ROM" "$BUILD/drivetest.log" "$BUILD/drivetest" "$ROMS"
if [ -d "$EXT/singlestep/6502/v1" ]; then
    side "per-opcode bus traces" "$BUILD/cputest.log" "$BUILD/cputest" "$EXT/singlestep/6502/v1" --quiet
else bad "per-opcode suite missing: tools/fetch.sh singlestep"; fi
if [ -f "$EXT/dormann/bin_files/6502_functional_test.bin" ]; then
    side "functional test" "$BUILD/dormann.log" "$BUILD/imgtest" "$EXT/dormann/bin_files/6502_functional_test.bin@0" --start 400 --success 3469
else bad "functional test missing: tools/fetch.sh dormann"; fi
SEDDON="$(ls "$EXT"/seddon/releases/6502-tests-generic.*.dat 2>/dev/null | tail -1)"
if [ -n "$SEDDON" ]; then
    side "undocumented-opcode tests" "$BUILD/seddon.log" \
        "$BUILD/imgtest" "$SEDDON@2000" --start 2000 --end 202b --fail 2013 --print 2033 --max-cycles 10000000000
else bad "undocumented-opcode tests missing: tools/fetch.sh seddon"; fi
sides

step "CPU against the chip"
if [ -x "$BUILD/cpudiff" ]; then
    n=0
    for lines in "" "--irq" "--nmi" "--rdy" "--irq --nmi --rdy" "--rdy --volatile" "--irq --nmi --rdy --volatile"; do
        n=$((n + 1))
        # shellcheck disable=SC2086
        side "chip comparison (${lines:-no lines})" "$BUILD/cpudiff-$n.log" "$BUILD/cpudiff" --seeds 8 --cycles 20000 $lines
    done
    # The SO pin (hw/cpu.md rule 19), pulsed every few cycles, alone and with IRQ.
    side "chip comparison (SO, dense)" "$BUILD/cpudiff-so.log" "$BUILD/cpudiff" --seeds 8 --cycles 20000 --so --dense
    side "chip comparison (SO and IRQ, dense)" "$BUILD/cpudiff-so-irq.log" "$BUILD/cpudiff" --seeds 8 --first 100 --cycles 20000 --so --irq --dense
    # Flag and stack instructions alone, with the lines changing every few cycles.
    side "chip comparison (flag and stack instructions, dense)" "$BUILD/cpudiff-dense.log" \
        "$BUILD/cpudiff" --seeds 40 --cycles 20000 --irq --rdy --volatile --dense --only 08,28,58,78,ea,48,68,a9,40,e8
    sides
else bad "chip comparison not built: tools/fetch.sh perfect6502"; fi

step "SID against the reference engine"
if [ -x "$BUILD/siddiff" ]; then
    # A write every N cycles: dense traffic down to almost none (the last run leaves the
    # chip alone long enough for the 8580's slow fades). The long runs start first, so
    # that they share a batch.
    for run in "3000000 60000000 3" "40000 40000000 3" "400 3000000 6" "16 300000 6" "2 300000 6"; do
        # shellcheck disable=SC2086
        set -- $run
        for m in 6581 8580; do
            side "SID comparison ($m, a write every $1 cycles)" "$BUILD/siddiff-$m-$1.log" \
                "$BUILD/siddiff" --model $m --every "$1" --cycles "$2" --seeds "$3"
        done
        if [ "$1" = 40000 ]; then
            # The combined waveforms are a model, not the reference's sampled tables: its
            # match with the chip samples is a number, kept in tests/wave-score.txt. A
            # change of the model changes the numbers and has to change that file with them.
            side "combined waveform score" "$BUILD/wave-score.txt" "$BUILD/siddiff" --score "$EXT/testprogs/SID/resid-test"
            side "DAC tables against the reference" "$BUILD/dacs.log" "$BUILD/siddiff" --dacs
            # The analog part: tone measurements of both engines, inside their bands, and
            # the same numbers as recorded (a change of the model has to change
            # tests/filter-score.txt).
            side "filter, mixer and volume stage outside a band" "$BUILD/filter-score.txt" "$BUILD/siddiff" --filter
        fi
    done
    sides
    diff -u tests/wave-score.txt "$BUILD/wave-score.txt" || bad "combined waveforms: the score differs from tests/wave-score.txt"
    diff -u tests/filter-score.txt "$BUILD/filter-score.txt" || bad "analog part: the score differs from tests/filter-score.txt"
else bad "SID comparison not built: tools/oracle/build-vice.sh"; fi

step "SID chip variants"
# Real 6581s differ in what noise + pulse does to the noise register. The suite has programs
# from both kinds: its seven "_to_C_old" programs pass only on the kind that leaves the
# register alone (they are in tests/suite-known.txt for the default), its wf12nsr program
# only on the kind that drains it.
WB="$EXT/testprogs/SID"
if [ -f "$WB/wf12nsr/wf12nsr.prg" ]; then
    for w in 8 9 A C D E F; do
        "$BUILD/k64run" "$ROMS" --autostart "$WB/wb_testsuite/noise_writeback_check_${w}_to_C_old.prg" --sid old \
            --sid-noise-pulse keeps --debugcart --cycles 10000000 >/dev/null 2>&1 || bad "noise + pulse left alone: write-back program $w to C"
    done
    "$BUILD/k64run" "$ROMS" --autostart "$WB/wf12nsr/wf12nsr.prg" --sid old --sid-noise-pulse keeps --debugcart --cycles 10000000 >/dev/null 2>&1
    [ $? = 255 ] || bad "noise + pulse left alone: wf12nsr must fail on that kind of chip"
else bad "test programs missing: tools/oracle/build-vice.sh"; fi

if [ "$QUICK" = 0 ]; then
    step "RetroArch behind the wall"
    tools/wall.sh --check >/dev/null || bad "wall control"
    install -D -m 755 "$CORE" "$BUILD/bin/kernal64_libretro.so"
    if command -v retroarch >/dev/null; then
        # retroarch-check.sh loads bin/ of the repo; point it at the gate's own build.
        K64_CORE="$BUILD/bin/kernal64_libretro.so" K64_SYSTEM="$ROMS" timeout 120 tools/retroarch-check.sh 120 "$BUILD/ra.png" >"$BUILD/ra.log" 2>&1 \
            || { tail -5 "$BUILD/ra.log"; bad "RetroArch run"; }
        grep -q "\[Kernal64\] version" "$BUILD/ra.log" || bad "RetroArch did not log the core's version line"
        grep -q "missing system files" "$BUILD/ra.log" && bad "RetroArch run without the system ROMs"
    else
        echo "(RetroArch not installed: skipped)"
    fi
fi

if [ "$fails" = 0 ]; then echo "GATE: PASS"; exit 0; fi
echo "GATE: $fails failure(s)"; exit 1

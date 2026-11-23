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
# mixer and volume stage in bands; the audio output path as numbers and tones through the
# API; the VIC-II against the article's rules and the start-up
# screen against the reference emulator (tools/oracle/build-vice.sh); the reference
# suite's CPU, CIA, interrupt, general and VIC-II programs on both CIA types and both VIC-II
# generations; our own probe and raster-effect programs; the core loading in RetroArch
# behind the wall.
# The third-party suites come from tools/fetch.sh.
# Runs on a quarter of the cores at low priority; its build folder is removed at exit.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
QUICK=0; [ "${1:-}" = "--quick" ] && QUICK=1
JOBS="$(( $(nproc --all) / 4 > 0 ? $(nproc --all) / 4 : 1 ))"
BUILD="$(mktemp -d "$ROOT/build-gate.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
fails=0
step() { echo "== $*"; }
bad() { echo "GATE FAIL: $*"; fails=$((fails + 1)); }

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

step "libretro conformance"
for model in pal ntsc ntsc-old pal-new ntsc-new pal-old; do
    # Typing at the READY prompt through the API: BASIC must answer. Nothing has touched the
    # SID: every audio sample is zero.
    "$BUILD/k64host" "$CORE" --system "$ROMS" --frames 400 --check --option kernal64_model=$model \
        --type 'PRINT 6*7\n' --expect-screen ' 42' --expect-silence \
        >"$BUILD/host-$model.log" 2>&1 || { tail -5 "$BUILD/host-$model.log"; bad "conformance ($model)"; }
    grep -q "missing system files" "$BUILD/host-$model.log" && bad "system ROMs not found in $ROMS"
done
# The border option: the picture handed to the frontend has the size asked for.
for want in "none 320x200" "small 384x270"; do
    "$BUILD/k64host" "$CORE" --system "$ROMS" --frames 60 --check --option kernal64_border="${want% *}" >"$BUILD/host-border.log" 2>&1
    grep -q "last frame ${want#* } " "$BUILD/host-border.log" || bad "border option ${want% *}: not ${want#* }"
done

step "audio"
# The rate conversion and the board's output network, as numbers.
"$BUILD/audiotest" >"$BUILD/audiotest.log" 2>&1 || { tail -5 "$BUILD/audiotest.log"; bad "audio output path"; }
# Through the API: a triangle on voice 1 (frequency register 7493, full volume and sustain)
# comes out at register * clock / 2^24 Hz, one voice's level, on both chips and clocks.
for run in "pal 440.02 6581" "ntsc 456.76 6581" "ntsc-old 456.76 8580" "pal-new 440.02 8580"; do
    # shellcheck disable=SC2086
    set -- $run
    "$BUILD/k64host" "$CORE" --system "$ROMS" --frames 1000 --option kernal64_model=$1 --option kernal64_sid=$3 \
        --type 'POKE54296,15:POKE54277,0:POKE54278,240:POKE54272,69:POKE54273,29:POKE54276,17\n' --expect-tone $2 -18.5 -15.5 \
        >"$BUILD/host-tone.log" 2>&1 || { tail -3 "$BUILD/host-tone.log"; bad "tone ($1, $3)"; }
done
# The same tone with the conformance checks: the state saved half-way holds a filter and an
# output network in motion, and the replay from it must end in the same state byte for byte.
"$BUILD/k64host" "$CORE" --system "$ROMS" --frames 1600 --check \
    --type 'POKE54296,15:POKE54277,0:POKE54278,240:POKE54272,69:POKE54273,29:POKE54276,17\n' --expect-tone 440.02 -18.5 -15.5 \
    >"$BUILD/host-tone.log" 2>&1 || { tail -3 "$BUILD/host-tone.log"; bad "conformance with a tone playing"; }
# Through the filter (8580, cutoff at its top, about 20 kHz): with the voice routed into it
# the low-pass lets the 440 Hz tone through at its level, the high-pass leaves about 1/400
# of it. (Two typed lines: BASIC takes 80 characters per line.)
for run in "31 -18.5 -15.5 low-pass" "79 -80 -60 high-pass"; do
    # shellcheck disable=SC2086
    set -- $run
    "$BUILD/k64host" "$CORE" --system "$ROMS" --frames 1300 --option kernal64_sid=8580 \
        --type "POKE54295,1:POKE54294,255:POKE54296,$1\nPOKE54277,0:POKE54278,240:POKE54272,69:POKE54273,29:POKE54276,17\n" --expect-level $2 $3 \
        >"$BUILD/host-tone.log" 2>&1 || { tail -3 "$BUILD/host-tone.log"; bad "tone through the $4"; }
done

step "memory map and boot"
if [ -f "$ROOT/gitignore/external/pla/PLA.BIN" ]; then
    "$BUILD/placheck" "$ROOT/gitignore/external/pla/PLA.BIN" >"$BUILD/pla.log" 2>&1 \
        || { tail -4 "$BUILD/pla.log"; bad "PLA against the chip read-out"; }
else bad "PLA read-out missing: tools/fetch.sh pla"; fi
for model in pal ntsc ntsc-old pal-new ntsc-new pal-old; do
    want=$([ "${model#pal}" != "$model" ] && echo PAL || echo NTSC)
    "$BUILD/k64run" "$ROMS" --frames 150 --model $model --expect "READY." --expect "38911 BASIC BYTES FREE" \
        >"$BUILD/boot-$model.log" 2>&1 || { tail -3 "$BUILD/boot-$model.log"; bad "boot to the BASIC prompt ($model)"; }
    grep -q "KERNAL detected $want" "$BUILD/boot-$model.log" || bad "KERNAL did not detect $want ($model)"
done

step "start-up screen against the reference emulator"
if [ -x "$ROOT/gitignore/external/vice/build/src/x64sc" ] && command -v podman >/dev/null; then
    tools/oracle/refcheck.sh "$BUILD/k64run" "$BUILD/refcheck" >"$BUILD/refcheck.log" 2>&1 \
        || { tail -6 "$BUILD/refcheck.log"; bad "start-up screen differs from the reference"; }
else bad "reference emulator missing: tools/oracle/build-vice.sh"; fi

step "CPU, CIA, interrupt, general, VIC-II and SID test programs against the reference emulator"
# (Lorenz's suite is in "general". Programs waiting for later hardware: tests/suite-known.txt.)
if [ -f "$ROOT/gitignore/external/testprogs/testbench/c64-testlist.in" ]; then
    tools/suite.sh '^\.\./(CIA|interrupts|general|CPU|VICII|SID)/' "$BUILD/k64run" >"$BUILD/suite.log" 2>&1 \
        || { grep -E '^(WORSE|FIXED)' "$BUILD/suite.log" | head -8; bad "test programs worse than the reference"; }
    tail -1 "$BUILD/suite.log"
else bad "test programs missing: tools/oracle/build-vice.sh"; fi

step "own probe programs"
tools/probes.sh "$BUILD/k64run" >"$BUILD/probes.log" 2>&1 || { grep -v '^ok' "$BUILD/probes.log" | head -8; bad "probe programs"; }

step "own raster-effect programs"
tools/effects.sh "$BUILD/k64run" >"$BUILD/effects.log" 2>&1 || { grep -v '^ok' "$BUILD/effects.log" | head -8; bad "raster-effect programs"; }

step "VIC-II rules"
"$BUILD/victest" >"$BUILD/victest.log" 2>&1 || { grep FAIL "$BUILD/victest.log" | head -8; bad "VIC-II rule tests"; }

step "CPU"
EXT="$ROOT/gitignore/external"
if [ -d "$EXT/singlestep/6502/v1" ]; then
    "$BUILD/cputest" "$EXT/singlestep/6502/v1" --quiet >"$BUILD/cputest.log" 2>&1 \
        || { tail -5 "$BUILD/cputest.log"; bad "per-opcode bus traces"; }
else bad "per-opcode suite missing: tools/fetch.sh singlestep"; fi
if [ -f "$EXT/dormann/bin_files/6502_functional_test.bin" ]; then
    "$BUILD/imgtest" "$EXT/dormann/bin_files/6502_functional_test.bin@0" --start 400 --success 3469 >"$BUILD/dormann.log" 2>&1 \
        || { tail -3 "$BUILD/dormann.log"; bad "functional test"; }
else bad "functional test missing: tools/fetch.sh dormann"; fi
SEDDON="$(ls "$EXT"/seddon/releases/6502-tests-generic.*.dat 2>/dev/null | tail -1)"
if [ -n "$SEDDON" ]; then
    "$BUILD/imgtest" "$SEDDON@2000" --start 2000 --end 202b --fail 2013 --print 2033 --max-cycles 10000000000 \
        >"$BUILD/seddon.log" 2>&1 || { grep -v " - ok" "$BUILD/seddon.log" | tail -5; bad "undocumented-opcode tests"; }
else bad "undocumented-opcode tests missing: tools/fetch.sh seddon"; fi
if [ -x "$BUILD/cpudiff" ]; then
    for lines in "" "--irq" "--nmi" "--rdy" "--irq --nmi --rdy" "--rdy --volatile" "--irq --nmi --rdy --volatile"; do
        # shellcheck disable=SC2086
        "$BUILD/cpudiff" --seeds 8 --cycles 20000 $lines >"$BUILD/cpudiff.log" 2>&1 \
            || { grep -A3 DIFFERS "$BUILD/cpudiff.log" | head -5; bad "chip comparison (${lines:-no lines})"; }
    done
    # Flag and stack instructions alone, with the lines changing every few cycles.
    "$BUILD/cpudiff" --seeds 40 --cycles 20000 --irq --rdy --volatile --dense --only 08,28,58,78,ea,48,68,a9,40,e8 >"$BUILD/cpudiff.log" 2>&1 \
        || { grep -A3 DIFFERS "$BUILD/cpudiff.log" | head -5; bad "chip comparison (flag and stack instructions, dense)"; }
else bad "chip comparison not built: tools/fetch.sh perfect6502"; fi

step "SID against the reference engine"
if [ -x "$BUILD/siddiff" ]; then
    for m in 6581 8580; do
        # A write every N cycles: dense traffic down to almost none (the last run leaves the
        # chip alone long enough for the 8580's slow fades).
        for run in "2 300000 6" "16 300000 6" "400 3000000 6" "40000 40000000 3" "3000000 60000000 3"; do
            # shellcheck disable=SC2086
            set -- $run
            "$BUILD/siddiff" --model $m --every "$1" --cycles "$2" --seeds "$3" >"$BUILD/siddiff.log" 2>&1 \
                || { grep -A7 DIFFERS "$BUILD/siddiff.log" | head -8; bad "SID comparison ($m, a write every $1 cycles)"; }
        done
    done
    # The combined waveforms are a model, not the reference's sampled tables: its match
    # with the chip samples is a number, kept in tests/wave-score.txt. A change of the
    # model changes the numbers and has to change that file with them.
    "$BUILD/siddiff" --score "$EXT/testprogs/SID/resid-test" >"$BUILD/wave-score.txt" 2>&1
    diff -u tests/wave-score.txt "$BUILD/wave-score.txt" || bad "combined waveforms: the score differs from tests/wave-score.txt"
    "$BUILD/siddiff" --dacs >"$BUILD/dacs.log" 2>&1 || { cat "$BUILD/dacs.log"; bad "DAC tables against the reference"; }
    # The analog part: tone measurements of both engines, inside their bands, and the same
    # numbers as recorded (a change of the model has to change tests/filter-score.txt).
    "$BUILD/siddiff" --filter >"$BUILD/filter-score.txt" 2>&1 || { grep FAIL "$BUILD/filter-score.txt"; bad "filter, mixer and volume stage outside a band"; }
    diff -u tests/filter-score.txt "$BUILD/filter-score.txt" || bad "analog part: the score differs from tests/filter-score.txt"
else bad "SID comparison not built: tools/oracle/build-vice.sh"; fi

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

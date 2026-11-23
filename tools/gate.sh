#!/usr/bin/env bash
# The one gate. Exit code 0 only if every step passed.
#
#   tools/gate.sh [--quick]     --quick skips the RetroArch run
#
# Steps: nothing forbidden is tracked; a clean build with warnings as errors;
# the libretro conformance checks on every machine model; the PLA against a chip
# read-out and the KERNAL booting to BASIC on every model; the CPU against the
# per-opcode bus traces, the functional and undocumented-opcode test programs and
# the transistor-level chip; the VIC-II against the article's rules and the start-up
# screen against the reference emulator (tools/oracle/build-vice.sh); the reference
# suite's CPU, CIA, interrupt, general and VIC-II programs on both CIA types; our own probe
# programs; the core loading in RetroArch behind the wall.
# The third-party suites come from tools/fetch.sh.
# Runs on a quarter of the cores at low priority; its build folder is removed at exit.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
QUICK=0; [ "${1:-}" = "--quick" ] && QUICK=1
JOBS="$(( $(nproc) / 4 > 0 ? $(nproc) / 4 : 1 ))"
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
for model in pal ntsc ntsc-old; do
    # Typing at the READY prompt through the API: BASIC must answer.
    "$BUILD/k64host" "$CORE" --system "$ROMS" --frames 400 --check --option kernal64_model=$model \
        --type 'PRINT 6*7\n' --expect-screen ' 42' \
        >"$BUILD/host-$model.log" 2>&1 || { tail -5 "$BUILD/host-$model.log"; bad "conformance ($model)"; }
    grep -q "missing system files" "$BUILD/host-$model.log" && bad "system ROMs not found in $ROMS"
done
# The border option: the picture handed to the frontend has the size asked for.
for want in "none 320x200" "small 384x270"; do
    "$BUILD/k64host" "$CORE" --system "$ROMS" --frames 60 --check --option kernal64_border="${want% *}" >"$BUILD/host-border.log" 2>&1
    grep -q "last frame ${want#* } " "$BUILD/host-border.log" || bad "border option ${want% *}: not ${want#* }"
done

step "memory map and boot"
if [ -f "$ROOT/gitignore/external/pla/PLA.BIN" ]; then
    "$BUILD/placheck" "$ROOT/gitignore/external/pla/PLA.BIN" >"$BUILD/pla.log" 2>&1 \
        || { tail -4 "$BUILD/pla.log"; bad "PLA against the chip read-out"; }
else bad "PLA read-out missing: tools/fetch.sh pla"; fi
for model in pal ntsc ntsc-old; do
    want=$([ "$model" = pal ] && echo PAL || echo NTSC)
    "$BUILD/k64run" "$ROMS" --frames 150 --model $model --expect "READY." --expect "38911 BASIC BYTES FREE" \
        >"$BUILD/boot-$model.log" 2>&1 || { tail -3 "$BUILD/boot-$model.log"; bad "boot to the BASIC prompt ($model)"; }
    grep -q "KERNAL detected $want" "$BUILD/boot-$model.log" || bad "KERNAL did not detect $want ($model)"
done

step "start-up screen against the reference emulator"
if [ -x "$ROOT/gitignore/external/vice/build/src/x64sc" ] && command -v podman >/dev/null; then
    tools/oracle/refcheck.sh "$BUILD/k64run" "$BUILD/refcheck" >"$BUILD/refcheck.log" 2>&1 \
        || { tail -6 "$BUILD/refcheck.log"; bad "start-up screen differs from the reference"; }
else bad "reference emulator missing: tools/oracle/build-vice.sh"; fi

step "CPU, CIA, interrupt, general and VIC-II test programs against the reference emulator"
# (Lorenz's suite is in "general". Programs waiting for later hardware: tests/suite-known.txt.)
if [ -f "$ROOT/gitignore/external/testprogs/testbench/c64-testlist.in" ]; then
    tools/suite.sh '^\.\./(CIA|interrupts|general|CPU|VICII)/' "$BUILD/k64run" >"$BUILD/suite.log" 2>&1 \
        || { grep -E '^(WORSE|FIXED)' "$BUILD/suite.log" | head -8; bad "test programs worse than the reference"; }
    tail -1 "$BUILD/suite.log"
else bad "test programs missing: tools/oracle/build-vice.sh"; fi

step "own probe programs"
tools/probes.sh "$BUILD/k64run" >"$BUILD/probes.log" 2>&1 || { grep -v '^ok' "$BUILD/probes.log" | head -8; bad "probe programs"; }

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
else bad "chip comparison not built: tools/fetch.sh perfect6502"; fi

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

#!/usr/bin/env bash
# The one gate. Exit code 0 only if every step passed.
#
#   tools/gate.sh [--quick]     --quick skips the RetroArch run
#
# Steps: nothing forbidden is tracked; a clean build with warnings as errors;
# the libretro conformance checks on every machine model; the test suites that
# exist so far; the core loading in RetroArch behind the wall.
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

step "libretro conformance"
for model in pal ntsc ntsc-old; do
    "$BUILD/k64host" "$CORE" --frames 300 --check --option kernal64_model=$model >"$BUILD/host-$model.log" 2>&1 \
        || { tail -5 "$BUILD/host-$model.log"; bad "conformance ($model)"; }
done

if [ "$QUICK" = 0 ]; then
    step "RetroArch behind the wall"
    tools/wall.sh --check >/dev/null || bad "wall control"
    install -D -m 755 "$CORE" "$BUILD/bin/kernal64_libretro.so"
    if command -v retroarch >/dev/null; then
        # retroarch-check.sh loads bin/ of the repo; point it at the gate's own build.
        K64_CORE="$BUILD/bin/kernal64_libretro.so" timeout 120 tools/retroarch-check.sh 120 "$BUILD/ra.png" >"$BUILD/ra.log" 2>&1 \
            || { tail -5 "$BUILD/ra.log"; bad "RetroArch run"; }
        grep -q "\[Kernal64\] version" "$BUILD/ra.log" || bad "RetroArch did not log the core's version line"
    else
        echo "(RetroArch not installed: skipped)"
    fi
fi

if [ "$fails" = 0 ]; then echo "GATE: PASS"; exit 0; fi
echo "GATE: $fails failure(s)"; exit 1

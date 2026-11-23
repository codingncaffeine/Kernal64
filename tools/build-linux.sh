#!/usr/bin/env bash
# Builds the core and the test tools, then refreshes bin/:
#   bin/kernal64_libretro.so   the core
#   bin/kernal64               starts it in RetroArch (arguments are passed on)
#   bin/k64host                the test host
#
#   tools/build-linux.sh [Release|Debug]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TYPE="${1:-Release}"
BUILD="$ROOT/build-${TYPE,,}"
JOBS="${K64_JOBS:-$(( $(nproc) / 4 > 0 ? $(nproc) / 4 : 1 ))}"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$TYPE" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
nice -n 10 cmake --build "$BUILD" -j "$JOBS"
ln -sf "$BUILD/compile_commands.json" "$ROOT/compile_commands.json"

mkdir -p "$ROOT/bin"
install -m 755 "$BUILD/kernal64_libretro.so" "$ROOT/bin/kernal64_libretro.so"
install -m 755 "$BUILD/k64host" "$ROOT/bin/k64host"
install -m 755 "$BUILD/cputest" "$ROOT/bin/cputest"
install -m 755 "$BUILD/imgtest" "$ROOT/bin/imgtest"
install -m 755 "$BUILD/placheck" "$ROOT/bin/placheck"
install -m 755 "$BUILD/k64run" "$ROOT/bin/k64run"
[ -f "$BUILD/cpudiff" ] && install -m 755 "$BUILD/cpudiff" "$ROOT/bin/cpudiff"
cat > "$ROOT/bin/kernal64" <<'EOF'
#!/bin/sh
# Starts the Kernal64 core in RetroArch. Any arguments (a disk or program file) are passed on.
exec retroarch -L "$(dirname "$(readlink -f "$0")")/kernal64_libretro.so" "$@"
EOF
chmod 755 "$ROOT/bin/kernal64"
echo "built: $ROOT/bin/kernal64_libretro.so"

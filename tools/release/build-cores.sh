#!/usr/bin/env bash
# Builds the release cores in containers (podman): Linux x86-64 against glibc 2.28 (manylinux_2_28:
# runs on distributions from 2018 on), Linux x86-64 against musl (Alpine), and 64-bit Windows
# (MinGW-w64, runtimes linked in). macOS is built by the release workflow on its runners.
#   tools/release/build-cores.sh [linux|musl|windows ...]     default: all three
# Out: dist/kernal64_libretro-<system>.zip, each holding the core, kernal64_libretro.info and LICENSE;
# dist/k64host-<system> beside them, for checking the core on that system.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DIST="$ROOT/dist"; mkdir -p "$DIST"
JOBS="${K64_JOBS:-4}"
want() { [ $# -eq 1 ] && return 0; local n; for n in "${@:2}"; do [ "$n" = "$1" ] && return 0; done; return 1; }

# build NAME IMAGE SETUP CMAKE_ARGS CORE_FILE
build() {
    local name="$1" image="$2" setup="$3" args="$4" file="$5"
    echo "== $name ($image)"
    local b="/k64/build-rel-$name"
    podman run --rm -v "$ROOT:/k64:Z" -w /k64 "$image" sh -c "
        set -e; $setup
        rm -rf $b
        cmake -S /k64 -B $b -DCMAKE_BUILD_TYPE=Release -DK64_CORE_ONLY=ON -DK64_WERROR=ON $args >/dev/null
        cmake --build $b -j $JOBS
        strip $( [ "$name" = windows-x86_64 ] && echo '--strip-unneeded' ) $b/$file 2>/dev/null || true"
    local tmp; tmp="$(mktemp -d)"
    cp "$ROOT/build-rel-$name/$file" "$ROOT/packaging/kernal64_libretro.info" "$ROOT/LICENSE" "$tmp/"
    rm -f "$DIST/kernal64_libretro-$name.zip"
    (cd "$tmp" && zip -q -9 "$DIST/kernal64_libretro-$name.zip" "$file" kernal64_libretro.info LICENSE)
    # k64host for this system too, kept beside the zip for checking the core there.
    for h in k64host k64host.exe; do [ -f "$ROOT/build-rel-$name/$h" ] && cp "$ROOT/build-rel-$name/$h" "$DIST/k64host-$name${h#k64host}"; done
    rm -rf "$tmp" "$ROOT/build-rel-$name"
    echo "   dist/kernal64_libretro-$name.zip"
}

if want linux "$@"; then
    build linux-x86_64 quay.io/pypa/manylinux_2_28_x86_64 \
        "pipx install cmake >/dev/null 2>&1 || pip3 install -q cmake; export PATH=/root/.local/bin:\$PATH" \
        "" kernal64_libretro.so
fi
if want musl "$@"; then
    build linux-x86_64-musl docker.io/library/alpine:3.20 "apk add -q --no-progress g++ cmake make" "" kernal64_libretro.so
fi
if want windows "$@"; then
    build windows-x86_64 docker.io/library/debian:bookworm \
        "apt-get update -qq >/dev/null && apt-get install -y -qq g++-mingw-w64-x86-64-posix cmake make >/dev/null" \
        "-DCMAKE_TOOLCHAIN_FILE=/k64/tools/release/mingw-w64.cmake" kernal64_libretro.dll
fi

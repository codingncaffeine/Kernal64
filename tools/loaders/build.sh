#!/usr/bin/env bash
# Fetches the open-source fast loaders that need building (pinned) and builds them in the
# k64-loaders container (tools/loaders/Containerfile) with no network:
#   Bitfire (Tobias Bindhammer, BSD-3-Clause): its loader, its own checksum benchmark
#   (benchmark/checksum.asm, CHECKSUM = 1) and a disk of the benchmark's files.
# Results stay in gitignore/external/loaders/; nothing here is committed or shipped.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DIR="$ROOT/gitignore/external/loaders"
mkdir -p "$DIR"
podman image exists k64-loaders || nice podman build -q -t k64-loaders "$ROOT/tools/loaders" >/dev/null

BITFIRE_PIN=f8ef98d1cc102b8168cc6c5eaf663043734b2784
B="$DIR/bitfire"
if [ ! -d "$B/.git" ] || [ "$(git -C "$B" rev-parse HEAD)" != "$BITFIRE_PIN" ]; then
    rm -rf "$B"
    git clone -q https://github.com/bboxy/bitfire.git "$B"
    git -C "$B" -c advice.detachedHead=false checkout -q "$BITFIRE_PIN"
    git -C "$B" submodule update --init -q
fi
if [ ! -s "$B/benchmark/k64-bench.d64" ]; then
    files=$(grep -o 'files/[a-z]*\.lz' "$B/benchmark/Makefile" | sed 's/^/-b /' | tr '\n' ' ')
    podman run --rm --network none -v "$B":/src:Z -w /src k64-loaders sh -c \
        "nice make -C loader >/dev/null && nice make -C d64write >/dev/null && cd benchmark && acme -f cbm -o benchmark.prg checksum.asm && ../d64write/d64write -c k64-bench.d64 -h test -i ab $files >/dev/null"
fi
echo "loaders: bitfire built at ${BITFIRE_PIN:0:8}"

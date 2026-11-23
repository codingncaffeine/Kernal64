#!/usr/bin/env bash
# Builds the reference cycle-exact emulator (VICE x64sc, headless UI) in a container and
# checks out the VICE test-program repository, both into gitignore/external/. They are
# used only as an oracle for reference frames and test results; nothing from them is
# committed or shipped.
#
#   tools/oracle/build-vice.sh
#
# Pins: the release tarball's SHA-256 and the test-program revision are recorded on the
# first run (vice/*.k64-sha256, testprogs/.k64-rev) and checked on every later run.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
EXT="$ROOT/gitignore/external"
VICE_VER=3.9
mkdir -p "$EXT/vice"
podman build -q -t k64-vice-oracle "$ROOT/tools/oracle" >/dev/null
podman run --rm --cpus 6 -e VICE_VER="$VICE_VER" -v "$EXT":/ext:Z k64-vice-oracle bash -euo pipefail -c '
  cd /ext/vice
  tar="vice-$VICE_VER.tar.gz"
  [ -f "$tar" ] || curl -fsSL -o "$tar" "https://sourceforge.net/projects/vice-emu/files/releases/$tar/download"
  sum="$(sha256sum "$tar" | cut -d" " -f1)"
  if [ -f "$tar.k64-sha256" ]; then [ "$(cat "$tar.k64-sha256")" = "$sum" ] || { echo "checksum changed"; exit 1; }
  else echo "$sum" > "$tar.k64-sha256"; fi
  if [ ! -x build/src/x64sc ]; then
    rm -rf src-tree build && mkdir src-tree build
    tar -xzf "$tar" -C src-tree --strip-components=1
    cd build
    ../src-tree/configure --enable-headlessui --disable-pdf-docs --disable-html-docs \
        --without-pulse --without-alsa --without-oss --disable-ffmpeg --without-libcurl \
        > /ext/vice/configure.log 2>&1
    nice -n 10 make -j6 > /ext/vice/make.log 2>&1
    cd ..
  fi
  ls -l build/src/x64sc
  cd /ext
  if [ ! -d testprogs/.svn ]; then
    rev="$(cat testprogs.k64-rev 2>/dev/null || echo HEAD)"
    svn checkout -q -r "$rev" https://svn.code.sf.net/p/vice-emu/code/testprogs testprogs
  fi
  svn info --show-item revision testprogs > testprogs.k64-rev
  echo "testprogs at r$(cat testprogs.k64-rev)"
'

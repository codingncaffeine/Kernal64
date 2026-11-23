#!/usr/bin/env bash
# Fetches the third-party test suites and oracles into gitignore/external/, each at
# a pinned revision. Nothing fetched here is ever committed or shipped.
#
#   tools/fetch.sh [NAME...]     default: all
#
# Re-running is cheap: a source already at its pin is left alone.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXT="$ROOT/gitignore/external"
mkdir -p "$EXT"

# name | repository | pinned commit | sparse paths (empty = whole tree) | license as found
SOURCES=(
  "singlestep|https://github.com/SingleStepTests/65x02.git|2f6980a2d95757486c7bee24355c360e40e2a224|6502/v1|MIT"
  "dormann|https://github.com/klaus2m5/6502_65c02_functional_tests.git|7954e2dbb49c469ea286070bf46cdd71aeb29e4b||GPL-3.0"
  "seddon|https://github.com/tom-seddon/6502-tests.git|6ecccbfebdb9e10588b1019166bcb5b3b6e617bc||GPL-3.0"
  "perfect6502|https://github.com/mist64/perfect6502.git|09fc542877a84318291aa42dab143a3e2c3db974||MIT"
)

want() { [ $# -eq 1 ] && return 0; local n; for n in "${@:2}"; do [ "$n" = "$1" ] && return 0; done; return 1; }

for entry in "${SOURCES[@]}"; do
    IFS='|' read -r name url pin sparse license <<<"$entry"
    want "$name" "$@" || continue
    dir="$EXT/$name"
    if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$pin" ]; then
        echo "fetch: $name at ${pin:0:8} (already)"; continue
    fi
    rm -rf "$dir"
    if [ -n "$sparse" ]; then
        git clone -q --filter=blob:none --no-checkout "$url" "$dir"
        git -C "$dir" sparse-checkout set --no-cone "/$sparse/"
    else
        git clone -q --filter=blob:none --no-checkout "$url" "$dir"
    fi
    git -C "$dir" -c advice.detachedHead=false checkout -q "$pin"
    echo "$license" > "$dir/.k64-license"
    echo "fetch: $name at ${pin:0:8} ($license)"
done

# Spindle 3.1 (Linus Akesson, MIT): a fast loader with its own disk-building tool, for the
# fast-loader test (tools/loaders.sh). A release archive, pinned by its checksum.
if want spindle "$@"; then
    dir="$EXT/loaders"; zip="$dir/spindle-3.1.zip"
    sum=4abbf757ed550d1da7aac3d9f04cf10566a1f6e9079780e02f84cfd4a164431b
    mkdir -p "$dir"
    if [ -f "$zip" ] && echo "$sum  $zip" | sha256sum -c --quiet 2>/dev/null; then
        echo "fetch: spindle 3.1 (already)"
    else
        curl -sfL -A curl -o "$zip" https://hd0.linusakesson.net/files/spindle-3.1.zip
        echo "$sum  $zip" | sha256sum -c --quiet || { rm -f "$zip"; echo "fetch: spindle: checksum differs"; exit 1; }
        rm -rf "$dir/spindle-3.1" && mkdir -p "$dir/spindle-3.1" && unzip -q -o "$zip" -d "$dir/spindle-3.1"
        echo "fetch: spindle 3.1 (MIT)"
    fi
fi

# The PLA read-out (PLA.BIN, 64 KB, one byte of outputs per input combination) comes from
# the user's own copy of Jens Schoenfeld's archive, gitignore/pla.zip; it is unpacked, never
# downloaded.
if want pla "$@"; then
    if [ -f "$ROOT/gitignore/pla.zip" ]; then
        mkdir -p "$EXT/pla"
        unzip -o -q "$ROOT/gitignore/pla.zip" PLA.BIN -d "$EXT/pla"
        echo "fetch: pla (PLA.BIN unpacked from gitignore/pla.zip)"
    else
        echo "fetch: pla skipped (no gitignore/pla.zip)"
    fi
fi

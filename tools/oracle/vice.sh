#!/usr/bin/env bash
# Runs the reference emulator (built by tools/oracle/build-vice.sh) headless in its
# container, with no network, no sound hardware and no input. The repository is mounted
# at /k64, so file arguments are given relative to the repository root as /k64/... paths.
#
# Its sound chip must be running for any program that reads it, so sound emulation is on
# and written to /dev/null through the file-writing device. (With the "dummy" device the
# sample buffer is never emptied: after the first buffer the SID stops being clocked,
# and programs that wait for an oscillator or envelope value never finish.)
#
#   tools/oracle/vice.sh [x64sc options...]
#
# (-t: VICE 3.9 crashes while logging when its output is not a terminal.)
# -default comes first: it resets the options given before it. The ROMs are named
# explicitly (the -directory search path did not find them); they are byte-identical
# to the user's (SHA-1 checked 2026-11-22).
# Colors come from tools/oracle/index.vpl (color i = R i*16, G 255-i*16) with no CRT
# filter, so a screenshot maps back to color indices exactly.
# Memory at power on is set to our machine's pattern (64 bytes $00, 64 bytes $FF, ...) with
# no random bits: the reference's default pattern differs and flips random bits, so a
# program that shows memory it never wrote would differ from run to run.
# K64_VICE_MODEL picks the machine (c64 = 6569 PAL, ntsc = 6567R8, oldntsc = 6567R56A);
# it is set before the ROMs because choosing a model resets them.
# Typical: -limitcycles N -exitscreenshot /k64/scratch/out.png -VICIIborders 2 (whole raster)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VICE="/k64/gitignore/external/vice"
exec podman run --rm -t --network none --cpus 1 -v "$ROOT":/k64:Z k64-vice-oracle \
    nice -n 10 "$VICE/build/src/x64sc" -default -model "${K64_VICE_MODEL:-c64}" \
    -kernal "$VICE/src-tree/data/C64/kernal-901227-03.bin" -basic "$VICE/src-tree/data/C64/basic-901226-01.bin" \
    -chargen "$VICE/src-tree/data/C64/chargen-901225-01.bin" \
    -VICIIextpal -VICIIpalette /k64/tools/oracle/index.vpl -VICIIfilter 0 \
    -raminitstartvalue 0 -raminitvalueinvert 64 -raminitvalueoffset 0 -raminitpatterninvert 0 \
    -raminitstartrandom 0 -raminitrepeatrandom 0 -raminitrandomchance 0 \
    -sound -sounddev wav -soundarg /dev/null "$@"

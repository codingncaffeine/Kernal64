#!/usr/bin/env bash
# The datasette (hw/tape.md): the real KERNAL saves a program onto a blank tape and loads it
# back; the suite's tap204060 programs record exactly the pulse lengths their documentation
# draws; a tape as content starts by itself through the core. Exit status = the verdict.
#   tools/tape.sh [BUILD_DIR]     (k64run, k64host and the core from BUILD_DIR, default bin/)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="${1:-$ROOT/bin}"
CORE="$B/kernal64_libretro.so"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/k64tape.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
rc=0

# 1. SAVE onto a blank tape (RECORD+PLAY down), then LOAD it back (PLAY down) and RUN.
"$B/k64run" "$ROOT/gitignore" --blank-tape --record --cycles 60000000 --type-at 3000000 $'10 ?4242\r' \
    --type-at 3300000 'SAVE"T"'$'\r' --save-tape "$OUT/kernal.tap" >"$OUT/save.log" 2>&1
if "$B/k64run" "$ROOT/gitignore" --tape "$OUT/kernal.tap" --play --cycles 60000000 --type-at 3000000 $'LOAD\r' \
       --type-at 55000000 $'RUN\r' --expect ' 4242' >"$OUT/load.log" 2>&1; then
    echo "tape: the KERNAL saves to tape and loads it back"
else
    echo "FAIL tape: KERNAL save + load"; grep -v '^ *$' "$OUT/load.log" | tail -4; rc=1
fi

# 2. The suite's pulse writer (tape/tap204060): $20 $40 $60 $40, inverted $30 $50 ...
T="$ROOT/gitignore/external/testprogs/tape/tap204060"
pulses() {   # the first N pulses of a TAP v1 as TAP units (cycles / 8), or the cycles if not whole
    python3 -I - "$1" "$2" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()[20:]
out, i = [], 0
while i < len(d) and len(out) < int(sys.argv[2]):
    b = d[i]; i += 1
    v = b * 8 if b else d[i] | d[i + 1] << 8 | d[i + 2] << 16
    i += 0 if b else 3
    out.append(f"{v // 8:02x}" if v % 8 == 0 and v <= 255 * 8 else str(v))
print(' '.join(out))
PY
}
for p in "tap204060once:20 40 60" "tap204060invonce:30 50"; do
    prg="${p%%:*}"; want="${p#*:}"
    "$B/k64run" "$ROOT/gitignore" --blank-tape --record --autostart "$T/$prg.prg" --cycles 30000000 \
        --save-tape "$OUT/$prg.tap" >/dev/null 2>&1
    got=$(pulses "$OUT/$prg.tap" 4)
    # The inverted program starts with its long lead-in pause; compare what follows it.
    [ "$prg" = tap204060invonce ] && got="${got#* }"
    case "$got" in
        "$want"*) echo "tape: $prg records $want ..." ;;
        *) echo "FAIL tape: $prg recorded '$got', not '$want ...'"; rc=1 ;;
    esac
done

# 3. A tape as content: PLAY down, LOAD and RUN typed by the core.
if "$B/k64host" "$CORE" --system "$ROOT/gitignore" --save "$OUT" --content "$OUT/kernal.tap" --frames 2700 \
       --expect-screen ' 4242' >"$OUT/host.log" 2>&1; then
    echo "tape: a tape as content starts by itself"
else
    echo "FAIL tape: as content"; grep -E '^FAIL' "$OUT/host.log" | head -2; rc=1
fi
exit $rc

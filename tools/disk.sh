#!/usr/bin/env bash
# Disks through drive 8, the stock KERNAL and DOS: the directory of a suite image lists, and
# its first program files load byte for byte as an independent reader of the image
# (tools/d64file.py) finds them, with the line links BASIC's LOAD rebuilds. Runs side by side; exit status = the verdict.
#   tools/disk.sh [K64RUN]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
K64RUN="${1:-$ROOT/bin/k64run}"
IMAGE="$ROOT/gitignore/external/testprogs/vdrive/test.d64"
[ -f "$IMAGE" ] || { echo "disk: $IMAGE missing (tools/fetch.sh)"; exit 2; }
OUT="$(mktemp -d "${TMPDIR:-/tmp}/k64disk.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

# TEXT typed in pieces of ten keys (the KERNAL's buffer), from cycle 4,000,000 on.
typed() {
    local text="${1//\\r/$'\r'}" t=4000000 i
    for ((i = 0; i < ${#text}; i += 10)); do printf -- '--type-at\n%d\n%s\n' "$t" "${text:i:10}"; t=$((t + 300000)); done
}

check_dir() {
    mapfile -t args < <(typed 'LOAD"$",8\r')
    "$K64RUN" "$ROOT/gitignore" --disk "$IMAGE" --cycles 20000000 "${args[@]}" --type-at 19000000 'LIST\r' \
        --expect 'BLOCKS FREE.' --expect '"DRIVEMEM"' >"$OUT/dir.log" 2>&1 || { echo "FAIL directory"; tail -20 "$OUT/dir.log"; return 1; }
}

check_file() {
    local name="$1" want len cycles got
    want=$(python3 -I "$ROOT/tools/d64file.py" "$IMAGE" "$name" --relinked) || { echo "FAIL $name: not in the image"; return 1; }
    len=$(( ${#want} / 2 - 2 ))
    cycles=$(( 8000000 + len * 3000 ))
    mapfile -t args < <(typed "LOAD\"$name\",8\\r")
    "$K64RUN" "$ROOT/gitignore" --disk "$IMAGE" --cycles "$cycles" "${args[@]}" --dump "0801:$(printf %x "$len")" \
        >"$OUT/$name.log" 2>&1
    got=$(grep '^[0-9a-f]\{4\}:' "$OUT/$name.log" | cut -d: -f2 | tr -d ' \n')
    [ "$got" = "${want:4}" ] || { echo "FAIL $name: the loaded bytes differ"; grep -v '^[0-9a-f]\{4\}:' "$OUT/$name.log" | grep -v '^ *$' | tail -5; return 1; }
}

pids=()
check_dir & pids+=($!)
for f in DRIVEMEM FILELEN CHECKDISK; do check_file "$f" & pids+=($!); done
rc=0
for p in "${pids[@]}"; do wait "$p" || rc=1; done
[ $rc = 0 ] && echo "disk: directory and 3 files exact"
exit $rc

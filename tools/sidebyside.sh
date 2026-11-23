# Sourced by the test scripts: runs their independent lines side by side.
#   sbs_start COMMAND...   starts one job (a shell function of the script, as a rule); at
#                          most K64_JOBS run at a time (default: a quarter of the cores)
#   sbs_finish             waits for all, prints their output in the order they were
#                          started, and returns 1 unless every job printed a line that
#                          starts with "ok" and none one that starts with "FAIL"
# The caller sets OUT (a folder for work files) before sourcing.
SBS_JOBS="${K64_JOBS:-$(( $(nproc --all) / 4 > 0 ? $(nproc --all) / 4 : 1 ))}"
SBS_DIR="$(mktemp -d "$OUT/side.XXXXXX")"
sbs_n=0
sbs_running=0
sbs_start() {
    sbs_n=$((sbs_n + 1))
    "$@" >"$SBS_DIR/$sbs_n.out" 2>&1 &
    sbs_running=$((sbs_running + 1))
    if [ "$sbs_running" -ge "$SBS_JOBS" ]; then
        wait -n
        sbs_running=$((sbs_running - 1))
    fi
}
sbs_finish() {
    local i rc=0
    wait
    for ((i = 1; i <= sbs_n; i++)); do
        cat "$SBS_DIR/$i.out"
        grep -q '^ok' "$SBS_DIR/$i.out" || rc=1
        grep -q '^FAIL' "$SBS_DIR/$i.out" && rc=1
    done
    [ "$sbs_n" -gt 0 ] || rc=1
    rm -rf "$SBS_DIR"
    return $rc
}

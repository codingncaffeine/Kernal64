#!/usr/bin/env bash
# Each kind of content in the real RetroArch, behind the wall (tools/wall.sh): run A plays N
# frames and RetroArch saves its state as it closes the content; run B loads that state at start
# and plays K frames; run C plays from the start to the same frame. B's last frame must equal
# C's, pixel for pixel (RetroArch's own screenshots). RetroArch applies the auto-load after the
# first frame has run (measured: a 1-frame run B shows the power-on frame), so B ends at frame
# N + K - 1. The control: K - 1 frames from the start without the state must look different,
# or the check could not see a state that was not restored.
#
#   tools/retroarch-states.sh [BUILD-FOLDER]     default: bin
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$(realpath "${1:-$ROOT/bin}")"
CORE="$BIN/kernal64_libretro.so"
OUT="$ROOT/scratch/ra-states"
rm -rf "$OUT"
mkdir -p "$OUT/media"
EXT="$ROOT/gitignore/external"

# ra NAME CONTENT FRAMES SHOT EXTRA-CONFIG-LINES: one RetroArch run behind the wall, with its own
# folders under $OUT/NAME (system ROMs copied in; states shared by the runs of one content).
ra() {
    local name="$1" content="$2" frames="$3" shot="$4" extra="$5" d="$OUT/$1"
    mkdir -p "$d/system" "$d/saves" "$d/states" "$d/shots"
    cp "$ROOT"/gitignore/bios/*.bin "$d/system/"
    cat >"$d/retroarch.cfg" <<CFG
audio_driver = "null"
audio_enable = "false"
input_driver = "null"
input_joypad_driver = "null"
video_driver = "gl"
video_fullscreen = "false"
menu_driver = "rgui"
pause_nonactive = "false"
config_save_on_exit = "false"
system_directory = "$d/system"
savefile_directory = "$d/saves"
savestate_directory = "$d/states"
screenshot_directory = "$d/shots"
savestate_file_compression = "false"
sort_savestates_by_content_enable = "false"
sort_savefiles_by_content_enable = "false"
notification_show_autoconfig = "false"
video_font_enable = "false"
log_verbosity = "true"
$extra
CFG
    "$ROOT/tools/wall.sh" retroarch --config "$d/retroarch.cfg" -L "$CORE" --max-frames="$frames" --max-frames-ss \
        --max-frames-ss-path="$shot" "$content" >"$d/$(basename "$shot" .png).log" 2>&1
}

same_picture() {
    python3 -I - "$1" "$2" <<'PY'
import sys
from PIL import Image
a, b = (Image.open(p).convert("RGB") for p in sys.argv[1:3])
sys.exit(0 if a.size == b.size and a.tobytes() == b.tobytes() else 1)
PY
}

# check NAME CONTENT N K
check() {
    local name="$1" content="$2" n="$3" k="$4" d="$OUT/$1"
    ra "$name" "$content" "$n" "$d/a.png" 'savestate_auto_save = "true"'
    local state
    state="$(find "$d/states" -name "*.state.auto" 2>/dev/null | head -1)"
    [ -s "$state" ] || { echo "FAIL $name: RetroArch saved no state"; grep -iE "error|state" "$d/a.log" | tail -3; return; }
    ra "$name" "$content" "$k" "$d/b.png" 'savestate_auto_load = "true"'
    grep -q "Auto-loading save state from .* succeeded" "$d/b.log" || { echo "FAIL $name: RetroArch did not load the state"; grep -i state "$d/b.log" | tail -3; return; }
    rm -f "$state"
    ra "$name" "$content" "$((n + k - 1))" "$d/c.png" ''
    ra "$name" "$content" "$((k - 1))" "$d/control.png" ''   # the control: no state
    [ -s "$d/b.png" ] && [ -s "$d/c.png" ] && [ -s "$d/control.png" ] || { echo "FAIL $name: a screenshot is missing"; return; }
    if same_picture "$d/control.png" "$d/c.png"; then echo "FAIL $name: blind, $((k - 1)) frames without the state look like $((n + k - 1))"
    elif same_picture "$d/b.png" "$d/c.png"; then echo "ok   $name: restored after $n frames, then the same frame as $((n + k - 1)) from the start"
    else echo "FAIL $name: the restored run's frame differs from the straight run's"; fi
}

# The content. A program and a cartridge whose pictures never settle: the program prints the
# jiffy clock's low byte and steps the border color, the cartridge steps the border color once
# a frame (at raster line $80).
M="$OUT/media"
printf '\x01\x08\x0b\x08\x0a\x00\x9e2061\x00\x00\x00\xa5\xa2\x8d\x00\x04\xee\x20\xd0\x4c\x0d\x08' >"$M/clock.prg"
{ printf '\x09\x80\x09\x80\xc3\xc2\xcd\x38\x30\x78\xad\x12\xd0\xc9\x80\xd0\xf9\xee\x20\xd0\xad\x12\xd0\xc9\x80\xf0\xf9\x4c\x0a\x80'
  head -c $((8192 - 30)) /dev/zero; } >"$M/border.bin"
python3 -I "$ROOT/tools/mkcrt.py" -o "$M/border.crt" --type 0 --exrom 0 --game 1 "$M/border.bin@0:8000"
"$BIN/k64run" "$ROOT/gitignore/bios" --blank-tape --record --cycles 60000000 --type-at 3000000 $'10 ?4242\r' \
    --type-at 3300000 'SAVE"T"'$'\r' --save-tape "$M/kernal.tap" >"$M/tape.log" 2>&1
python3 -I "$ROOT/tools/mknib.py" "$EXT/testprogs/drive/scanner/scan35.g64" "$M/scan35.nib" 7 >/dev/null
printf '%s\n' "$EXT/testprogs/vdrive/test.d64" "$EXT/testprogs/drive/scanner/scan35.d64" >"$M/two.m3u"

check prg "$M/clock.prg" 300 100
check crt "$M/border.crt" 300 100
check d64 "$EXT/testprogs/vdrive/test.d64" 600 100
check g64 "$EXT/testprogs/drive/scanner/scan35.g64" 600 100
check nib "$M/scan35.nib" 600 100
check m3u "$M/two.m3u" 600 100
check tap "$M/kernal.tap" 600 100

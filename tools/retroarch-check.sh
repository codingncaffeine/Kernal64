#!/usr/bin/env bash
# Loads the built core in the real RetroArch, behind the wall, runs a number of
# frames and saves RetroArch's own screenshot of the last one.
#
#   tools/retroarch-check.sh [FRAMES] [OUT.png] [CONTENT]
#
# Sound and input drivers are null; nothing reaches the desktop (see tools/wall.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FRAMES="${1:-120}"
OUT="$(realpath -m "${2:-$ROOT/build-release/retroarch-check.png}")"
CONTENT="${3:-}"
CORE="${K64_CORE:-$ROOT/bin/kernal64_libretro.so}"
[ -f "$CORE" ] || { echo "no core at $CORE; run tools/build-linux.sh" >&2; exit 2; }
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"

# The configuration is written inside the wall's private home by the inner command.
inner='
  cfg="$K64_TEST_TMP/retroarch.cfg"
  cat > "$cfg" <<EOF
audio_driver = "null"
audio_enable = "false"
input_driver = "null"
input_joypad_driver = "null"
video_driver = "gl"
video_fullscreen = "false"
menu_driver = "rgui"
pause_nonactive = "false"
config_save_on_exit = "false"
system_directory = "$K64_TEST_TMP/system"
savefile_directory = "$K64_TEST_TMP/saves"
savestate_directory = "$K64_TEST_TMP/states"
screenshot_directory = "$K64_TEST_TMP"
notification_show_autoconfig = "false"
log_verbosity = "true"
libretro_log_level = "1"
video_font_enable = "false"
EOF
  mkdir -p "$K64_TEST_TMP/system" "$K64_TEST_TMP/saves" "$K64_TEST_TMP/states"
  retroarch --config "$cfg" -L "$1" --max-frames="$2" --max-frames-ss --max-frames-ss-path="$3" ${4:+"$4"} \
      >"$K64_TEST_TMP/retroarch.log" 2>&1; rc=$?
  grep -E "Kernal64|\[ERROR\]" "$K64_TEST_TMP/retroarch.log" | head -20
  exit $rc
'
"$ROOT/tools/wall.sh" bash -c "$inner" inner "$CORE" "$FRAMES" "$OUT" "$CONTENT"
if [ ! -s "$OUT" ]; then echo "retroarch-check: no screenshot was written" >&2; exit 1; fi
echo "retroarch-check: screenshot $OUT ($(file -b "$OUT" | cut -d, -f2 | xargs))"

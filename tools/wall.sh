#!/usr/bin/env bash
# Runs a command the way every test run of a frontend (RetroArch, Emutastic) must
# run: behind a wall, so it cannot reach the desktop, the sound devices, the input
# devices or the network of the machine it is started on.
#
#   tools/wall.sh COMMAND [ARGS...]
#   tools/wall.sh --check     the control: proves each point below from inside
#
# Inside the wall (bubblewrap):
#   - no sound is possible: no /dev/snd, and the PulseAudio/PipeWire socket folder is empty
#   - none of the machine's devices can be opened: no controller, keyboard or other
#     input device, no USB device, no camera, no graphics card (OpenGL runs in software,
#     through Mesa only: a vendor GL library would look for its device and crash)
#   - no network but loopback
#   - own process numbering: no desktop process exists inside, and all ends with the wall
#   - the desktop's window servers cannot be reached; windows open on a virtual X server
#   - a private home, private XDG folders and a private /tmp; never the real profile
#   - a session bus of its own that can start nothing (no portal, no keyring)
#
#   K64_TEST_HOME=DIR      keep the private folders in DIR (default: temporary, removed)
#   K64_TEST_SCREEN=WxHxD  the virtual screen (default 1280x1024x24)
set -euo pipefail
[ $# -ge 1 ] || { sed -n '2,7p' "$0"; exit 2; }
RT="/run/user/$(id -u)"
HERE="$(cd "$(dirname "$0")" && pwd)"

# A mistake in sizing a test run should cost the run, never the desktop's programs.
echo 1000 > /proc/self/oom_score_adj 2>/dev/null || true

# /dev inside holds only what every program needs.
BWRAP=(--dev-bind / / --tmpfs /dev --tmpfs /dev/shm --unshare-user --unshare-pid --unshare-net --proc /proc --die-with-parent
       --symlink /proc/self/fd /dev/fd --symlink /proc/self/fd/0 /dev/stdin --symlink /proc/self/fd/1 /dev/stdout
       --symlink /proc/self/fd/2 /dev/stderr)
for node in null zero full random urandom tty; do
    [ -e "/dev/$node" ] && BWRAP+=(--dev-bind "/dev/$node" "/dev/$node")
done
[ -d "$RT" ] && BWRAP+=(--tmpfs "$RT")
[ -d /tmp/.X11-unix ] && BWRAP+=(--perms 1777 --tmpfs /tmp/.X11-unix)
command -v bwrap >/dev/null 2>&1 && bwrap "${BWRAP[@]}" true 2>/dev/null || {
    echo "wall: bubblewrap cannot run here; refusing to run a frontend without the wall" >&2; exit 3; }

devices_here='ls -d /dev/input/* /dev/hidraw* /dev/uinput /dev/uhid /dev/bus/usb/*/* /dev/video* /dev/dri/* /dev/snd/* 2>/dev/null; true'

if [ "$1" = "--check" ]; then
    inside() { bwrap "${BWRAP[@]}" -- "$@"; }
    out="$(inside sh -c "$devices_here"'; for s in "'"$RT"'"/pulse "'"$RT"'"/pipewire-0 "'"$RT"'"/bus; do [ -e "$s" ] && echo "$s"; done; true')"
    if [ -n "$out" ]; then echo "wall: CONTROL FAILED, reachable inside the wall:"; echo "$out" | head -5; exit 1; fi
    echo "wall: control ok (devices: $(sh -c "$devices_here" | wc -l) sound, input, USB, camera and graphics devices outside, none inside)"
    links_of='awk "NR > 2 { sub(/:.*/, \"\"); gsub(/ /, \"\"); print }" /proc/net/dev | sort | tr "\n" " "'
    links="$(inside sh -c "$links_of")"
    if [ "$links" != "lo " ]; then echo "wall: CONTROL FAILED, network links inside the wall: $links"; exit 1; fi
    echo "wall: control ok (network: only loopback inside, $(sh -c "$links_of" | wc -w) links outside)"
    seen="$(inside sh -c 'ls -d /proc/[0-9]* | wc -l')"
    if [ "$seen" -gt 8 ]; then echo "wall: CONTROL FAILED, $seen processes are visible inside the wall"; exit 1; fi
    echo "wall: control ok (processes: $seen visible inside, $(ls -d /proc/[0-9]* | wc -l) outside)"
    servers="$(inside sh -c 'ls -A /tmp/.X11-unix 2>/dev/null; true')"
    if [ -n "$servers" ]; then echo "wall: CONTROL FAILED, window server sockets inside the wall: $servers"; exit 1; fi
    echo "wall: control ok (window servers: none reachable inside, $(ls -A /tmp/.X11-unix 2>/dev/null | wc -l) outside)"
    exit 0
fi

if [ -n "${K64_TEST_HOME:-}" ]; then
    TMP="$(realpath -m "$K64_TEST_HOME")"; mkdir -p "$TMP"
else
    TMP="$(mktemp -d "${TMPDIR:-/tmp}/k64-wall.XXXXXX")"
    trap 'rm -rf "$TMP"' EXIT
fi
mkdir -p "$TMP/home" "$TMP/config" "$TMP/data" "$TMP/state" "$TMP/cache" "$TMP/rt"
chmod 700 "$TMP/rt"
unset WAYLAND_DISPLAY WAYLAND_SOCKET DISPLAY DBUS_SESSION_BUS_ADDRESS
ENVS=(HOME="$TMP/home" XDG_CONFIG_HOME="$TMP/config" XDG_DATA_HOME="$TMP/data" XDG_STATE_HOME="$TMP/state"
      XDG_CACHE_HOME="$TMP/cache" XDG_RUNTIME_DIR="$TMP/rt" XDG_SESSION_TYPE=x11
      PULSE_SERVER=unix:/nonexistent/k64-wall PIPEWIRE_REMOTE=/nonexistent/k64-wall
      SDL_AUDIODRIVER=dummy SDL_AUDIO_DRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 NO_AT_BRIDGE=1
      __GLX_VENDOR_LIBRARY_NAME=mesa __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
      K64_TEST_BUS="$HERE/isolated-bus.conf" K64_TEST_TMP="$TMP" K64_TEST_SCREEN="${K64_TEST_SCREEN:-1280x1024x24}")

# The X server picks a free display itself; it is ours ($!) and stopped by us.
inner='
  rm -f "$K64_TEST_TMP/display"
  Xvfb -displayfd 3 -screen 0 "$K64_TEST_SCREEN" -nolisten tcp 3>"$K64_TEST_TMP/display" 2>"$K64_TEST_TMP/xvfb.err" &
  server=$!
  for _ in $(seq 200); do [ -s "$K64_TEST_TMP/display" ] && break; kill -0 $server 2>/dev/null || break; sleep 0.05; done
  if [ ! -s "$K64_TEST_TMP/display" ]; then
    echo "wall: the virtual X server did not start: $(tail -2 "$K64_TEST_TMP/xvfb.err" | tr "\n" " ")" >&2
    kill $server 2>/dev/null; exit 4
  fi
  export DISPLAY=":$(cat "$K64_TEST_TMP/display")"
  dbus-run-session --config-file="$K64_TEST_BUS" -- "$@"; rc=$?
  kill $server 2>/dev/null; wait $server 2>/dev/null
  exit $rc
'
bwrap "${BWRAP[@]}" --perms 1777 --tmpfs /tmp --bind "$TMP" "$TMP" -- env "${ENVS[@]}" bash -c "$inner" wall "$@"

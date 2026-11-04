#!/usr/bin/env bash
# Runs a command inside a throwaway headless Wayland desktop with screen
# sharing: sway (headless, software rendering) + PipeWire + WirePlumber +
# xdg-desktop-portal with the wlroots backend, all on a private D-Bus session
# bus. Used by CI and the cloud to test the PipeWire capturer without a GPU,
# a monitor or a screen-sharing dialog (the portal is set to share
# HEADLESS-1 without asking).
#
#   tools/wayland_headless.sh ./build/bin/rdaccel_platform_tests --gtest_filter='PipeWire*'
#
# Inside the command: WAYLAND_DISPLAY, SWAYSOCK and XDG_SESSION_TYPE=wayland
# are set, DISPLAY is not; `swaymsg output HEADLESS-1 bg '#rrggbb' solid_color`
# changes what is on the screen.
#
# Ubuntu 24.04 packages: sway pipewire wireplumber xdg-desktop-portal
# xdg-desktop-portal-wlr dbus-daemon (Linux only; it makes no sound: there is
# no audio device in a container, and nothing here plays audio).
set -euo pipefail

if [ "$#" -eq 0 ]; then
  echo "usage: $0 command [args...]" >&2
  exit 2
fi

if [ "${RDA_WL_INNER:-}" != 1 ]; then
  work=$(mktemp -d "${TMPDIR:-/tmp}/rdaccel-wl.XXXXXX")
  mkdir -m 700 "$work/run"
  mkdir -p "$work/config/xdg-desktop-portal-wlr"
  cat > "$work/sway.conf" <<'EOF'
output HEADLESS-1 resolution 1280x720 position 0 0 bg #202020 solid_color
default_border none
EOF
  cat > "$work/config/xdg-desktop-portal-wlr/config" <<'EOF'
[screencast]
output_name=HEADLESS-1
chooser_type=none
max_fps=60
EOF
  # No user configuration or services from the host may leak in.
  status=0
  env -i PATH="$PATH" HOME="$work" TMPDIR="${TMPDIR:-/tmp}" LANG=C.UTF-8 \
    XDG_RUNTIME_DIR="$work/run" XDG_CONFIG_HOME="$work/config" \
    XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=sway \
    RDA_WL_INNER=1 RDA_WL_WORK="$work" \
    ${ASAN_OPTIONS:+ASAN_OPTIONS="$ASAN_OPTIONS"} \
    ${LSAN_OPTIONS:+LSAN_OPTIONS="$LSAN_OPTIONS"} \
    ${RDACCEL_DEBUG:+RDACCEL_DEBUG="$RDACCEL_DEBUG"} \
    ${RDA_WL_XDPW_LOG:+RDA_WL_XDPW_LOG="$RDA_WL_XDPW_LOG"} \
    ${RDA_WL_XDPW:+RDA_WL_XDPW="$RDA_WL_XDPW"} \
    dbus-run-session -- "$0" "$@" || status=$?
  if [ "$status" -ne 0 ] && [ -n "${RDA_WL_KEEP_LOGS:-}" ]; then
    for f in "$work"/*.log; do echo "==> $f"; tail -n 40 "$f"; done
  fi
  # xdg-document-portal may leave a FUSE mount behind in the runtime dir.
  fusermount3 -u "$work/run/doc" 2>/dev/null || fusermount -u "$work/run/doc" 2>/dev/null || true
  rm -rf "$work" 2>/dev/null || true
  exit "$status"
fi

work="$RDA_WL_WORK"
pids=()
cleanup() {
  for ((i = ${#pids[@]} - 1; i >= 0; i--)); do kill "${pids[i]}" 2>/dev/null || true; done
  wait 2>/dev/null || true
}
trap cleanup EXIT

wait_for() {  # wait_for <what> <test command...>
  local what="$1"
  shift
  for _ in $(seq 100); do
    if "$@" >/dev/null 2>&1; then return 0; fi
    sleep 0.1
  done
  echo "wayland_headless: $what did not come up" >&2
  for f in "$work"/*.log; do echo "==> $f" >&2; tail -n 20 "$f" >&2; done
  exit 1
}

WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
  sway -c "$work/sway.conf" > "$work/sway.log" 2>&1 &
pids+=($!)
wait_for sway sh -c 'ls "$XDG_RUNTIME_DIR"/wayland-* "$XDG_RUNTIME_DIR"/sway-ipc.*.sock'
WAYLAND_DISPLAY=$(basename "$(ls "$XDG_RUNTIME_DIR"/wayland-? | head -n1)")
SWAYSOCK=$(ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock | head -n1)
export WAYLAND_DISPLAY SWAYSOCK
unset DISPLAY
dbus-update-activation-environment WAYLAND_DISPLAY SWAYSOCK XDG_CURRENT_DESKTOP XDG_SESSION_TYPE \
  XDG_CONFIG_HOME 2>/dev/null || true

pipewire > "$work/pipewire.log" 2>&1 &
pids+=($!)
wait_for pipewire test -S "$XDG_RUNTIME_DIR/pipewire-0"
wireplumber > "$work/wireplumber.log" 2>&1 &
pids+=($!)

"${RDA_WL_XDPW:-/usr/libexec/xdg-desktop-portal-wlr}" -l ${RDA_WL_XDPW_LOG:-INFO} > "$work/xdpw.log" 2>&1 &
pids+=($!)
# Asks the bus without activating the service (that would start the
# packaged binary instead of $RDA_WL_XDPW).
wait_for xdg-desktop-portal-wlr sh -c 'gdbus call --session --dest org.freedesktop.DBus \
  --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.NameHasOwner \
  org.freedesktop.impl.portal.desktop.wlr | grep -q true'
/usr/libexec/xdg-desktop-portal -r > "$work/portal.log" 2>&1 &
pids+=($!)
wait_for xdg-desktop-portal sh -c 'gdbus introspect --session --dest org.freedesktop.portal.Desktop \
  --object-path /org/freedesktop/portal/desktop | grep -q org.freedesktop.portal.ScreenCast'

status=0
"$@" || status=$?
if [ "$status" -ne 0 ]; then
  for f in "$work"/*.log; do echo "==> $f" >&2; tail -n 30 "$f" >&2; done
fi
exit "$status"

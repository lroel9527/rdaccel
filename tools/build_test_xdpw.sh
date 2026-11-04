#!/usr/bin/env bash
# Builds xdg-desktop-portal-wlr 0.7.1 (Ubuntu 24.04's version) with one change
# for headless tests: it then shares a screen that offers only shared-memory
# buffers, as sway's software renderer does (the packaged one insists on
# DMA-BUF, i.e. a GPU). Test infrastructure only; the library does not use it.
#
#   tools/build_test_xdpw.sh OUTDIR     -> OUTDIR/xdg-desktop-portal-wlr
#
# Needs deb-src entries, meson, ninja and the build dependencies
# (libinih-dev libgbm-dev libdrm-dev libsystemd-dev libpipewire-0.3-dev
# libwayland-dev wayland-protocols).
set -euo pipefail
out=$(realpath -m "$1")
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
apt-get source -qq --download-only xdg-desktop-portal-wlr=0.7.1-1build2 >/dev/null 2>&1 ||
  apt-get source -qq --download-only xdg-desktop-portal-wlr >/dev/null
tar -xzf xdg-desktop-portal-wlr_0.7.1.orig.tar.gz
cd xdg-desktop-portal-wlr-0.7.1
patch -p1 < "$here/xdpw-shm-only.patch"
meson setup build -Dman-pages=disabled -Dsd-bus-provider=libsystemd >/dev/null
ninja -C build >/dev/null
mkdir -p "$out"
cp build/xdg-desktop-portal-wlr "$out/"
echo "$out/xdg-desktop-portal-wlr"

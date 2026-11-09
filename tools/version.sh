#!/usr/bin/env bash
# Prints the full version from CMakeLists.txt: 1.0.0, or 1.0.0-rc1 for a
# pre-release (RDACCEL_PRERELEASE). Releases are tagged v<this> (docs/releasing.md).
set -euo pipefail
cd "$(dirname "$0")/.."
v=$(sed -n 's/^project(rdaccel VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
pre=$(sed -n 's/^set(RDACCEL_PRERELEASE "\(.*\)")/\1/p' CMakeLists.txt)
echo "$v${pre:+-$pre}"

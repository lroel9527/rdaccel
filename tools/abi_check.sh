#!/usr/bin/env bash
# Compares the C ABI of librdaccel.so at a base revision with the working
# tree, using libabigail's abidiff. Only what include/rdaccel/ declares
# counts (the exported rda_* functions and the types they use).
#
#   tools/abi_check.sh <base-ref> [work-dir]
#
# Within one 0.x minor version (or one major from 1.0 on) an incompatible
# change fails the check; across a version bump it is only reported.
# Appending fields to a struct that starts with struct_size is the
# library's way of growing; abidiff reports it as a change, not a failure.
set -euo pipefail

base_ref=${1:?usage: tools/abi_check.sh <base-ref> [work-dir]}
work=${2:-$(mktemp -d)}
root=$(git rev-parse --show-toplevel)
mkdir -p "$work"

build() {  # <src> <build-dir>
  cmake -S "$1" -B "$2" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_SHARED_LIBS=ON \
    -DRDACCEL_BUILD_TESTS=OFF -DRDACCEL_BUILD_EXAMPLES=OFF -DRDACCEL_INSTALL=OFF >/dev/null
  cmake --build "$2" -j --target rdaccel >/dev/null
}
version() {  # <src> -> MAJOR.MINOR (MAJOR alone from 1.0 on)
  local v
  v=$(sed -n 's/^project(rdaccel VERSION \([0-9.]*\).*/\1/p' "$1/CMakeLists.txt")
  if [[ $v == 0.* ]]; then echo "${v%.*}"; else echo "${v%%.*}"; fi
}

rm -rf "$work/base-src"
git -C "$root" worktree add --detach "$work/base-src" "$base_ref" >/dev/null 2>&1
trap 'git -C "$root" worktree remove --force "$work/base-src" >/dev/null 2>&1 || true' EXIT
build "$work/base-src" "$work/base"
build "$root" "$work/head"

old=$(find "$work/base" -name 'librdaccel.so.*.*.*' -type f | head -1)
new=$(find "$work/head" -name 'librdaccel.so.*.*.*' -type f | head -1)
base_v=$(version "$work/base-src")
head_v=$(version "$root")
echo "ABI: $base_ref ($base_v) -> working tree ($head_v)"

set +e
abidiff --headers-dir1 "$work/base-src/include/rdaccel" --headers-dir2 "$root/include/rdaccel" \
  --no-added-syms --fail-no-debug-info "$old" "$new" | tee "$work/abidiff.txt"
status=${PIPESTATUS[0]}
set -e
# abidiff exit bits: 1 error, 2 usage, 4 ABI change, 8 incompatible change.
if (( status & 3 )); then
  echo "abidiff failed to run (status $status)"
  exit 1
fi
if (( status & 8 )); then
  if [[ $base_v == "$head_v" ]]; then
    echo "Incompatible ABI change within version $head_v: bump the version (see docs/releasing.md)."
    exit 1
  fi
  echo "Incompatible ABI change, allowed by the version bump $base_v -> $head_v."
elif (( status & 4 )); then
  echo "Compatible ABI changes (see above)."
else
  echo "No ABI change."
fi

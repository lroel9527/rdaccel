#!/usr/bin/env bash
# Compares the working tree with an earlier version on the same test clips:
# every clip in CLIP_DIR through `rdaccel_clip eval`, once built from this
# tree and once from BASE (a tag or commit), in normal and in text mode.
#
#   tools/clip/compare.sh BASE [CLIP_DIR] [--modes="saver normal"]
#   e.g. tools/clip/compare.sh v1.9.5 clips
#
# BASE is checked out into a temporary worktree and built there (the library
# only, plus this tree's tools/clip, which works on any version since 1.6).
# The table goes to stdout, every line of eval output to CLIP_DIR/compare-*.txt.
# The clips stay where they are (docs/clips.md).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
BASE=${1:?usage: compare.sh BASE [CLIP_DIR] [--modes="saver normal"]}
DIR=${2:-$REPO/clips}
MODES="saver normal"
for a in "$@"; do [[ $a == --modes=* ]] && MODES=${a#--modes=}; done
shopt -s nullglob
CLIPS=("$DIR"/*.clip)
[[ ${#CLIPS[@]} -gt 0 ]] || { echo "no clips in $DIR" >&2; exit 1; }

echo "building this tree" >&2
cmake -S "$REPO" -B "$REPO/build" > /dev/null
cmake --build "$REPO/build" -j --target rdaccel_clip > "$REPO/build/compare-build.log" 2>&1 || { cat "$REPO/build/compare-build.log" >&2; exit 1; }
NEW=$REPO/build/bin/rdaccel_clip

WT=$(mktemp -d)
trap 'git -C "$REPO" worktree remove --force "$WT" >/dev/null 2>&1 || true' EXIT
echo "building $BASE" >&2
git -C "$REPO" worktree add -q --detach "$WT" "$BASE"
mkdir -p "$WT/tools/clip"
cp "$REPO"/tools/clip/clip.h "$REPO"/tools/clip/rdaccel_clip.cpp "$WT/tools/clip/"
if ! grep -q rdaccel_clip "$WT/CMakeLists.txt"; then
  cat >> "$WT/CMakeLists.txt" <<'CM'
add_executable(rdaccel_clip tools/clip/rdaccel_clip.cpp)
target_include_directories(rdaccel_clip PRIVATE tools/clip examples)
target_link_libraries(rdaccel_clip PRIVATE rdaccel_internal ZLIB::ZLIB)
CM
fi
cmake -S "$WT" -B "$WT/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRDACCEL_BUILD_TESTS=OFF -DRDACCEL_BUILD_EXAMPLES=OFF > /dev/null
cmake --build "$WT/build" -j --target rdaccel_clip > "$WT/build.log" 2>&1 || { cat "$WT/build.log" >&2; exit 1; }
OLD=$WT/build/bin/rdaccel_clip

OUTF=$DIR/compare-$(date +%Y%m%d-%H%M%S).txt
for c in "${CLIPS[@]}"; do
  name=$(basename "$c" .clip)
  for m in $MODES; do
    t=0; [[ $m == saver ]] && t=1
    for v in old new; do
      bin=$OLD; label="$name@$BASE"
      [[ $v == new ]] && { bin=$NEW; label="$name@new"; }
      echo "  $label $m" >&2
      "$bin" eval "$c" --text=$t --label="$label" >> "$OUTF" 2>&1 || echo "$label $m failed" >> "$OUTF"
    done
  done
done

python3 - "$OUTF" "$BASE" <<'PY'
import re, sys
path, base = sys.argv[1], sys.argv[2]
rows = {}
for l in open(path):
    m = re.match(r'(\S+)@(\S+)\s+(saver|normal).*sent\s+(\d+) kB.*exact\s+([\d.]+)%.*pauses: exact\s+([\d.]+)%', l)
    if m:
        rows[(m.group(1), 'new' if m.group(2) == 'new' else 'old', m.group(3))] = (int(m.group(4)), float(m.group(5)), float(m.group(6)))
print(f"{'clip':14s} {'mode':6s} {base + ' kB':>12s} {'new kB':>9s} {'change':>8s}   exact over time        exact at pauses")
for name, _, mode in sorted({(k[0], 0, k[2]) for k in rows}):
    a, b = rows.get((name, 'old', mode)), rows.get((name, 'new', mode))
    if not a or not b:
        print(f"{name:14s} {mode:6s} (missing)")
        continue
    ch = 100.0 * (b[0] - a[0]) / a[0] if a[0] else 0.0
    print(f"{name:14s} {mode:6s} {a[0]:12d} {b[0]:9d} {ch:+7.1f}%   {a[1]:5.1f}% -> {b[1]:5.1f}%      {a[2]:5.1f}% -> {b[2]:5.1f}%")
print(f"\n(all output: {path})")
PY

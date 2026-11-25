#!/usr/bin/env bash
# Checks that a change to the host leaves every decision it makes as it was
# (docs/clips.md): every clip goes through `rdaccel_clip decisions` (a fake
# encoder whose pictures depend only on their input, no viewer) in each of
# the settings below, once before the change (record) and again after it
# (compare). Equal logs: on every frame the host sent the same bytes and
# reported the same decisions.
#
#   tools/clip/check_decisions.sh record  [CLIPS_DIR] [OUT_DIR]   # writes OUT_DIR/<setting>/<clip>.txt
#   tools/clip/check_decisions.sh compare [CLIPS_DIR] [OUT_DIR]   # reruns, diffs against OUT_DIR
#
# CLIPS_DIR: every *.clip in it (default: clips). OUT_DIR: default
# CLIPS_DIR/decisions. rdaccel_clip is built first (build/); RDACCEL_CLIP=PATH
# runs that binary instead. compare prints the first differing line of each
# log that differs and exits 1 (a run that fails counts too); the new logs are
# then kept beside OUT_DIR. The logs hold only sizes and hashes, but they stay
# under clips/ with the clips. One run after another.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
MODE=${1:-}
[[ $MODE == record || $MODE == compare ]] || { echo "usage: check_decisions.sh record|compare [CLIPS_DIR] [OUT_DIR]" >&2; exit 2; }
DIR=${2:-$REPO/clips}
OUT=${3:-$DIR/decisions}
# A name, then the flags of `rdaccel_clip decisions`.
SETTINGS=(
  "normal          --text=0 --bitrate=8000000 --fps=60"
  "saver8m         --text=1 --bitrate=8000000 --fps=30"
  "saver1m         --text=1 --bitrate=1000000 --fps=30"
  "saver300k       --text=1 --bitrate=300000 --fps=30"
  "normal-perturb  --text=0 --bitrate=8000000 --fps=60 --perturb=1"
  "saver1m-perturb --text=1 --bitrate=1000000 --fps=30 --perturb=1"
)

shopt -s nullglob
CLIPS=("$DIR"/*.clip)
[[ ${#CLIPS[@]} -gt 0 ]] || { echo "no clips in $DIR" >&2; exit 1; }

if [[ -n ${RDACCEL_CLIP:-} ]]; then
  BIN=$RDACCEL_CLIP
else
  # Always the tree as it is now: a stale binary would compare the old code
  # with itself.
  if [[ ! -f $REPO/build/CMakeCache.txt ]]; then
    cmake -S "$REPO" -B "$REPO/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRDACCEL_BUILD_TESTS=OFF \
      -DRDACCEL_BUILD_EXAMPLES=OFF > /dev/null
  fi
  cmake --build "$REPO/build" -j --target rdaccel_clip > "$REPO/build/check-decisions-build.log" 2>&1 ||
    { cat "$REPO/build/check-decisions-build.log" >&2; exit 1; }
  BIN=$REPO/build/bin/rdaccel_clip
fi

if [[ $MODE == record ]]; then
  DEST=$OUT
else
  [[ -d $OUT ]] || { echo "nothing recorded in $OUT" >&2; exit 1; }
  # Beside OUT_DIR, so under clips/ as well.
  DEST=$(mktemp -d "${OUT%/}.compare.XXXXXX")
  trap 'rm -rf "$DEST"' EXIT
fi

# The first line where two logs differ (streamed: a log may have many
# thousands of lines, and after a change most of them differ).
first_diff() {
  awk -v a="$1" -v b="$2" 'BEGIN {
    for (n = 1; ; n++) {
      ra = (getline la < a) > 0
      rb = (getline lb < b) > 0
      if (!ra && !rb) exit
      if (ra && rb && (la "") == (lb "")) continue
      printf "    line %d\n    recorded: %s\n    now:      %s\n", n, ra ? la : "(end of the log)", rb ? lb : "(end of the log)"
      exit
    }
  }'
}

bad=0
start=$SECONDS
for clip in "${CLIPS[@]}"; do
  name=$(basename "$clip" .clip)
  for s in "${SETTINGS[@]}"; do
    read -r setting rest <<< "$s"
    read -r -a flags <<< "$rest"
    mkdir -p "$DEST/$setting"
    log=$DEST/$setting/$name.txt
    t0=$SECONDS
    if ! summary=$("$BIN" decisions "$clip" "${flags[@]}" --out="$log"); then
      echo "$setting/$name: FAILED"
      rm -f "$log"
      bad=1
      continue
    fi
    took="$((SECONDS - t0)) s, $summary"
    if [[ $MODE == record ]]; then
      echo "$setting/$name: $took"
    elif [[ ! -f $OUT/$setting/$name.txt ]]; then
      echo "$setting/$name: nothing recorded"
      bad=1
    elif cmp -s "$OUT/$setting/$name.txt" "$log"; then
      echo "$setting/$name: same ($took)"
    else
      echo "$setting/$name: DIFFERS"
      first_diff "$OUT/$setting/$name.txt" "$log"
      bad=1
    fi
  done
done
echo "$((SECONDS - start)) s in all"
if [[ $MODE == compare && $bad != 0 ]]; then
  trap - EXIT
  echo "the new logs are in $DEST"
fi
exit $bad

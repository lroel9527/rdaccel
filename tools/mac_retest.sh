#!/usr/bin/env bash
# One-click re-test of the M2 features on a real Mac (see docs/mac_retest.md).
#
#   tools/mac_retest.sh [--skip-build] [--skip-screen] [--background=SCREEN.ppm]
#
# SILENT: nothing here plays or records sound. rdaccel has no audio code at
# all (ScreenCaptureKit is configured with capturesAudio = NO), and the
# script never calls say, afplay, osascript, or anything else that makes
# sound. It does not touch the volume either.
#
# --skip-screen leaves out the steps that capture the real screen (they need
# the Screen Recording permission for the terminal; macOS asks once, which is
# a dialog, not a sound).
#
# Everything goes to retest-<date>/ in the repository: one log per step and
# summary.txt with the numbers to compare against docs/mac_retest.md.
# Runs on Linux too with RDACCEL_RETEST_ANY_OS=1 (for checking the script).

set -u
cd "$(dirname "$0")/.."

skip_build=0
skip_screen=0
background=""
for a in "$@"; do
  case "$a" in
    --skip-build) skip_build=1 ;;
    --skip-screen) skip_screen=1 ;;
    --background=*) background="${a#--background=}" ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

if [[ "$(uname -s)" != "Darwin" && "${RDACCEL_RETEST_ANY_OS:-0}" != "1" ]]; then
  echo "This script is for macOS (set RDACCEL_RETEST_ANY_OS=1 to run it elsewhere)." >&2
  exit 2
fi

out="retest-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
summary="$out/summary.txt"
bin=build-retest/bin
failures=0

{
  echo "rdaccel Mac re-test, $(date)"
  echo "commit $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "$(uname -a)"
  if command -v sw_vers >/dev/null; then sw_vers | tr '\n' ' '; echo; fi
  if command -v sysctl >/dev/null; then sysctl -n machdep.cpu.brand_string 2>/dev/null; fi
  echo
} > "$summary"

# run NAME CMD...: runs CMD with its output in $out/NAME.log; notes the result.
run() {
  local name="$1"; shift
  echo "== $name"
  if "$@" > "$out/$name.log" 2>&1; then
    echo "   ok"
    echo "[ok]   $name" >> "$summary"
  else
    echo "   FAILED (see $out/$name.log)"
    echo "[FAIL] $name (exit $?, see $name.log)" >> "$summary"
    failures=$((failures + 1))
  fi
}

# note NAME PATTERN: copies the matching lines of a step's log into the summary.
note() {
  grep -E "$2" "$out/$1.log" 2>/dev/null | grep -v 'Cannot load libcuda' | sed 's/^/         /' >> "$summary"
}
rows='^(scenario|idle|typing|scroll|drag|video|pause|pointer) |PSNR inside|cursor channel'

if [[ $skip_build == 0 ]]; then
  run configure cmake -S . -B build-retest -DCMAKE_BUILD_TYPE=RelWithDebInfo
  run build cmake --build build-retest -j
  if [[ ! -x $bin/rdaccel_tests ]]; then
    echo "build failed, stopping" | tee -a "$summary"
    exit 1
  fi
fi

# 1. Unit tests, then VideoToolbox + public API tests (hardware encoder).
run unit_tests "$bin/rdaccel_tests"
note unit_tests 'PASSED|FAILED'
run platform_tests "$bin/rdaccel_platform_tests"
note platform_tests 'PASSED|FAILED'

# 2. The real screen over UDP on this machine (serve + view on localhost).
serve_view() {  # NAME SERVE_ARGS... -- VIEW_ARGS...
  local name="$1"; shift
  local sargs=() vargs=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do sargs+=("$1"); shift; done
  shift
  vargs=("$@")
  "$bin/rdaccel_serve" "${sargs[@]}" > "$out/$name.serve.log" 2>&1 &
  local sp=$!
  sleep 1
  "$bin/rdaccel_view" 127.0.0.1 "${vargs[@]}" > "$out/$name.log" 2>&1
  local rc=$?
  wait "$sp"
  return $rc
}
if [[ $skip_screen == 0 ]]; then
  # Native size: cursor image at the screen's pixel size (Retina: 2x points).
  run screen_native serve_view screen_native --udp=7391 --source=screen --seconds=6 -- \
    --udp=7391 --seconds=5 --dump="$out/screen_native.ppm"
  note screen_native 'cursor image|fps'
  # Scaled (smaller than a Retina screen's pixels): the cursor image must
  # shrink to match. The picture is also the background for the scenes below.
  run screen_scaled serve_view screen_scaled --udp=7392 --source=screen --seconds=6 --size=1920x1200 -- \
    --udp=7392 --seconds=5 --dump="$out/screen_scaled.ppm"
  note screen_scaled 'cursor image|fps'
  # A keyframe the viewer asks for while the screen is static still comes
  # (from memory: ScreenCaptureKit sends no frames while nothing changes).
  echo "   (keep the screen and the mouse still for the next 8 seconds)"
  run screen_static_keyframe serve_view screen_static_keyframe --udp=7393 --source=screen --seconds=9 -- \
    --udp=7393 --seconds=8 --keyframe-at=5
  note screen_static_keyframe 'keyframe|frames after'
  if ! grep -qE 'frames after the keyframe request: [1-9]' "$out/screen_static_keyframe.log"; then
    echo "[FAIL] no frame after the keyframe request on a static screen" >> "$summary"
    failures=$((failures + 1))
  fi
fi

bg=()
if [[ -n "$background" ]]; then
  bg=(--background="$background")
elif [[ -s "$out/screen_scaled.ppm" ]]; then
  bg=(--background="$out/screen_scaled.ppm")
fi
echo "background: ${bg[*]:-built-in test pattern}" >> "$summary"

# 3. Scenes, on the screenshot from step 2 (or the built-in pattern with
#    --skip-screen / --background=FILE.ppm).
run bench_all "$bin/rdaccel_bench" ${bg[@]+"${bg[@]}"} --scenario=all --frames=600
note bench_all "$rows"
#    Video areas on and off: two VideoToolbox sessions at once in the first.
run bench_video_regions "$bin/rdaccel_bench" ${bg[@]+"${bg[@]}"} --scenario=video --frames=600 --regions=1
note bench_video_regions "$rows"
run bench_video_noregions "$bin/rdaccel_bench" ${bg[@]+"${bg[@]}"} --scenario=video --frames=600 --regions=0
note bench_video_noregions "$rows"
run bench_pointer "$bin/rdaccel_bench" ${bg[@]+"${bg[@]}"} --scenario=pointer --frames=600 --cursor=channel
note bench_pointer "$rows"

# 4. Simulated network (in one process): ABR changes the VideoToolbox
#    bitrate at run time; progressive first picture; cursor latency.
run netsim_drop "$bin/rdaccel_netsim" ${bg[@]+"${bg[@]}"} --seconds=15 --bandwidth=4000000 --drop-to=1000000 --drop-at=6
note netsim_drop 'Mbps'
run netsim_progressive_on "$bin/rdaccel_netsim" ${bg[@]+"${bg[@]}"} --scenario=idle --seconds=10 --warmup=6 --bandwidth=1500000 --progressive=1
note netsim_progressive_on 'first picture|start:'
run netsim_progressive_off "$bin/rdaccel_netsim" ${bg[@]+"${bg[@]}"} --scenario=idle --seconds=10 --warmup=6 --bandwidth=1500000 --progressive=0
note netsim_progressive_off 'first picture|start:'
run netsim_cursor "$bin/rdaccel_netsim" ${bg[@]+"${bg[@]}"} --seconds=10 --bandwidth=1500000 --cursor=1
note netsim_cursor 'Mbps|cursor:'

echo >> "$summary"
echo "failures: $failures" >> "$summary"
echo
cat "$summary"
echo
echo "Logs: $out/ (send summary.txt, or the whole folder)"
exit $((failures > 0))

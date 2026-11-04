#!/usr/bin/env bash
# Runs support_host and support_viewer against each other on this machine
# (Linux: under an X server, e.g. xvfb-run). Checks that a paired viewer
# gets pictures, the cursor and the host's session info, that the host
# serves a second viewer after the first, and that a wrong pairing code is
# refused.
#
#   examples/integration/smoke.sh <dir with support_host and support_viewer>
set -euo pipefail
bin=${1:?usage: smoke.sh <bin dir>}
work=$(mktemp -d)
host_pid=
cleanup() {
  status=$?
  if (( status != 0 )); then
    for f in "$work"/*.log; do echo "== $f"; cat "$f"; done
  fi
  [[ -n $host_pid ]] && kill $host_pid 2>/dev/null || true
  rm -rf "$work"
}
trap cleanup EXIT
od -An -tx1 -N32 /dev/urandom | tr -d ' \n' > "$work/key"
port=$((20000 + RANDOM % 20000))

"$bin/support_host" --key-file="$work/key" --port=$port --pair-code=4711 --sessions=3 \
  > "$work/host.log" 2>&1 &
host_pid=$!

v() { "$bin/support_viewer" --key-file="$work/key" --port=$port "$@"; }
v --pair-code=4711 --seconds=2 --snapshot="$work/snap.ppm" | tee "$work/v1.log"
grep -q 'session info "name=support_host' "$work/v1.log"
grep -q 'cursor updates [1-9]' "$work/v1.log"
test -s "$work/snap.ppm"
# Retries cover the moment the host takes to set up for the next viewer; a
# refusal ends the viewer at once.
if v --pair-code=wrong --seconds=1 --retries=20 > "$work/v2.log" 2>&1; then
  echo "a wrong pairing code was let in"; cat "$work/v2.log"; exit 1
fi
grep -q 'refused by the host: RDA_ERR_AUTH' "$work/v2.log"
if grep -qE '^(connected|stream )' "$work/v2.log"; then
  echo "the refused viewer saw the session"; cat "$work/v2.log"; exit 1
fi
v --pair-code=4711 --seconds=1 --retries=20 --no-input | tee "$work/v3.log"
grep -q 'session info "name=support_host' "$work/v3.log"
wait $host_pid
cat "$work/host.log"
grep -q 'clipboard from viewer: 22 bytes' "$work/host.log"
grep -q 'last clipboard text: "pasted from the viewer"' "$work/host.log"
echo "integration smoke test passed"

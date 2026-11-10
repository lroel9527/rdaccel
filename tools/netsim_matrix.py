#!/usr/bin/env python3
"""Weak-network benchmark: runs rdaccel_netsim over a set of network conditions
and scenarios several times and prints medians, so a change can be judged on
more than one noisy run (single runs of the same setup differ by 2x in p95).

  tools/netsim_matrix.py BUILD/bin/rdaccel_netsim [--reps=3] [--seconds=12]
                         [--conditions=hl2,hl1,slow,wifi] [--scenarios=typing,drag,scroll,video]
                         [--save=results.json] [--compare=before.json]

Conditions (one way delay, jitter, loss, bandwidth):
  hl2   100 ms, 40 ms, 2%, 4 Mbps   high latency (a 200 ms round trip)
  hl1   100 ms, 40 ms, 1%, 4 Mbps
  slow  100 ms, 40 ms, 1%, 1 Mbps   high latency on a slow link
  wifi   15 ms, 60 ms, 1%, 4 Mbps   jittery Wi-Fi (packets reordered)

--save writes the runs; --compare prints them next to an earlier --save.
No CI runs this: run it locally before and after a transport or rate change.
"""
import argparse
import json
import re
import statistics
import subprocess
import sys

CONDITIONS = {
    "hl2": dict(delay=100, jitter=40, loss=0.02, bandwidth=4_000_000),
    "hl1": dict(delay=100, jitter=40, loss=0.01, bandwidth=4_000_000),
    "slow": dict(delay=100, jitter=40, loss=0.01, bandwidth=1_000_000),
    "wifi": dict(delay=15, jitter=60, loss=0.01, bandwidth=4_000_000),
}

LINE = re.compile(
    r"steady: +(?P<kbps>\d+) kbps, +(?P<shown>\d+)% frames shown \((?P<skipped>\d+) skipped\), "
    r"latency p50 +(?P<p50>\d+) p95 +(?P<p95>\d+) max +(?P<max>\d+) ms, stall +(?P<stall>\d+) ms, "
    r"PSNR +(?P<psnr>[\d.]+) dB, target +(?P<target>\d+) kbps, estimate +(?P<estimate>\d+) kbps")

FIELDS = [("p95", "p95 ms", "{:.0f}"), ("max", "max ms", "{:.0f}"), ("psnr", "PSNR", "{:.1f}"),
          ("skipped", "skipped", "{:.0f}"), ("estimate", "estimate", "{:.0f}"), ("target", "target", "{:.0f}")]


def run(netsim, cond, scenario, seconds):
    c = CONDITIONS[cond]
    args = [netsim, f"--scenario={scenario}", f"--seconds={seconds}", f"--warmup={max(2, seconds // 4)}",
            f"--bandwidth={c['bandwidth']}", f"--delay={c['delay']}", f"--jitter={c['jitter']}", f"--loss={c['loss']}"]
    out = subprocess.run(args, capture_output=True, text=True, timeout=seconds * 10).stdout
    m = LINE.search(out)
    if not m:
        sys.exit(f"no result line from: {' '.join(args)}\n{out}")
    return {k: float(v) for k, v in m.groupdict().items()}


def medians(runs):
    return {k: statistics.median(r[k] for r in runs) for k in runs[0]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("netsim")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--seconds", type=int, default=12)
    ap.add_argument("--conditions", default="hl2,hl1,slow,wifi")
    ap.add_argument("--scenarios", default="typing,drag,scroll,video")
    ap.add_argument("--save")
    ap.add_argument("--compare")
    a = ap.parse_args()
    results = {}
    for cond in a.conditions.split(","):
        for sc in a.scenarios.split(","):
            results[f"{cond}/{sc}"] = [run(a.netsim, cond, sc, a.seconds) for _ in range(a.reps)]
            print(f"  {cond}/{sc} done", file=sys.stderr)
    if a.save:
        with open(a.save, "w") as f:
            json.dump(results, f)
    before = json.load(open(a.compare)) if a.compare else None
    head = f"{'case':14}" + "".join(f"{label:>18}" for _, label, _ in FIELDS)
    print(head + ("   (before -> now, medians)" if before else "   (medians)"))
    for key, runs in results.items():
        now = medians(runs)
        row = f"{key:14}"
        for k, _, fmt in FIELDS:
            if before and key in before:
                row += f"{fmt.format(medians(before[key])[k]) + ' -> ' + fmt.format(now[k]):>18}"
            else:
                row += f"{fmt.format(now[k]):>18}"
        print(row)


if __name__ == "__main__":
    main()

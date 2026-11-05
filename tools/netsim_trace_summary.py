#!/usr/bin/env python3
"""Summarizes a `rdaccel_netsim --trace=FILE` CSV in half-second steps:
kbps sent (video / lossless), frames skipped, target, estimate, RTT, frames
shown and their worst latency, and the largest frame."""
import csv
import sys
from collections import defaultdict

rows = list(csv.DictReader(open(sys.argv[1])))
steps = defaultdict(list)
for r in rows:
    steps[int(float(r["t_ms"]) // 500)].append(r)
for k in sorted(steps):
    rs = steps[k]
    kbps = lambda col: sum(int(r[col]) for r in rs) * 8 / 0.5 / 1000
    shown = [float(r["shown_latency_ms"]) for r in rs if float(r["shown_latency_ms"]) >= 0]
    print(f"{k * 0.5:5.1f}s sent {kbps('sent_bytes'):5.0f} (video {kbps('video_bytes'):5.0f}, "
          f"lossless {kbps('lossless_bytes'):4.0f}) skipped {sum(int(r['skipped']) for r in rs):2d} "
          f"target {rs[-1]['target_kbps']:>5} est {rs[-1]['estimate_kbps']:>5} rtt {rs[-1]['rtt_ms']:>6} "
          f"shown {len(shown):2d}/{len(rs):2d} worst {max(shown) if shown else -1:5.0f} "
          f"largest {max(int(r['sent_bytes']) for r in rs)} B")

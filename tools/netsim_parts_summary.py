#!/usr/bin/env python3
"""Summarizes RDACCEL_FRAME_PARTS=1 output (stderr of rdaccel_netsim) in
half-second steps: lossless and video kbps, tile copies (moves), cache loads,
lossless tiles, region rects. With a second argument T (seconds), also prints
the raw lines from T to T + 1.5 s."""
import sys
from collections import defaultdict

rows = []
for line in open(sys.argv[1]):
    if not line.startswith("parts"):
        continue
    f = line.replace("(", "").replace(")", "").split()
    d = dict(zip(f[1::2], f[2::2]))
    rows.append((d, line.rstrip()))
t0 = int(rows[0][0]["t_us"])
steps = defaultdict(lambda: defaultdict(int))
for d, _ in rows:
    s = steps[(int(d["t_us"]) - t0) // 500_000]
    s["frames"] += 1
    for k in ("lossless", "nal", "total", "moves", "loads", "tiles", "stores", "rects"):
        s[k] += int(d[k])
for k in sorted(steps):
    s = steps[k]
    print(f"{k / 2:5.1f}s frames {s['frames']:2d} total {s['total'] * 16 / 1000:5.0f} kbps "
          f"(video {s['nal'] * 16 / 1000:5.0f}, lossless {s['lossless'] * 16 / 1000:5.0f}) "
          f"moves {s['moves']:5d} loads {s['loads']:4d} stores {s['stores']:4d} "
          f"lossless tiles {s['tiles']:4d} rects {s['rects']:5d}")
if len(sys.argv) > 2:
    t = float(sys.argv[2])
    for d, line in rows:
        if t <= (int(d["t_us"]) - t0) / 1e6 <= t + 1.5:
            print(line)

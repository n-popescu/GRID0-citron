#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Report actual frame counts in a fixed wall-clock window, not smoothed UI FPS."""
import argparse
import csv
import math
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("logs", nargs="+", type=Path)
parser.add_argument("--start", type=float, default=50)
parser.add_argument("--end", type=float, default=80)
parser.add_argument("--csv", type=Path, help="Optional raw frame interval CSV; includes stalls")
args = parser.parse_args()
pattern = re.compile(r"\[\s*([\d.]+)\].*CitrosisBenchmark\] interval_s=([\d.]+) frames=([\d.]+) fps=([\d.]+)")
for log in args.logs:
    contents = log.read_text(errors="replace")
    rows = [tuple(map(float, match.groups())) for match in pattern.finditer(contents)]
    rows = [row for row in rows if args.start <= row[0] <= args.end]
    failures = len(re.findall(r"Userspace PANIC|Native guest fault|VK_ERROR_DEVICE_LOST", contents))
    if not rows:
        print(f"{log.parent.name}: no measurements in requested window; failures={failures}")
        continue
    interval = sum(row[1] for row in rows)
    frames = sum(row[2] for row in rows)
    values = sorted(row[3] for row in rows)
    print(f"{log.parent.name}: {frames / interval:.2f} FPS over {interval:.2f}s, "
          f"minimum half-second FPS={values[0]:.2f}, "
          f"p05={values[int((len(values)-1)*.05)]:.2f}, failures={failures}")

if args.csv:
    with args.csv.open() as stream:
        times = sorted(float(row["frame_ms"]) for row in csv.DictReader(stream)
                       if args.start <= float(row["elapsed_s"]) <= args.end)
    if times:
        def percentile(p):
            return times[math.ceil(len(times) * p) - 1]
        print(f"Raw frame intervals: n={len(times)}, mean={sum(times)/len(times):.3f}ms, "
              f"p99={percentile(.99):.3f}ms, p99.9={percentile(.999):.3f}ms, "
              f"worst={times[-1]:.3f}ms, "
              f"over 20ms={sum(t > 20 for t in times)/len(times):.2%}, "
              f"over 100ms={sum(t > 100 for t in times)/len(times):.2%}")
    else:
        print("No raw frame intervals in requested window")

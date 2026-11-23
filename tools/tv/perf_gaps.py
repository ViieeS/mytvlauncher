#!/usr/bin/env python3
"""Frame pacing verdict for perfgate.sh: compares the base and head runs of perf_scenario.sh.

The debug build logs `present <ms> +<wait>` for every frame while a measurement runs. A gap of
24..100 ms between two presents inside motion is a missed vsync: the frame took longer than one
refresh. Longer gaps are idle time between inputs and are skipped. The BENCH/STAT averages are
not used: while anything animates the loop waits for vsync inside the timed call, so a clean
60 fps reads as "every frame 16 ms".

Per group (navigation = the bench walks, pointer = the sweep and the wiggle) the missed vsyncs
of all runs of a build are pooled. Head fails when its share exceeds base * SLACK + MARGIN. Runs
of one build on one TV spread from 2 to 6 %, and the first run after a launch reached 15 %, so
perfgate.sh discards it. The gate therefore catches a regression of several
points, such as a change that redraws a whole row per frame, not a 1-point drift.

usage: perf_gaps.py <dir with base-*.log and head-*.log>   exit 0 pass, 1 regression, 2 no data
"""
import glob
import os
import re
import sys

MISSED_MS, IDLE_MS = 24, 100
SLACK, MARGIN = 1.5, 3.0   # head may miss up to 1.5x base's share plus 3 points


def segments(path):
    """(group, [present times]) per measured segment of one perf_scenario.sh output."""
    out = []
    for line in open(path):
        m = re.match(r"cmd \d+ (bench|statbeg)$", line.strip())
        if m:
            out.append(("navigation" if m.group(1) == "bench" else "pointer", []))
            continue
        m = re.match(r"present (\d+) \+\d+", line)
        if m and out:
            out[-1][1].append(int(m.group(1)))
    return out


def pooled(paths):
    """{group: (moving gaps, missed vsyncs, worst gap ms)} over all runs of one build."""
    acc = {}
    for p in paths:
        for group, times in segments(p):
            gaps = [b - a for a, b in zip(times, times[1:]) if b - a <= IDLE_MS]
            n, missed, worst = acc.get(group, (0, 0, 0))
            acc[group] = (n + len(gaps), missed + sum(g >= MISSED_MS for g in gaps), max([worst] + gaps))
    return acc


def main(d):
    runs = {b: sorted(glob.glob(os.path.join(d, b + "-*.log"))) for b in ("base", "head")}
    stats = {b: pooled(p) for b, p in runs.items()}
    if not all(stats.values()):
        print("no measurements in", d)
        return 2
    failed = False
    print(f"{'group':11} {'build':5} {'frames':>7} {'missed':>7} {'share':>7} {'worst':>6}")
    for group in ("navigation", "pointer"):
        share = {}
        for b in ("base", "head"):
            n, missed, worst = stats[b].get(group, (0, 0, 0))
            share[b] = 100.0 * missed / n if n else 0.0
            print(f"{group:11} {b:5} {n:7} {missed:7} {share[b]:6.1f}% {worst:4}ms")
        limit = share["base"] * SLACK + MARGIN
        bad = share["head"] > limit
        failed |= bad
        print(f"{'':11} {'FAIL' if bad else 'ok':5} head {share['head']:.1f}% vs limit {limit:.1f}%")
    print(f"runs: base {len(runs['base'])}, head {len(runs['head'])}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))

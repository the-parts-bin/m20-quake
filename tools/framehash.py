#!/usr/bin/env python3
"""Compares the logs of make HASH=1 runs (saved by tdrun.py) frame by frame.

framehash.py LOG...

Frames where the logs disagree are listed with the runs that differ from the
majority and the bands that differ (0 is the top, 24 lines each; 13 and 14
are the status bar). The first line says whether the game itself went the
same way in all of them: if not, the pictures can't be compared. Frames 1 to
24 can differ between builds in one band, climbing the screen with the console."""
import collections
import re
import sys

LINE = re.compile(r"fh (\d+) ([0-9a-f]{8})((?: [0-9a-f]{4}){15}) s ([0-9a-f]{8}) (\d+) r (\d+)")


def load(path):
    frames = {}
    for line in open(path, "rb").read().decode("latin1").replace("\r", "\n").split("\n"):
        m = LINE.match(line)
        if m:
            frames[int(m[1])] = (m[2], m[3].split(), m[4], int(m[6]))
    return frames


logs = sys.argv[1:]
runs = {name: load(name) for name in logs}
common = sorted(set.intersection(*(set(r) for r in runs.values())))
state = sum(len({runs[n][i][2:] for n in logs}) > 1 for i in common)
print(f"{len(common)} frames in all {len(logs)} logs; the game differs in {state}")
wrong = collections.Counter()
for i in common:
    votes = collections.Counter(runs[n][i][0] for n in logs)
    if len(votes) == 1:
        continue
    most = votes.most_common(1)[0][0]
    good = next(runs[n][i] for n in logs if runs[n][i][0] == most)
    odd = {}
    for n in logs:
        if runs[n][i][0] != most:
            odd[n] = [b for b in range(15) if runs[n][i][1][b] != good[1][b]]
            wrong[n] += 1
    print(i, odd)
print("frames off the majority:", {n: wrong[n] for n in logs})

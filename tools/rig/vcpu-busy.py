#!/usr/bin/env python3
"""The longest stretch each vCPU ran without a voluntary switch, during the last VM in a vmguard or vmguard2 log."""
import re, sys
lines = open(sys.argv[1]).read().splitlines()
last = max([i for i, l in enumerate(lines) if 'novm' in l] + [-1])
rows = []
for line in lines[last + 1:]:
    m = re.findall(r'v(\d):(\w)(?:@\d+)?:(\d+):(\d+)', line)
    if m: rows.append((float(line.split()[0]), {int(v): (int(t), int(vol)) for v, s, t, vol in m}))
best = {}
for v in range(4):
    run = longest = 0
    for a, b in zip(rows, rows[1:]):
        if v in a[1] and v in b[1] and b[1][v][1] == a[1][v][1] and b[1][v][0] > a[1][v][0]: run += b[0] - a[0]
        else: run = 0
        longest = max(longest, run)
    best[v] = round(longest, 1)
print(f'{len(rows)} samples, longest no-sleep stretch per vCPU (s): {best}')

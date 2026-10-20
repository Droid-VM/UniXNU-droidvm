#!/usr/bin/env python3
"""Symbolize kernel addresses with out/kernel.nm: sym.py ADDR..."""
import bisect, sys
rows = []
for line in open(__import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)), 'out', 'kernel.nm')):
    parts = line.split()
    if len(parts) == 3:
        rows.append((int(parts[0], 16), parts[2]))
rows.sort()
keys = [r[0] for r in rows]
for arg in sys.argv[1:]:
    addr = int(arg, 16)
    i = bisect.bisect_right(keys, addr) - 1
    print(f'{addr:#x} -> {rows[i][1]}+{addr - rows[i][0]:#x}' if i >= 0 else f'{addr:#x} -> ?')

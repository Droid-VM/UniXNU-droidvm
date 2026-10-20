#!/usr/bin/env python3
"""Print the traced UI roles' thread rows the droidvm-monitor worker publishes (droidvm_trace_rows, BootUiTraceProbe.c).

STK<role> rows: the main thread (pid, shared cache slide, image header, pc/lr, the pid its Mach message goes to), its
registers and frames, then one row per thread group: #index xcount p<peer pid> pc lr frames. HOT rows: the busiest
threads. Roles: B = backboardd, S = SpringBoard, T/U = boot-ui-focus3/4."""
import struct
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import peek

_, count = peek.read('droidvm_trace_count', 4)
count = min(struct.unpack('<I', count)[0], 96)
_, rows = peek.read('droidvm_trace_rows', 96 * 120)
for index in range(count):
    print(rows[index * 120:(index + 1) * 120].split(b'\0')[0].decode(errors='replace'))

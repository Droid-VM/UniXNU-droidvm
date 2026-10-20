#!/usr/bin/env python3
"""Print the process rows the droidvm-monitor worker samples (boot_process_rows, 10 x 120 bytes) from guest RAM.

Each row: name pid, n threads, r running, w waiting, b busy, s state/priority of the first thread, W wait event,
C continuation, U user PC, +shared region slide (kits/droidvm/BootProcessProbe.c)."""
import struct
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import peek

_, rows = peek.read('boot_process_rows', 10 * 120)
_, passes = peek.read('droidvm_monitor_passes', 4)
print('passes', struct.unpack('<I', passes)[0])
for index in range(10):
    print(rows[index * 120:(index + 1) * 120].split(b'\0')[0].decode(errors='replace'))

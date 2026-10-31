#!/usr/bin/env python3
"""Record the guest scheduler for a while and save it: schedtrace.py SECONDS OUT.bin

The guest kernel's scheduler trace (package/DroidVMSchedTrace.c, droidvm-sched-trace.patch: every context switch and every
wakeup, with each thread's names) is switched on and off from here (droidvm_sched_trace_on) and read from guest RAM by physical
address, so nothing runs in the guest to watch it. OUT.bin holds a 16-byte header (records written, records the ring keeps,
names) and the ring and name table as the guest has them; schedtrace-decode.py reads it.
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import export  # noqa: E402
import peek  # noqa: E402

if len(sys.argv) != 3:
    sys.exit(__doc__)
seconds, out = float(sys.argv[1]), sys.argv[2]
pid, maps = peek.crosvm_maps()
q = lambda name: struct.unpack('<Q', export.read_guest(pid, maps, export.physical(name), 8))[0]  # noqa: E731
u32 = lambda name: struct.unpack('<I', export.read_guest(pid, maps, export.physical(name), 4))[0]  # noqa: E731
pa, size = q('droidvm_sched_trace_pa'), q('droidvm_sched_trace_bytes')
if not pa or not size:
    sys.exit('the guest kernel has no scheduler trace (droidvm-sched-trace=0, or a kernel without DroidVMSchedTrace.c)')
records, names = u32('droidvm_sched_trace_records'), u32('droidvm_sched_trace_names')
export.write_guest(pid, maps, export.physical('droidvm_sched_trace_head'), struct.pack('<Q', 0))
export.write_guest(pid, maps, export.physical('droidvm_sched_trace_on'), struct.pack('<I', 1))
began = time.time()
time.sleep(seconds)
export.write_guest(pid, maps, export.physical('droidvm_sched_trace_on'), struct.pack('<I', 0))
took = time.time() - began
head = q('droidvm_sched_trace_head')
data = export.read_guest(pid, maps, pa, size)
if len(data) != size:
    sys.exit(f'read {len(data)} of {size} bytes of the trace')
with open(out, 'wb') as f:
    f.write(struct.pack('<QII', head, records, names) + data)
print(f'{out}: {head} records in {took:.1f} s ({min(head, records)} kept), ring of {records}, {names} name slots')

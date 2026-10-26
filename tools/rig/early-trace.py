#!/usr/bin/env python3
"""Print each core's early bootstrap trace (droidvm_early_trace, UniXNU patches/xnu/non-apple-vcpu-start-flush.patch).

Stage a1 at the reset vector, a2 found its cpu data (+40: its address), a3 calling the reset handler (+48), a4 in start_cpu;
d0 no cpu data or handler (+8 handler, +16 entry, +24 cpu data, +32 entries table); e0+N an exception at early vector N
(+8 ELR, +16 ESR, +24 FAR, +32 SPSR); the core then waits in WFI."""
import struct, sys
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import peek
_, data = peek.read('droidvm_early_trace', 8 * 64)
for cpu in range(8):
    words = struct.unpack_from('<8Q', data, cpu * 64)
    if any(words): print(f'cpu{cpu}: stage {words[0]:#x} ' + ' '.join(f'{w:#x}' for w in words[1:]))

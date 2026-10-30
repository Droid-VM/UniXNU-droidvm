#!/usr/bin/env python3
"""gpumem.py: the guest's wired and free memory, the GPU blobs' guest memory by client (pages the virtio GPU driver allocated, the
callers' own pages it wired), and the surfaces the display keeps wired (droidvm_virtgpu_live, droidvm_virtgpu_client_memory,
droidvm_virtio_display_wired)."""
import struct, sys
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__))); import peek
def u32(n): return struct.unpack('<I', peek.read(n, 4)[1])[0]
def q(n, k): return struct.unpack(f'<{k}Q', peek.read(n, 8 * k)[1])
M = 1 << 20
print(f'wired {u32("_vm_page_wire_count") * 4096 // M} MiB, free {u32("_vm_page_free_count") * 4096 // M} MiB')
live = q('droidvm_virtgpu_live', 4); disp = q('droidvm_virtio_display_wired', 2)
print(f'virtgpu blobs: driver pages {live[0] // M} MiB ({live[2]}), user pages wired {live[1] // M} MiB ({live[3]}); '
      f'display surfaces {disp[0]} = {disp[1] // M} MiB')
d = peek.read('droidvm_virtgpu_client_memory', 64 * 64)[1]
rows = []
for i in range(64):
    name = d[64 * i:64 * i + 16].split(b'\0')[0].decode(errors='replace'); pb, ub = struct.unpack_from('<QQ', d, 64 * i + 16)
    sizes = struct.unpack_from('<8I', d, 64 * i + 32)
    if pb or ub: rows.append((pb + ub, name, pb, ub, i, sizes))
print('  live blobs by size (MiB): <1 <2 <4 <8 <16 <32 <64 64+')
for t, name, pb, ub, i, sizes in sorted(rows, reverse=True)[:14]:
    print(f'  slot {i:2} {name:16} pages {pb // M:5} MiB  user {ub // M:5} MiB  ' + ' '.join(str(n) for n in sizes))

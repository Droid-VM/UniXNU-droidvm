"""The guest's virtio-gpu 3D clients (DroidVMVirtioGpu3D.cpp gClients): per client its connection, DRM context, live resources
(guest blobs, user-page blobs, host-visible ones), their bytes and fenced submissions in flight; then the totals, to compare with
the host's dmabuf fds in crosvm (one per imported resource).

  vgpu-clients.py [--sizes] reads guest RAM (--sizes: each client's largest resources too) through crosvm's /proc/PID/mem (peek.py); the guest keeps running

Layouts are this kernel's: Client {connection, lock, context, rings, nextHandle, resourceCount, resourceCapacity, pendingCount,
closedCount, closedCapacity, resources, closed, sequence, ...}, Resource {handle, id, size, hostVisible, pages, wired, window, map} (56 bytes)."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rigenv  # noqa: F401
import peek
pid, maps = peek.crosvm_maps()
cache = {}
def rpa(pa, n):
    return peek.adb(f'dd if=/proc/{pid}/mem bs=4096 skip={peek.host_address(maps, pa, n) // 4096} count={n // 4096} 2>/dev/null')
def page(pa):
    base = pa & ~0xfff
    if base not in cache: cache[base] = rpa(base, 0x1000)
    return cache[base]
def q(pa): return struct.unpack_from('<Q', page(pa), pa & 0xfff)[0]
def kva(sym): return peek.symbol(sym) - peek.KERNEL_VA + peek.KERNEL_PA
root = q(kva('cpu_ttep'))
def walk(va):
    table = root
    for level, shift in enumerate((30, 21, 12)):
        bits = 8 if level == 0 else 9          # T1SZ 26
        d = q(table + ((va >> shift) & ((1 << bits) - 1)) * 8)
        if d & 1 == 0: return None
        out = d & 0x0000fffffffff000
        if shift == 12: return out + (va & 0xfff)
        if d & 2 == 0: return out + (va & ((1 << shift) - 1))
        table = out
def vbytes(va, n):
    out = b''
    while n:
        pa = walk(va); k = min(n, 0x1000 - (va & 0xfff))
        out += (page(pa)[pa & 0xfff:(pa & 0xfff) + k] if pa else b'\0' * k); va += k; n -= k
    return out
clients = struct.unpack('<64Q', vbytes(peek.symbol('__ZN16DroidVMVirtioGpu12_GLOBAL__N_18gClientsE'), 64 * 8))
total = dict(clients=0, resources=0, user=0, host=0, bytes=0, pending=0)
for slot, c in enumerate(clients):
    if not c: continue
    (connection, lock, context, rings, next_handle, count, capacity, pending, closed, closed_capacity, resources, closed_rows,
     sequence) = struct.unpack('<QQIIIIIIIIQQQ', vbytes(c, 72))
    rows = vbytes(resources, count * 56) if resources and count else b''
    user = host = size = 0
    sizes = []
    for i in range(count):
        handle, rid, rsize, visible, pages, wired, window, rmap = struct.unpack_from('<IIQ?7xQ?7xQQ', rows, i * 56)
        size += rsize; user += wired; host += visible; sizes.append(rsize)
    print(f'client {slot:2}: connection {connection:#x} context {context} handles {next_handle - 1} live {count} '
          f'(user {user}, host-visible {host}) {size / 2**20:.1f} MiB, pending {pending}, closed waiting {closed}, '
          f'fenced submissions {sequence}')
    if '--sizes' in sys.argv:
        big = sorted(sizes, reverse=True)
        print('    largest MiB:', ' '.join(f'{b / 2**20:.1f}' for b in big[:12]),
              f'| {sum(1 for b in sizes if b >= 4 << 20)} of 4 MiB or more, {sum(1 for b in sizes if b <= 64 << 10)} of 64 KiB or less')
    total['clients'] += 1; total['resources'] += count; total['user'] += user; total['host'] += host
    total['bytes'] += size; total['pending'] += pending
print(f"total: {total['clients']} clients, {total['resources']} live resources (user {total['user']}, host-visible "
      f"{total['host']}), {total['bytes'] / 2**20:.1f} MiB, {total['pending']} pending")

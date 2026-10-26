"""What each CPU of the stopped UniXNU guest is running: process, thread, its syscall (x16 of the user state), and a
frame-pointer backtrace from every interrupt frame on the thread's kernel stack. Offsets are this DEVELOPMENT kernel's
(assym.s and get_threadtask / task_get_proc_raw / proc_best_name)."""
import bisect, struct, sys
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import peek
pid, maps = peek.crosvm_maps()
cache = {}
def rpa(pa, n):
    return peek.adb(f'dd if=/proc/{pid}/mem bs=1 skip={peek.host_address(maps, pa, n)} count={n} 2>/dev/null')
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
def vq(va):
    pa = walk(va); return None if pa is None else q(pa)
def vbytes(va, n):
    out = b''
    while n:
        pa = walk(va); k = min(n, 0x1000 - (va & 0xfff))
        out += (page(pa)[pa & 0xfff:(pa & 0xfff) + k] if pa else b'\0' * k); va += k; n -= k
    return out
nm = {}
for line in open(peek.NM):
    p = line.split()
    if len(p) == 3 and p[1] in 'tTsS': nm[int(p[0], 16)] = p[2]
addrs = sorted(nm)
def sym(pc):
    i = bisect.bisect_right(addrs, pc) - 1
    return f'{nm[addrs[i]][1:]}+{pc - addrs[i]:#x}' if i >= 0 and pc - addrs[i] < 0x40000 else hex(pc)

proc_size = struct.unpack('<Q', rpa(0xfffffff00c07d188 - peek.KERNEL_VA + peek.KERNEL_PA, 8))[0]
def describe(thread):
    tid = vq(thread + 0x508); tro = vq(thread + 0x438); task = vq(tro + 0x28) if tro else None
    name = '?'
    if task:
        proc = task - proc_size
        raw = vbytes(proc + 0x5bc, 0x30)
        best = raw[0x11:0x11 + 32].split(b'\0')[0] or raw[:17].split(b'\0')[0]
        name = best.decode(errors='replace')
    return tid, name
def backtrace(fp, limit=14):
    out = []
    for _ in range(limit):
        if not fp or fp >> 40 != 0xffffff: break
        nxt, lr = vq(fp), vq(fp + 8)
        if lr is None: break
        out.append(sym(lr)); fp = nxt
    return out
entries = rpa(kva('CpuDataEntries'), 16 * 4)
for cpu in range(4):
    paddr = struct.unpack_from('<Q', entries, cpu * 16)[0]
    cd = rpa(paddr, 600)
    active, int_state = struct.unpack_from('<Q', cd, 40)[0], struct.unpack_from('<Q', cd, 192)[0]
    tid, name = describe(active)
    upcb = vq(active + 272); x16 = vq(upcb + 8 + 16 * 8) if upcb else None
    print(f'== cpu{cpu}: {name} tid {tid} thread {active:#x} syscall x16 {x16 if x16 is None else hex(x16)} int_state {int_state:#x}')
    top = vq(active + 344); base = (top & ~0xfff) - 0x4000
    data = vbytes(base, (top & ~0xfff) + 0x1000 - base)
    for o in range(0, len(data) - 288, 8):
        if struct.unpack_from('<II', data, o) == (21, 72):
            fp, lr, sp, pc = struct.unpack_from('<4Q', data, o + 240)
            if pc >> 40 == 0xffffff:
                print(f'  frame @{base + o:#x}: pc {sym(pc)} lr {sym(lr)}')
                print('    ' + ' <- '.join(backtrace(fp)))

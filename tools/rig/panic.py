#!/usr/bin/env python3
"""Tell whether the guest kernel has panicked, and print the panic log with its kernel addresses symbolized.

  panic.py            exit 0 with one line when there is no panic; else print the log and exit 1
  panic.py --raw      the log only, unsymbolized

A panic does not stop this VM: cpu_signal fails in DebuggerXCallEnter, so only the panicking vCPU spins, and the other
vCPUs keep running -- the rig still answers while userland stalls or launchd never starts. kernel_debugger_entry_count says
whether a CPU is in the debugger, debugger_current_op whether for a panic (3, DBOP_PANIC). The log is in the panic region
(make-droidvm-adt.py --panic-region, by default the top 512 KiB of guest RAM, found from crosvm's RAM mapping;
PANIC_REGION=0x... names another base) at the offset of debug_buf_base, up to debug_buf_ptr."""
import bisect
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rigenv  # noqa: F401
import peek

UNSLID_TEXT = 0xFFFFFFF007004000    # the kernel's __TEXT base in out/kernel.nm
DBOP_PANIC = 3
PANIC_REGION_SIZE = 0x80000


def panic_region():
    if 'PANIC_REGION' in os.environ:
        return int(os.environ['PANIC_REGION'], 16)
    _, maps = peek.crosvm_maps()
    guest, size, _ = max(maps, key=lambda mapping: mapping[1])   # guest RAM; the framebuffer's small mapping lies above it
    return guest + size - PANIC_REGION_SIZE


def word(name, size):
    _, data = peek.read(name, size)
    return int.from_bytes(data[:size], 'little')


def symbolizer():
    names = []
    for line in open(peek.NM):
        parts = line.split()
        if len(parts) >= 3 and parts[1] in 'tTsS':   # the kernel's code is in __TEXT_EXEC, which nm types s/S
            names.append((int(parts[0], 16), parts[2]))
    names.sort()
    addresses = [a for a, _ in names]

    def name(address):
        i = bisect.bisect_right(addresses, address) - 1
        return f'{names[i][1]}+0x{address - names[i][0]:x}' if i >= 0 and address - names[i][0] < 0x100000 else '?'
    return name


def main():
    entries, operation = word('kernel_debugger_entry_count', 4), word('debugger_current_op', 4)
    if not entries:
        print('no panic (no CPU in the kernel debugger)')
        return 0
    if operation != DBOP_PANIC:
        print(f'a CPU is in the kernel debugger, operation {operation} (not a panic)')
        return 1
    base, end = word('debug_buf_base', 8), word('debug_buf_ptr', 8)
    _, data = peek.read(f'{panic_region() + (base & 0x3fff):#x}', max(0, min(end - base, PANIC_REGION_SIZE - (base & 0x3fff))))
    text = data.split(b'\0', 1)[0].decode(errors='replace')
    if '--raw' in sys.argv:
        print(text)
        return 1
    found = re.search(r'Kernel text base:\s+0x([0-9a-f]+)', text)
    slide = int(found[1], 16) - UNSLID_TEXT if found else 0
    name = symbolizer()
    for line in text.splitlines():
        addresses = [int(a, 16) for a in re.findall(r'0x(fffffff[0-9a-f]{9})', line)]
        code = [f'{a:#x} = {name(a - slide)}' for a in addresses if name(a - slide) != '?']
        print(line + (f'    [{", ".join(code)}]' if code and re.search(r'\b(pc|lr|caller)\b', line) else ''))
    return 1


if __name__ == '__main__':
    sys.exit(main())

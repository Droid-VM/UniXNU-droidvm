#!/usr/bin/env python3
"""Make the minimal-UBSan overflow traps of a COPY of the kernel record their caller instead of trapping.

A DEVELOPMENT XNU built with -fsanitize=...,minimal calls __ubsan_handle_*_overflow_minimal, a bare
`brk #0xff00; ret`, on every checked integer overflow; sleh_synchronous then logs it and resumes. Before
arm_init has a current thread (TPIDR_EL1 = 0) that handler faults, and the nested fault recurses until
the exception stack runs through the kernel image, erasing the original state. This probe replaces each
`brk #0xff00` with a branch to one stub placed over _sysctl_ubsan_test (a test sysctl, never called at boot):

    slot+0x00  LR of the first overflow     (the call site of the first handler hit)
    slot+0x08  number of overflows
    slot+0x10  LR of the latest overflow

The bootstrap mapping of the kernel is writable, so the slots live in the stub's own text; once arm_vm_init
makes text read-only a later first overflow would fault, which is itself a finding. Read the slots from
the host (peek.py). Never apply this to the original kernel.

  patch-ubsan-probe.py IN.macho OUT.macho
"""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HANDLERS = ['___ubsan_handle_sub_overflow_minimal', '___ubsan_handle_add_overflow_minimal',
            '___ubsan_handle_divrem_overflow_minimal', '___ubsan_handle_negate_overflow_minimal',
            '___ubsan_handle_mul_overflow_minimal']
CAVE = '_sysctl_ubsan_test'
BRK_FF00 = 0xd43fe000
SLOT_OFFSET = 0x50          # slots at CAVE+0x50 .. +0x68, inside the 0x68-byte function


def segments(data):
    magic, cputype, _, filetype, ncmds, _, _, _ = struct.unpack_from('<8I', data, 0)
    if magic != 0xfeedfacf:
        raise SystemExit('not a 64-bit Mach-O')
    result, offset = [], 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x19:
            name = data[offset + 8:offset + 24].rstrip(b'\0').decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', data, offset + 24)
            result.append((name, vmaddr, vmsize, fileoff, filesize))
        offset += size
    return result


def file_offset(segs, va):
    for name, vmaddr, vmsize, fileoff, filesize in segs:
        if vmaddr <= va < vmaddr + filesize:
            return fileoff + va - vmaddr
    raise SystemExit(f'{va:#x} is in no file-backed segment')


def symbols(path):
    out = subprocess.run(['llvm-nm', '--defined-only', path], capture_output=True, text=True, check=True).stdout
    table = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            table[parts[2]] = int(parts[0], 16)
    return table


def assemble(source, base):
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / 'a.s').write_text(source)
        subprocess.run(['aarch64-linux-gnu-as', '-o', f'{tmp}/a.o', f'{tmp}/a.s'], check=True)
        subprocess.run(['aarch64-linux-gnu-objcopy', '-O', 'binary', '-j', '.text', f'{tmp}/a.o', f'{tmp}/a.bin'], check=True)
        return (Path(tmp) / 'a.bin').read_bytes()


def main():
    source, target = sys.argv[1:3]
    data = bytearray(Path(source).read_bytes())
    segs = segments(data)
    table = symbols(source)
    cave = table[CAVE]
    if table[HANDLERS[2]] - cave != 0x68:
        raise SystemExit('unexpected layout: the cave must end where the divrem handler starts')
    stub = assemble(f"""
        .text
        adr  x16, slot
        ldr  x17, [x16]
        cbnz x17, 1f
        str  x30, [x16]
    1:  ldr  x17, [x16, #8]
        add  x17, x17, #1
        str  x17, [x16, #8]
        str  x30, [x16, #16]
        ret
        .org {SLOT_OFFSET}
    slot:
        .quad 0, 0, 0
    """, cave)
    if len(stub) != SLOT_OFFSET + 24 or len(stub) > 0x68:
        raise SystemExit(f'stub is {len(stub)} bytes')
    offset = file_offset(segs, cave)
    data[offset:offset + 0x68] = stub.ljust(0x68, b'\0')
    for name in HANDLERS:
        va = table[name]
        at = file_offset(segs, va)
        if struct.unpack_from('<I', data, at)[0] != BRK_FF00:
            raise SystemExit(f'{name} does not start with brk #0xff00')
        delta = (cave - va) >> 2
        struct.pack_into('<I', data, at, 0x14000000 | (delta & 0x3ffffff))     # b cave
        print(f'{name} @ {va:#x}: brk -> b {cave:#x}')
    Path(target).write_bytes(data)
    print(f'stub @ {cave:#x}, slots @ {cave + SLOT_OFFSET:#x} (first LR, count, last LR)')


if __name__ == '__main__':
    main()

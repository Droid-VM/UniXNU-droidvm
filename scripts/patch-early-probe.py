#!/usr/bin/env python3
"""Early-boot probes for a COPY of the kernel, read back from the host through guest memory.

A DroidVM guest has no kernel serial console, but the host can read guest RAM (peek.py). Before arm_init
sets a current thread, TPIDR_EL1 is 0 and any EL1 exception recurses: the SP0 vector and
check_exception_stack read the thread from TPIDR_EL1, fault, and re-enter until the exception stack runs
through the kernel image and erases the original state. The probes:

  catcher (always)  The EL1 SP0 and SP1 synchronous vectors branch to a stub (over _pmap_coredump_test, a
                    xnupost self-test never run at boot). With TPIDR_EL1 = 0 it records the FIRST exception
                    and parks the CPU (wfe loop, interrupts masked):
                      slot+0x00 ESR_EL1  +0x08 ELR_EL1  +0x10 FAR_EL1  +0x18 SPSR_EL1
                      slot+0x20 x30      +0x28 x29      +0x30 SP       +0x38 vector (0 = SP0, 1 = SP1)
                    Otherwise it restores x16/x17, replays the displaced instruction and resumes the vector.
  --ubsan keep      leave the minimal-UBSan overflow traps (brk #0xff00); the catcher reports one as
                    ESR 0xf200ff00 with ELR at the handler and x30 at the overflowing call site
  --ubsan nop       turn each overflow handler into a plain return, to see what comes after

The bootstrap mapping of the kernel is writable, so the slots live in the stub's own text.

  patch-early-probe.py IN.macho OUT.macho [--ubsan keep|nop]
"""
import argparse
import struct
import subprocess
import tempfile
from pathlib import Path

UBSAN = ['___ubsan_handle_sub_overflow_minimal', '___ubsan_handle_add_overflow_minimal',
         '___ubsan_handle_divrem_overflow_minimal', '___ubsan_handle_negate_overflow_minimal',
         '___ubsan_handle_mul_overflow_minimal']
CAVE = '_pmap_coredump_test'
SP0 = 'el1_sp0_synchronous_vector_long'
SP1 = 'el1_sp1_synchronous_vector_long'
CHECK = 'check_exception_stack'
BRK_FF00 = 0xd43fe000
NOP = 0xd503201f
STP_X0_X1_PRE = 0xa9bf07e0            # stp x0, x1, [sp, #-0x10]!

CATCHER = """
    .text
stub_sp0:
    stp  x16, x17, [sp, #-16]!
    mov  x17, #0
    b    common
stub_sp1:
    stp  x16, x17, [sp, #-16]!
    mov  x17, #1
common:
    mrs  x16, tpidr_el1
    cbnz x16, resume
    adr  x16, slots
    str  x17, [x16, #56]
    ldr  x17, [x16]
    cbnz x17, spin
    mrs  x17, esr_el1
    str  x17, [x16]
    mrs  x17, elr_el1
    str  x17, [x16, #8]
    mrs  x17, far_el1
    str  x17, [x16, #16]
    mrs  x17, spsr_el1
    str  x17, [x16, #24]
    str  x30, [x16, #32]
    str  x29, [x16, #40]
    add  x17, sp, #16
    str  x17, [x16, #48]
    dsb  sy
spin:
    wfe
    b    spin
resume:
    cbnz x17, resume_sp1
    ldp  x16, x17, [sp], #16
    stp  x0, x1, [sp, #-16]!
to_sp0:
    .word 0
resume_sp1:
    ldp  x16, x17, [sp], #16
to_check:
    .word 0
    .balign 16
slots:
    .quad 0, 0, 0, 0, 0, 0, 0, 0
end:
"""


def segments(data):
    magic, _, _, _, ncmds = struct.unpack_from('<5I', data, 0)
    if magic != 0xfeedfacf:
        raise SystemExit('not a 64-bit Mach-O')
    result, offset = [], 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x19:
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', data, offset + 24)
            result.append((vmaddr, fileoff, filesize))
        offset += size
    return result


def file_offset(segs, va):
    for vmaddr, fileoff, filesize in segs:
        if vmaddr <= va < vmaddr + filesize:
            return fileoff + va - vmaddr
    raise SystemExit(f'{va:#x} is in no file-backed segment')


def symbols(path):
    out = subprocess.run(['llvm-nm', '--defined-only', path], capture_output=True, text=True, check=True).stdout
    return {parts[2]: int(parts[0], 16) for parts in (line.split() for line in out.splitlines()) if len(parts) == 3}


def assemble(source):
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / 'a.s').write_text(source)
        subprocess.run(['aarch64-linux-gnu-as', '-o', f'{tmp}/a.o', f'{tmp}/a.s'], check=True)
        subprocess.run(['aarch64-linux-gnu-objcopy', '-O', 'binary', '-j', '.text', f'{tmp}/a.o', f'{tmp}/a.bin'], check=True)
        labels = {}
        for line in subprocess.run(['aarch64-linux-gnu-nm', f'{tmp}/a.o'], capture_output=True, text=True, check=True).stdout.splitlines():
            value, _, name = line.split()
            labels[name] = int(value, 16)
        return bytearray((Path(tmp) / 'a.bin').read_bytes()), labels


def branch(source, target):
    delta = target - source
    if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise SystemExit(f'branch {source:#x} -> {target:#x} out of range')
    return 0x14000000 | ((delta >> 2) & 0x3ffffff)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('input')
    parser.add_argument('output')
    parser.add_argument('--ubsan', choices=['keep', 'nop'], default='keep')
    args = parser.parse_args()
    data = bytearray(Path(args.input).read_bytes())
    segs = segments(data)
    table = symbols(args.input)
    put = lambda va, word: struct.pack_into('<I', data, file_offset(segs, va), word)
    get = lambda va: struct.unpack_from('<I', data, file_offset(segs, va))[0]

    cave, sp0, sp1, check = table[CAVE], table[SP0], table[SP1], table[CHECK]
    if get(sp0) != STP_X0_X1_PRE:
        raise SystemExit(f'{SP0} does not start with stp x0, x1, [sp, #-0x10]!')
    if get(sp1) != branch(sp1, check):
        raise SystemExit(f'{SP1} does not start with b {CHECK}')
    code, labels = assemble(CATCHER)
    if labels['end'] > 0xa40:
        raise SystemExit('catcher larger than the cave')
    struct.pack_into('<I', code, labels['to_sp0'], branch(cave + labels['to_sp0'], sp0 + 4))
    struct.pack_into('<I', code, labels['to_check'], branch(cave + labels['to_check'], check))
    offset = file_offset(segs, cave)
    data[offset:offset + len(code)] = code
    put(sp0, branch(sp0, cave + labels['stub_sp0']))
    put(sp1, branch(sp1, cave + labels['stub_sp1']))
    print(f'catcher @ {cave:#x}; slots @ {cave + labels["slots"]:#x} '
          '(ESR, ELR, FAR, SPSR, x30, x29, SP, vector)')

    for name in UBSAN:
        va = table[name]
        if get(va) != BRK_FF00:
            raise SystemExit(f'{name} does not start with brk #0xff00')
        if args.ubsan == 'nop':
            put(va, NOP)
    print(f'UBSan overflow traps: {args.ubsan}')
    Path(args.output).write_bytes(data)


if __name__ == '__main__':
    main()

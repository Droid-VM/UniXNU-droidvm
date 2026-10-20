#!/usr/bin/env python3
"""Read the guest memory of the running UniXNU VM from the host side (crosvm's /proc/PID/mem).

  peek.py SYMBOL|0xPA|va:VA [LENGTH] [--hex|--str|--q]

A symbol comes from out/kernel.nm (llvm-nm -n of kernel.macho). The kernel image is mapped linearly
from DarwinBoot's hand-off (KERNEL_VA -> KERNEL_PA, from the console line "DARWINBOOT: ARM64 va= pa=").
Guest PA -> host address goes through the crosvm_guest memfd mapping whose file offset is PA - RAM_BASE.
"""
import argparse
import re
import shlex
import struct
import subprocess
import sys

PHONE = __import__('os').environ['PHONE']   # the adb serial of the phone
NM = __import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)), 'out', 'kernel.nm')
KERNEL_VA = 0xFFFFFFF00427C000
KERNEL_PA = 0x84A7C000
MEMFD_GUEST_BASE = 0x80000000          # crosvm_guest memfd offset 0 is guest PA 0x80000000


def adb(command):
    return subprocess.run(['adb', '-s', PHONE, 'shell', 'su -c ' + shlex.quote(command)], capture_output=True, timeout=60).stdout


def crosvm_maps():
    pid = adb("for p in $(pidof crosvm); do tr '\\0' ' ' < /proc/$p/cmdline | grep -q UniXNU && echo $p; done").split()[0].decode()
    maps = []
    for line in adb(f'grep crosvm_guest /proc/{pid}/maps').decode().splitlines():
        span, perms, offset = line.split()[:3]
        start, end = (int(x, 16) for x in span.split('-'))
        maps.append((int(offset, 16) + MEMFD_GUEST_BASE, end - start, start))
    return pid, maps


def host_address(maps, pa, length):
    for guest, size, host in maps:
        if guest <= pa and pa + length <= guest + size:
            return host + pa - guest
    raise SystemExit(f'guest PA {pa:#x}+{length:#x} is not in a crosvm_guest mapping')


def symbol(name):
    for line in open(NM):
        parts = line.split()
        if len(parts) == 3 and parts[2] in (name, '_' + name):
            return int(parts[0], 16)
    raise SystemExit(f'no symbol {name}')


def read(target, length):
    if target.startswith('0x'):
        pa = int(target, 16)
    else:
        va = int(target[3:], 16) if target.startswith('va:') else symbol(target)
        pa = va - KERNEL_VA + KERNEL_PA
    pid, maps = crosvm_maps()
    host = host_address(maps, pa, length)
    data = adb(f'dd if=/proc/{pid}/mem bs=1 skip={host} count={length} 2>/dev/null')
    return pa, data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target')
    parser.add_argument('length', nargs='?', type=lambda v: int(v, 0), default=8)
    parser.add_argument('--str', action='store_true')
    parser.add_argument('--q', action='store_true', help='print as 64-bit words')
    args = parser.parse_args()
    pa, data = read(args.target, args.length)
    if args.str:
        print(data.split(b'\0')[0].decode(errors='replace'))
    elif args.q:
        for index in range(0, len(data) - 7, 8):
            print(f'{pa + index:#x}: {struct.unpack_from("<Q", data, index)[0]:#018x}')
    else:
        for index in range(0, len(data), 16):
            chunk = data[index:index + 16]
            print(f'{pa + index:#x}: {chunk.hex(" ")}  {"".join(chr(c) if 32 <= c < 127 else "." for c in chunk)}')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""kernel.nm for an MH_FILESET kernel: the native kernel's symbols moved to where the fileset emitter placed each of its
segments (the com.apple.kernel entry's segments, by name and order). fileset-syms.py FILESET NATIVE NATIVE.nm OUT.nm"""
import struct, sys
def segments(data, offset=0):
    magic, cpu, sub, kind, ncmds, size = struct.unpack_from('<6I', data, offset)
    at, out, entries = offset + 32, [], []
    for _ in range(ncmds):
        cmd, length = struct.unpack_from('<II', data, at)
        if cmd == 0x19:
            name = data[at + 8:at + 24].split(b'\0')[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', data, at + 24)
            out.append((name, vmaddr, vmsize, fileoff))
        elif cmd == 0x80000035:   # LC_FILESET_ENTRY
            vmaddr, fileoff, nameoff = struct.unpack_from('<QQI', data, at + 8)
            entries.append((data[at + nameoff:data.index(b'\0', at + nameoff)].decode(), vmaddr, fileoff))
        at += length
    return out, entries
fileset, native = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
_, entries = segments(fileset)
kernel = next(e for e in entries if e[0] == 'com.apple.kernel')
placed, _ = segments(fileset, kernel[2])
original, _ = segments(native)
deltas = []
for name, vmaddr, vmsize, _ in original:
    match = [p for p in placed if p[0] == name]
    if not match or not vmsize:
        continue
    deltas.append((vmaddr, vmaddr + vmsize, match[0][1] - vmaddr, name))
for lo, hi, d, name in deltas:
    print(f'{name:20} {lo:#x}+{hi - lo:#x} -> {lo + d:#x} (delta {d:+#x})', file=sys.stderr)
with open(sys.argv[3]) as src, open(sys.argv[4], 'w') as dst:
    for line in src:
        parts = line.split()
        if len(parts) >= 3:
            value = int(parts[0], 16)
            for lo, hi, d, _ in deltas:
                if lo <= value < hi:
                    line = f'{value + d:016x} {" ".join(parts[1:])}\n'
                    break
        dst.write(line)

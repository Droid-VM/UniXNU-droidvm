#!/usr/bin/env python3
"""The symbols of the kexts of a symbol-bearing kernel collection (macOS 27's), as nm lines at their address in the running
collection (each LC_SYMTAB's own nlist values, plus SLIDE: XnuPrelink's collection_slide, 0x1f000000000 for the virtual Mac's).
kext-syms.py FILESET SLIDE OUT.nm"""
import struct, sys
data = open(sys.argv[1], 'rb').read()
slide = int(sys.argv[2], 0)
magic, cpu, sub, kind, ncmds, size = struct.unpack_from('<6I', data, 0)
entries, at = [], 32
for _ in range(ncmds):
    cmd, length = struct.unpack_from('<II', data, at)
    if cmd == 0x80000035:                       # LC_FILESET_ENTRY
        va, fileoff, name_at = struct.unpack_from('<QQI', data, at + 8)
        entries.append((data[at + name_at:data.index(b'\0', at + name_at)].decode(), fileoff))
    at += length
out = []
for name, offset in entries:
    if name == 'com.apple.kernel':
        continue
    ncmds = struct.unpack_from('<I', data, offset + 16)[0]
    at = offset + 32
    for _ in range(ncmds):
        cmd, length = struct.unpack_from('<II', data, at)
        if cmd == 0x2:                          # LC_SYMTAB
            symoff, nsyms, stroff, strsize = struct.unpack_from('<4I', data, at + 8)
            for i in range(nsyms):
                strx, ntype, nsect, ndesc, value = struct.unpack_from('<IBBHQ', data, symoff + 16 * i)
                if ntype & 0xe0 or not (ntype & 0x0e) == 0x0e or not value:
                    continue
                symbol = data[stroff + strx:data.index(b'\0', stroff + strx)].decode(errors='replace')
                out.append(f'{(value + slide) & (2 ** 64 - 1):016x} t {symbol}@{name.removeprefix("com.apple.")}')
        at += length
out.sort()
open(sys.argv[3], 'w').write('\n'.join(out) + '\n')
print(len(entries), 'entries', len(out), 'symbols')

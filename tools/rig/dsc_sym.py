#!/usr/bin/env python3
"""Symbolize user addresses of a DroidVM guest against the iPadOS dyld shared cache (runs where the cache is, the Mac).

  dsc_sym.py CACHE_DIR SLIDE ADDRESS...          (hex; SLIDE is the shared region slide the trace prints as cache+<slide>)

CACHE_DIR holds dyld_shared_cache_arm64 and its subcaches (.01 ... .dylddata, .dyldlinkedit, .symbols), e.g. the SystemOS
cryptex's System/Library/Caches/com.apple.dyld. Each address is unslid, its image found from the cache's image list and
__TEXT extents, and named by the closest preceding symbol of that image: the exported ones from its LC_SYMTAB and the
local ones (static functions, Objective-C methods) from the .symbols file."""
import bisect
import mmap
import struct
import sys
from pathlib import Path

LC_SEGMENT_64, LC_SYMTAB = 0x19, 0x2


class Cache:
    def __init__(self, directory):
        files = sorted(p for p in Path(directory).glob('dyld_shared_cache_arm64*') if not p.name.endswith('.symbols')
                       and not p.name.endswith('.map') and not p.name.endswith('.atlas'))
        self.maps = []
        self.main = None
        for path in files:
            handle = open(path, 'rb')
            data = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
            if data[:15] != b'dyld_v1   arm64':
                continue
            if path.name == 'dyld_shared_cache_arm64':
                self.main = data
            offset, count = struct.unpack_from('<II', data, 0x10)
            for index in range(count):
                address, size, file_offset = struct.unpack_from('<QQQ', data, offset + index * 32)
                self.maps.append((address, size, file_offset, data))
        self.maps.sort(key=lambda m: m[0])
        self.base = self.maps[0][0]
        images_offset, images_count = struct.unpack_from('<II', self.main, 0x1c0)
        if not images_offset:
            images_offset, images_count = struct.unpack_from('<II', self.main, 0x18)
        self.images = []
        for index in range(images_count):
            address, _, _, path_offset = struct.unpack_from('<QQQI', self.main, images_offset + index * 32)
            end = self.main.find(b'\0', path_offset)
            self.images.append((address, self.main[path_offset:end].decode()))
        self.images.sort()
        self.starts = [image[0] for image in self.images]
        self.symbols_file = Path(directory) / 'dyld_shared_cache_arm64.symbols'
        self.local_entries = None
        self.parsed = {}

    def read(self, address, size):
        for start, length, file_offset, data in self.maps:
            if start <= address and address + size <= start + length:
                at = file_offset + address - start
                return data[at:at + size]
        return None

    def segments(self, header):
        magic, _, _, _, ncmds, _ = struct.unpack_from('<IiiIII', self.read(header, 28))
        commands = self.read(header + 32, 0x10000)
        segments, symtab, at = {}, None, 0
        for _ in range(ncmds):
            command, size = struct.unpack_from('<II', commands, at)
            if command == LC_SEGMENT_64:
                name = commands[at + 8:at + 24].split(b'\0')[0].decode()
                segments[name] = struct.unpack_from('<QQQQ', commands, at + 24)   # vmaddr, vmsize, fileoff, filesize
            elif command == LC_SYMTAB:
                symtab = struct.unpack_from('<IIII', commands, at + 8)              # symoff, nsyms, stroff, strsize
            at += size
        return segments, symtab

    def locals_for(self, header):
        if self.local_entries is None:
            self.local_entries = {}
            if self.symbols_file.exists():
                handle = open(self.symbols_file, 'rb')
                data = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
                info = struct.unpack_from('<Q', data, 0x48)[0]
                nlist_offset, nlist_count, strings_offset, _, entries_offset, entries_count = struct.unpack_from('<6I', data, info)
                self.local_data = (data, info + nlist_offset, info + strings_offset)
                for index in range(entries_count):
                    dylib, start, count = struct.unpack_from('<QII', data, info + entries_offset + index * 16)
                    self.local_entries[dylib] = (start, count)
        entry = self.local_entries.get(header - self.base)
        if not entry:
            return []
        data, nlists, strings = self.local_data
        symbols = []
        for index in range(entry[0], entry[0] + entry[1]):
            strx, kind, _, _, value = struct.unpack_from('<IBBHQ', data, nlists + index * 16)
            if kind & 0xe0 or (kind & 0x0e) != 0x0e:
                continue
            end = data.find(b'\0', strings + strx)
            symbols.append((value, data[strings + strx:end].decode(errors='replace')))
        return symbols

    def image(self, header):
        if header in self.parsed:
            return self.parsed[header]
        segments, symtab = self.segments(header)
        text = segments.get('__TEXT', (header, 0, 0, 0))
        symbols = []
        linkedit = segments.get('__LINKEDIT')
        if symtab and linkedit:
            symoff, nsyms, stroff, strsize = symtab
            table = self.read(linkedit[0] + symoff - linkedit[2], nsyms * 16)
            strings = self.read(linkedit[0] + stroff - linkedit[2], strsize)
            if table and strings:
                for index in range(nsyms):
                    strx, kind, _, _, value = struct.unpack_from('<IBBHQ', table, index * 16)
                    if kind & 0xe0 or (kind & 0x0e) != 0x0e:
                        continue
                    end = strings.find(b'\0', strx)
                    symbols.append((value, strings[strx:end].decode(errors='replace')))
        symbols += self.locals_for(header)
        symbols.sort()
        self.parsed[header] = (text[0], text[0] + text[1], symbols)
        return self.parsed[header]

    def symbolize(self, address):
        index = bisect.bisect_right(self.starts, address) - 1
        if index < 0:
            return 'outside the cache'
        header, path = self.images[index]
        start, end, symbols = self.image(header)
        name = path.rsplit('/', 1)[-1]
        if not start <= address < end:
            return f'outside the cache text ({name}+{address - header:#x}?)'
        values = [s[0] for s in symbols]
        at = bisect.bisect_right(values, address) - 1
        if at < 0:
            return f'{name}+{address - header:#x}'
        return f'{name}`{symbols[at][1]}+{address - symbols[at][0]:#x}'


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    cache = Cache(sys.argv[1])
    slide = int(sys.argv[2], 16)
    for text in sys.argv[3:]:
        address = int(text, 16)
        print(f'{address:#x}  {cache.symbolize(address - slide)}')


if __name__ == '__main__':
    main()

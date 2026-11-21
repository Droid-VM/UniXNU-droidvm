#!/usr/bin/env python3
"""The UI-scale fixups of an iPadOS build, found in its dyld shared cache's code rather than kept as offsets of one build.

  find-ui-scale-fixups.py CACHE_DIR [--max-height N] [--out FILE]

CACHE_DIR: the SystemOS cryptex's System/Library/Caches/com.apple.dyld (dyld_shared_cache_arm64e, its subcaches and .symbols).

QuartzCore's -[CADisplayMode preferredScale] gives a mode without a preferred UI scale of its own (an internal panel's) @2x on a
virtual device (CADeviceIsVirtualized) whatever its size, and otherwise @2x when it is taller than 1080 pixels, @1x when not. So
half of a 12.9- or 13-inch iPad's pixels came out laid out as an iPad mini's. Two fixups of that method (the kernel's
native-text-fixup: /chosen/text-fixups, applied to the signed text after its page is validated, only where the original bytes
match) send a virtual device to the height rule and raise the rule's limit to N (default 1920): a VM display up to N pixels tall
is @1x, a taller one @2x as before. In 24A446 the method reads:

    ldrb w8, [x8, #...]          CADeviceIsVirtualized's cached answer
    ldr  x9, [x0, #0x8]
    cmp  w8, #1
    b.ne height                  not virtual: the height rule
    ldr  w8, [x9, #0x24]         virtual: max(the mode's scale, 2)   <- "b height"
    ...
  height:
    ldr  x8, [x9, #0x18]         the mode's height in pixels
    cmp  x8, #1080                                                   <- "cmp x8, #N"
    mov  w8, #1 ; cinc x8, x8, hi

They are found by what the code does, not where it is. The virtual test: the method's load (adrp + ldrb) of the byte
CADeviceIsVirtualized caches, __ZZ21CADeviceIsVirtualizedE14is_virtualized, found by its local symbol, and the branch that tests
it (cmp #1 + b.eq/b.ne, tbz/tbnz on bit 0, cbz/cbnz); the virtual side's first instruction becomes a branch to the other side.
Every path into that side goes the other way with it -- in 24A446 also a mode flag's (tbnz on bit 4 of the mode's byte 3), as
the fixup tested on the phones since 11-03 did. The height rule: the method's one 64-bit compare with 1080. Checked 11-21 on
three caches: iPadOS 27 24A446 (the two fixups of 11-03, byte for byte), macOS 27 26A434 (tbz instead of cmp, subcache .03; an
earlier matcher on "ldrb + cmp #1" alone took the Catalyst display test there), and iPadOS 17 21H461, whose method asks nothing
about virtualisation (no load of the byte: the height fixup alone, said on stderr). A method that reads the byte but whose test
is not recognised, or has no 1080 compare, stops the build with what was missing: a build whose QuartzCore reads otherwise
needs a look, not a silent skip (the kernel would skip mismatching fixups without a word).

Prints each fixup as FILE,OFFSET,ORIGINAL,REPLACEMENT (hex; make-droidvm-adt.py --text-fixup's form) on stdout, or into --out,
one a line, for make-droidvm-adt.py --text-fixups-file."""
import argparse
import mmap
import struct
import sys
from pathlib import Path

LC_SEGMENT_64, LC_SYMTAB = 0x19, 0x2
METHOD = '-[CADisplayMode preferredScale]'
FRAMEWORK, BINARY = '/QuartzCore.framework/', '/QuartzCore'   # iOS: .../QuartzCore; macOS: .../Versions/A/QuartzCore
FLAG = '__ZZ21CADeviceIsVirtualizedE14is_virtualized'   # its cached answer
HEIGHT_LIMIT = 1080


class Cache:
    """The shared cache of CACHE_DIR: its mappings (with the file each comes from), its images and their symbols."""

    def __init__(self, directory):
        directory = Path(directory)
        mains = [p for p in (directory / 'dyld_shared_cache_arm64e', directory / 'dyld_shared_cache_arm64') if p.is_file()]
        if not mains:
            sys.exit(f'{directory}: no dyld_shared_cache_arm64e (or arm64)')
        self.main_path = mains[0]
        self.maps = []
        self.main = None
        for path in sorted(directory.glob(self.main_path.name + '*')):
            if path.suffix in ('.symbols', '.map', '.atlas', '.a2s'):
                continue
            with open(path, 'rb') as handle:
                data = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
            if data[:15] not in (b'dyld_v1   arm64', b'dyld_v1  arm64e'):
                continue
            if path == self.main_path:
                self.main = data
            offset, count = struct.unpack_from('<II', data, 0x10)
            for index in range(count):
                address, size, file_offset = struct.unpack_from('<QQQ', data, offset + index * 32)
                self.maps.append((address, size, file_offset, data, path.name))
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
        self.symbols_path = directory / (self.main_path.name + '.symbols')

    def locate(self, address, size):
        """(file name, offset in it, the bytes) of [address, address + size), which must lie in one mapping."""
        for start, length, file_offset, data, name in self.maps:
            if start <= address and address + size <= start + length:
                at = file_offset + address - start
                return name, at, bytes(data[at:at + size])
        sys.exit(f'{address:#x}: in no mapping of the cache')

    def read(self, address, size):
        return self.locate(address, size)[2]

    def symbols(self, header):
        """The image's symbols as (address, name): exported ones from its LC_SYMTAB, local ones from the .symbols file."""
        magic, _, _, _, ncmds, _ = struct.unpack_from('<IiiIII', self.read(header, 28))
        commands = self.read(header + 32, 0x8000)
        segments, symtab, at = {}, None, 0
        for _ in range(ncmds):
            command, size = struct.unpack_from('<II', commands, at)
            if command == LC_SEGMENT_64:
                name = commands[at + 8:at + 24].split(b'\0')[0].decode()
                segments[name] = struct.unpack_from('<QQQQ', commands, at + 24)
            elif command == LC_SYMTAB:
                symtab = struct.unpack_from('<IIII', commands, at + 8)
            at += size
        out = []
        linkedit = segments.get('__LINKEDIT')
        if symtab and linkedit:
            symoff, nsyms, stroff, strsize = symtab
            table = self.read(linkedit[0] + symoff - linkedit[2], nsyms * 16)
            strings = self.read(linkedit[0] + stroff - linkedit[2], strsize)
            out += self.nlists(table, 0, nsyms, strings, 0)
        if self.symbols_path.is_file():
            with open(self.symbols_path, 'rb') as handle:
                data = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
            info = struct.unpack_from('<Q', data, 0x48)[0]
            nlist_offset, _, strings_offset, _, entries_offset, entries_count = struct.unpack_from('<6I', data, info)
            for index in range(entries_count):
                dylib, start, count = struct.unpack_from('<QII', data, info + entries_offset + index * 16)
                if dylib == header - self.base:
                    out += self.nlists(data, info + nlist_offset + start * 16, count, data, info + strings_offset)
        return sorted(out)

    @staticmethod
    def nlists(table, at, count, strings, strings_at):
        out = []
        for index in range(count):
            strx, kind, _, _, value = struct.unpack_from('<IBBHQ', table, at + index * 16)
            if kind & 0xe0 or (kind & 0x0e) != 0x0e:     # defined in a section, not a debugging entry
                continue
            end = strings.find(b'\0', strings_at + strx)
            out.append((value, bytes(strings[strings_at + strx:end]).decode(errors='replace')))
        return out


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def cmp_immediate(word):
    """(is 64-bit, register, value) of CMP Rn, #imm (SUBS ZR, Rn, #imm), or None."""
    if (word & 0x7f80001f) != 0x7100001f:
        return None
    value = (word >> 10) & 0xfff
    return bool(word >> 31), (word >> 5) & 31, value << 12 if word & (1 << 22) else value


def adrp(word, address):
    """(register, page) of ADRP Xd, page, or None."""
    if (word & 0x9f000000) != 0x90000000:
        return None
    offset = signed((((word >> 5) & 0x7ffff) << 2) | ((word >> 29) & 3), 21) << 12
    return word & 31, (address & ~0xfff) + offset


def ldrb(word):
    """(destination, base register, offset) of LDRB Wt, [Xn, #imm], or None."""
    if (word & 0xffc00000) != 0x39400000:
        return None
    return word & 31, (word >> 5) & 31, (word >> 10) & 0xfff


def test_branch(words, i, register):
    """How words[i] (and words[i + 1]) branch on W<register> being 1: (index of the branch, its target, whether the target is
    the true case), or None when words[i] is not such a test."""
    address, word = words[i]
    compare = cmp_immediate(word)
    if compare and not compare[0] and compare[1] == register and compare[2] == 1 and i + 1 < len(words):
        branch_address, branch = words[i + 1]
        if (branch & 0xff000010) == 0x54000000 and (branch & 15) in (0, 1):          # b.eq / b.ne
            return i + 1, branch_address + signed((branch >> 5) & 0x7ffff, 19) * 4, (branch & 15) == 0
    if (word & 0x7e000000) == 0x36000000 and (word & 31) == register and ((word >> 19) & 31) == 0 and not word >> 31:
        return i, address + signed((word >> 5) & 0x3fff, 14) * 4, bool(word & (1 << 24))   # tbz / tbnz w, #0
    if (word & 0x7e000000) == 0x34000000 and (word & 31) == register:
        return i, address + signed((word >> 5) & 0x7ffff, 19) * 4, bool(word & (1 << 24))   # cbz / cbnz
    return None


def writes(word, register):
    """Whether an instruction plausibly writes the register (its Rd/Rt field), to stop a forward scan."""
    return (word & 31) == register and (word & 0x7f80001f) != 0x7100001f and (word & 0x7e000000) not in (0x34000000, 0x36000000) \
        and (word & 0xff000010) != 0x54000000


def branch_to(address, target):
    return 0x14000000 | (((target - address) >> 2) & 0x3ffffff)


def find(cache, max_height):
    images = [(header, path) for header, path in cache.images if FRAMEWORK in path and path.endswith(BINARY)]
    if len(images) != 1:
        sys.exit(f'{len(images)} QuartzCore images in the cache')
    symbols = cache.symbols(images[0][0])
    named = {}
    for value, name in symbols:
        named.setdefault(name, []).append(value)
    for name in (METHOD, FLAG):
        if len(named.get(name, [])) != 1:
            sys.exit(f'QuartzCore: {len(named.get(name, []))} symbols {name} (none: no local symbols -- the .symbols file?)')
    start, flag = named[METHOD][0], named[FLAG][0]
    end = min([value for value, _ in symbols if value > start] + [start + 0x400])
    code = cache.read(start, end - start)
    words = [(start + at, struct.unpack_from('<I', code, at)[0]) for at in range(0, len(code) - 3, 4)]
    at = {address: index for index, (address, _) in enumerate(words)}

    # The virtual test: the byte CADeviceIsVirtualized caches, loaded (adrp + ldrb of its address) and tested against 1.
    tests, loads = [], 0
    for i, (address, word) in enumerate(words):
        load = ldrb(word)
        if not load:
            continue
        pages = [adrp(w, a) for a, w in words[max(0, i - 3):i]]
        if not any(p and p[0] == load[1] and p[1] + load[2] == flag for p in pages):
            continue
        loads += 1
        for j in range(i + 1, min(i + 5, len(words))):
            test = test_branch(words, j, load[0])
            if test:
                tests.append(test)
                break
            if writes(words[j][1], load[0]):
                break
    patches = []
    if not loads:
        # The method does not ask whether the device is virtual (iPadOS 17's): the height rule alone decides.
        print(f'{METHOD}: no test of {FLAG} in this build; the height rule alone', file=sys.stderr)
    elif len(tests) != 1:
        sys.exit(f'{METHOD}: reads {FLAG} {loads} times, {len(tests)} tests of it recognised (one expected)')
    else:
        branch, target, target_is_virtual = tests[0]
        fall_through = words[branch][0] + 4
        virtual, other = (target, fall_through) if target_is_virtual else (fall_through, target)
        if virtual not in at or other not in at:
            sys.exit(f'{METHOD}: the virtual test at {words[branch][0]:#x} leaves the method')
        # The virtual case's first instruction jumps to the other case. Every path into that case goes the other way with it:
        # in 24A446 also a mode flag's (tbnz on bit 4 of the mode's byte 3), as the fixup tested on the phones since 11-03 did.
        patches.append((virtual, branch_to(virtual, other)))

    limits = [i for i, (_, word) in enumerate(words) if (c := cmp_immediate(word)) and c[0] and c[2] == HEIGHT_LIMIT]
    if len(limits) != 1:
        sys.exit(f'{METHOD}: {len(limits)} 64-bit compares with {HEIGHT_LIMIT} (one expected: the height rule)')
    if not 0 < max_height < 4096:
        sys.exit(f'--max-height {max_height}: a compare takes 1..4095 unshifted')
    limit_address, limit_word = words[limits[0]]
    patches.append((limit_address, (limit_word & ~(0xfff << 10) & ~(1 << 22)) | (max_height << 10)))

    fixups = []
    for address, replacement in patches:
        name, offset, original = cache.locate(address, 4)
        fixups.append(f'{name},{offset:x},{original.hex()},{struct.pack("<I", replacement).hex()}')
    return fixups


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('cache', type=Path)
    parser.add_argument('--max-height', type=int, default=1920, help='the tallest display, in pixels, that is @1x')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    fixups = find(Cache(args.cache), args.max_height)
    text = ''.join(line + '\n' for line in fixups)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text)
    sys.stdout.write(text)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""fileset-add-kexts.py --tree TREE --fileset IN --native KERNEL --manifest MANIFEST --output OUT

The board's own kexts into the iPadOS kernel collection: after XnuPrelink has put the native kernel into the 24A446 fileset
(kernel/kernel.macho, an MH_FILESET keeping the selected Apple kexts), each kext of MANIFEST (scripts/build-droidvm-kexts.py:
the relocatable objects of one driver and its Info.plist values) is linked against the native kernel by name, placed in the room
the dropped Apple kexts left, and added to the collection as an entry of its own: an MH_KEXT_BUNDLE header, an LC_FILESET_ENTRY
in the root header, its pointers in the root's chained fixups, its kmod_info and its _PrelinkInfoDictionary entry. XNU then loads
it as it loads the Apple kexts (OSKext::readPrelinkedExtensions), runs its C++ constructors and hands its personalities to IOKit.

Where the pieces may go is what arm_vm_init derives for a fileset (osfmk/arm64/arm_vm_init.c, kernel_mach_header_is_in_fileset):
  __TEXT        the collection's __PRELINK_TEXT
  __TEXT_EXEC   the collection's __TEXT_EXEC below the native kernel's __TEXT_EXEC (kext code precedes kernel code)
  __DATA_CONST  the collection's __DATA_CONST above the native kernel's __LASTDATA_CONST
  __DATA        the collection's __DATA above the native kernel's __PRELINK_DATA
each in whole pages of the chained-fixup page size, so no page holds anything else's pointers.

The linker is macOS-DroidVM's scripts/kc_native.py (lateautumn233), which links the same kind of objects into the macOS kernel
collection; its use here differs in one way: the iPadOS collection's Apple kexts carry no symbols, so a kext of ours resolves names
against the native kernel only (and against the kexts linked before it).
"""
import argparse
import json
import plistlib
import struct
import sys
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import kc_native  # noqa: E402

LC_SEGMENT_64, LC_SYMTAB, LC_UUID = 0x19, 0x2, 0x1b
LC_DYLD_CHAINED_FIXUPS, LC_FILESET_ENTRY = 0x80000034, 0x80000035
MASK64 = (1 << 64) - 1
KMOD_START, KMOD_STOP, KMOD_ADDRESS = 180, 188, 156
KMOD_INFO_SIZE = 196


def to_signed(value):
    return value - (1 << 64) if value >> 63 else value


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def chain_word(auth, key, diversity, address_diversity, target_offset, step):
    if not 0 <= target_offset < 1 << 30 or not 0 <= step < 1 << 12:
        raise SystemExit('chained pointer target or step out of range')
    value = (target_offset & 0x3fffffff) | ((step & 0xfff) << 51)
    if auth:
        value |= 1 << 63 | ((key & 3) << 49) | ((diversity & 0xffff) << 32) | ((address_diversity & 1) << 48)
    return value


class Collection:
    def __init__(self, data, image_class):
        self.data = bytearray(data)
        self.Image = image_class
        self.root = image_class(bytes(self.data))
        if self.root.kind != 12:
            raise SystemExit('expected an MH_FILESET collection')
        self.base = self.root.base
        self.segments = {s['name']: s for s in self.root.segments}
        self.kernel = image_class(bytes(self.data), self.root.entries['com.apple.kernel'][1])
        self.kernel_segments = {s['name']: s for s in self.kernel.segments}
        found = self.root.command(LC_DYLD_CHAINED_FIXUPS)
        dataoff, _ = struct.unpack_from('<II', self.data, found[0] + 8)
        _, starts_offset, _, _, imports, _, _ = struct.unpack_from('<7I', self.data, dataoff)
        if imports:
            raise SystemExit('chained fixup imports are not supported')
        self.table = dataoff + starts_offset
        count, = struct.unpack_from('<I', self.data, self.table)
        self.starts = {}            # root segment name -> offset of its dyld_chained_starts_in_segment
        self.page = None
        for index in range(count):
            relative, = struct.unpack_from('<I', self.data, self.table + 4 + 4 * index)
            if not relative:
                continue
            at = self.table + relative
            _, page_size, fmt, segment_offset, _, _ = struct.unpack_from('<IHHQIH', self.data, at)
            if fmt != 8:
                raise SystemExit(f'unexpected chained pointer format {fmt}')
            self.page = self.page or page_size
            if page_size != self.page:
                raise SystemExit('mixed chain page sizes')
            segment = next(s for s in self.root.segments if s['va'] - self.base == segment_offset)
            self.starts[segment['name']] = at
        # What is taken: every entry's segments (the native kernel's included) but their link edits.
        self.used = []
        for name, (_, offset) in self.root.entries.items():
            for segment in self.Image(bytes(self.data), offset).segments:
                if segment['size'] and segment['name'] != '__LINKEDIT':
                    self.used.append((segment['va'], segment['va'] + segment['size']))
        k = self.kernel_segments
        root = self.segments
        self.regions = {
            '__TEXT': (root['__PRELINK_TEXT']['va'], self.end(root['__PRELINK_TEXT'])),
            '__TEXT_EXEC': (root['__TEXT_EXEC']['va'], k['__TEXT_EXEC']['va']),
            '__DATA_CONST': (self.end(k['__LASTDATA_CONST']), self.end(root['__DATA_CONST'])),
            '__DATA': (k['__PRELINK_DATA']['va'], self.end(root['__DATA'])),
        }

    @staticmethod
    def end(segment):
        return segment['va'] + segment['size']

    def offset(self, va, length=1):
        for s in self.root.segments:
            if s['va'] <= va and va + length <= s['va'] + s['filesize']:
                return s['fileoff'] + va - s['va']
        raise SystemExit(f'unbacked address {va:#x}')

    def allocate(self, segment, size, _align):
        """Whole chain pages in the segment's region, clear of every entry: the first gap that fits."""
        low, high = self.regions[segment]
        size = align(max(size, 1), self.page)
        cursor = align(low, self.page)
        for start, end in sorted(self.used):
            if end <= cursor:
                continue
            if start >= cursor + size:
                break
            cursor = align(max(cursor, end), self.page)
        if cursor + size > high:
            raise SystemExit(f'no room for {size:#x} bytes of kext {segment} in {low:#x}-{high:#x}')
        self.offset(cursor, size)            # file-backed, or this collection cannot carry it
        self.used.append((cursor, cursor + size))
        return cursor

    def chain_page(self, va):
        """(starts table offset, page index, page VA) of the root chain page holding VA."""
        for name, at in self.starts.items():
            s = self.segments[name]
            if s['va'] <= va < s['va'] + s['size']:
                page = (va - s['va']) // self.page
                pages, = struct.unpack_from('<H', self.data, at + 20)
                if page >= pages:
                    raise SystemExit(f'{va:#x}: past the chain table of {name}')
                return at, page, s['va'] + page * self.page
        raise SystemExit(f'{va:#x}: in no root segment with chained fixups')

    def add_sites(self, sites):
        """Our pointers into the root chains: every page they are on is ours alone (allocate), so its chain is new."""
        by_page = {}
        for site, target, auth, diversity, address_diversity, key in sites:
            by_page.setdefault(self.chain_page(site), []).append((site, target, auth, diversity, address_diversity, key))
        for (at, page, page_va), chain in sorted(by_page.items()):
            start, = struct.unpack_from('<H', self.data, at + 22 + 2 * page)
            if start != 0xffff:
                raise SystemExit(f'chain page at {page_va:#x} already has pointers')
            chain.sort()
            for index, (site, target, auth, diversity, address_diversity, key) in enumerate(chain):
                if site % 4:
                    raise SystemExit(f'unaligned pointer at {site:#x}')
                step = (chain[index + 1][0] - site) // 4 if index + 1 < len(chain) else 0
                struct.pack_into('<Q', self.data, self.offset(site, 8),
                                 chain_word(auth, key, diversity, address_diversity, (target - self.base) & MASK64, step))
            struct.pack_into('<H', self.data, at + 22 + 2 * page, chain[0][0] - page_va)

    def function_auth(self):
        """How the Apple kexts sign kmod_info.start: (key, diversity, address diversity), read from their own chains."""
        fixups = self.root.fixups()
        _, info = self.prelink_info()
        for entry in info['_PrelinkInfoDictionary']:
            kmod = entry.get('_PrelinkKmodInfo', 0) & MASK64
            if not kmod or entry['CFBundleIdentifier'] == 'com.apple.kernel':
                continue
            found = fixups.get(kmod + KMOD_START)
            if found and found[1]:
                return found[2], found[3], bool(found[4])
        raise SystemExit('no Apple kext with a signed kmod_info.start to take its signing from')

    def prelink_info(self):
        segment = self.segments['__PRELINK_INFO']
        raw = bytes(self.data[segment['fileoff']:segment['fileoff'] + segment['filesize']]).split(b'\0', 1)[0]
        return segment, plistlib.loads(raw)

    def set_prelink_info(self, info):
        segment = self.segments['__PRELINK_INFO']
        xml = plistlib.dumps(info, sort_keys=False) + b'\0'
        if len(xml) > segment['filesize']:
            raise SystemExit('prelink info outgrows its segment')
        self.data[segment['fileoff']:segment['fileoff'] + segment['filesize']] = xml + bytes(segment['filesize'] - len(xml))

    def add_entry_command(self, name, vmaddr, fileoff):
        body = struct.pack('<QQI4x', vmaddr, fileoff, 32) + name.encode() + b'\0'
        length = align(8 + len(body), 8)
        command = struct.pack('<II', LC_FILESET_ENTRY, length) + body + bytes(length - 8 - len(body))
        count, size = struct.unpack_from('<II', self.data, 16)
        end = 32 + size
        first = min(s['fileoff'] for s in self.root.segments if s['filesize'] and s['fileoff'])
        text = self.segments['__TEXT']
        limit = min(first, text['fileoff'] + text['filesize'])
        if end + len(command) > limit or any(self.data[end:end + len(command)]):
            raise SystemExit('no room in the root header for another fileset entry')
        self.data[end:end + len(command)] = command
        struct.pack_into('<II', self.data, 16, count + 1, size + len(command))


def kext_header(collection, segs, linkedit, uuid_bytes):
    """MH_KEXT_BUNDLE with its four segments, the collection's shared __LINKEDIT and no symbols (macOS-DroidVM kc-build.py)."""
    prot = {'__TEXT': 1, '__TEXT_EXEC': 5, '__DATA': 3, '__DATA_CONST': 3}
    commands = []
    for name in ('__TEXT', '__TEXT_EXEC', '__DATA', '__DATA_CONST'):
        seg = segs[name]
        sections = b''
        for sect, va, size, flags in seg['sections']:
            zerofill = flags & 0xff in (1, 0xc, 0x12)
            sections += struct.pack('<16s16s2Q8I', sect.encode(), name.encode(), va, size,
                                    0 if zerofill else collection.offset(va), 4, 0, 0, flags, 0, 0, 0)
        size = len(seg['data'])
        commands.append(struct.pack('<II16s4Q4I', LC_SEGMENT_64, 72 + len(sections), name.encode(), seg['va'], size,
                                    collection.offset(seg['va'], size), size, prot[name], prot[name], len(seg['sections']), 0)
                        + sections)
    commands.append(struct.pack('<II16s4Q4I', LC_SEGMENT_64, 72, b'__LINKEDIT', linkedit['va'], linkedit['size'],
                                linkedit['fileoff'], linkedit['filesize'], 1, 1, 0, 0))
    commands.append(struct.pack('<IIIIII', LC_SYMTAB, 24, linkedit['fileoff'], 0, linkedit['fileoff'], 0))
    commands.append(struct.pack('<II16s', LC_UUID, 24, uuid_bytes))
    body = b''.join(commands)
    return struct.pack('<8I', 0xfeedfacf, 0x100000c, 0xc0000002, 11, len(commands), len(body), 0x80000085, 0) + body


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--tree', type=Path, required=True, help='the UniXNU build tree (kits/XnuPrelink)')
    p.add_argument('--fileset', type=Path, required=True)
    p.add_argument('--native', type=Path, required=True, help='the native kernel XnuPrelink took (unstripped: its symbols)')
    p.add_argument('--manifest', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--report', type=Path)
    p.add_argument('--sites', type=Path, action='append', default=[],
                   help='reviewed per-site bindings in XnuPrelink\'s fixture format (fixtures/fileset-*-symbols.json): one original '
                        'pointer site of the collection bound to a native symbol, applied when the fixture names this '
                        'collection\'s kernelcache')
    p.add_argument('--record', type=Path, help='XnuPrelink\'s record of the collection (default: the fileset\'s .json): its '
                   'kernelcache and slide')
    args = p.parse_args()
    sys.path.insert(0, str(args.tree / 'kits/XnuPrelink'))
    from kc_image import Image

    collection = Collection(args.fileset.read_bytes(), Image)
    native = Image(args.native.read_bytes())
    # The native kernel's symbols where the collection put them: by segment, as XnuPrelink moved each segment whole.
    moved = {s['name']: collection.kernel_segments[s['name']]['va'] - s['va']
             for s in native.segments if s['name'] in collection.kernel_segments}
    kernel_symbols = {}
    for address, names in native.symbols().items():
        segment = next((s for s in native.segments if s['size'] and s['va'] <= address < s['va'] + s['size']), None)
        if segment is None or segment['name'] not in moved:
            continue
        for name in names:
            kernel_symbols.setdefault(name, []).append(address + moved[segment['name']])
    linked_exports = {}

    def resolve(name):
        found = kernel_symbols.get(name, [])
        if len(found) == 1:
            return found[0]
        found = linked_exports.get(name, [])
        return found[0] if len(found) == 1 else None

    # Reviewed site bindings for a kernelcache XnuPrelink has no fixture of (as its fixtures/, kept by the board): an original
    # component's pointer slot that must name another native object than the one its target pairs with (AMFI's PAGE_SHIFT_CONST
    # GOT slot, bound to unixnu_kernel_stack_shift as in XnuPrelink's 24A446 and 26C5035r fixtures). The chained pointer keeps its
    # next and signing fields; its target becomes the symbol's.
    bound = []
    if args.sites:
        record = json.loads((args.record or args.fileset.with_suffix('.json')).read_text())
        slide = int(record['collection_slide'], 16)
        for path in args.sites:
            fixture = json.loads(path.read_text())
            if fixture['inputs']['kernelcache_sha256'] != record['inputs']['kernelcache_sha256']:
                continue
            for entry in fixture['sites']:
                if not entry.get('evidence'):
                    raise SystemExit(f'{path}: a reviewed site needs its evidence')
                site = (int(entry['site'], 16) + slide) & MASK64
                found = kernel_symbols.get(entry['native_symbol'], [])
                if len(found) != 1:
                    raise SystemExit(f'{path}: {entry["native_symbol"]} is not one native symbol')
                at = collection.offset(site, 8)
                word, = struct.unpack_from('<Q', collection.data, at)
                target = found[0] - collection.base
                if not 0 <= target < 1 << 30:
                    raise SystemExit(f'{entry["native_symbol"]} lies outside the collection')
                struct.pack_into('<Q', collection.data, at, (word & ~0x3fffffff) | target)
                bound.append({'site': hex(site), 'native_symbol': entry['native_symbol'], 'was': hex(word & 0x3fffffff),
                              'fixture': path.name})
                print(f'site {site:#x}: {entry["native_symbol"]}')
    key, diversity, address_diversity = collection.function_auth()
    linkedit = collection.segments['__LINKEDIT']
    _, info = collection.prelink_info()
    present = {e['CFBundleIdentifier'] for e in info['_PrelinkInfoDictionary']}
    manifest = json.loads(args.manifest.read_text())
    report = []
    for spec in manifest['kexts']:
        if spec['bundle'] in present:
            raise SystemExit(f'{spec["bundle"]}: already in the collection')
        missing = [library for library in spec['libraries'] if library not in present]
        if missing:
            raise SystemExit(f'{spec["bundle"]}: libraries not in the collection: {missing}')
        objects = [str(args.manifest.parent / o) for o in spec['objects']]
        try:
            linked = kc_native.link(objects, resolve, collection.allocate, 0x1000, KMOD_INFO_SIZE + 4,
                                    spec['start'], spec['stop'])
        except kc_native.LinkError as problem:
            raise SystemExit(f'{spec["bundle"]}: {problem}')
        segs = linked['segments']
        for seg in segs.values():
            at = collection.offset(seg['va'], len(seg['data']))
            collection.data[at:at + len(seg['data'])] = seg['data']
        text_va, data_va = segs['__TEXT']['va'], segs['__DATA']['va']
        kmod = (struct.pack('<QiI', 0, 1, 0xffffffff) + spec['bundle'].encode().ljust(64, b'\0')
                + spec['version'].encode().ljust(64, b'\0')
                + struct.pack('<iQQQQ', -1, 0, text_va, len(segs['__TEXT']['data']), 0) + bytes(16))
        assert len(kmod) == KMOD_INFO_SIZE
        at = collection.offset(data_va, len(kmod))
        collection.data[at:at + len(kmod)] = kmod
        sites = list(linked['sites'])
        for field, symbol in ((KMOD_START, spec['start']), (KMOD_STOP, spec['stop'])):
            sites.append((data_va + field, linked['symbols'][symbol], True, diversity, address_diversity, key))
        collection.add_sites(sites)
        uuid_bytes = uuid.uuid5(uuid.NAMESPACE_URL, spec['bundle'] + spec['version']).bytes
        header = kext_header(collection, segs, linkedit, uuid_bytes)
        if len(header) > 0x1000:
            raise SystemExit(f'{spec["bundle"]}: header larger than its reserve')
        at = collection.offset(text_va, len(header))
        collection.data[at:at + len(header)] = header
        collection.add_entry_command(spec['bundle'], text_va, collection.offset(text_va))
        entry = {
            'CFBundleIdentifier': spec['bundle'], 'CFBundleName': spec['name'], 'CFBundleExecutable': spec['name'],
            'CFBundleVersion': spec['version'], 'CFBundleShortVersionString': spec['version'],
            'CFBundlePackageType': 'KEXT', 'CFBundleInfoDictionaryVersion': '6.0', 'CFBundleDevelopmentRegion': 'en',
            'OSBundleCompatibleVersion': spec['version'], 'OSBundleRequired': 'Root',
            'OSBundleLibraries': spec['libraries'], 'IOKitPersonalities': spec['personalities'],
            'OSBundleUUID': uuid_bytes,
            '_PrelinkBundlePath': f'/System/Library/Extensions/{spec["name"]}.kext',
            '_PrelinkExecutableRelativePath': spec['name'],
            '_PrelinkExecutableLoadAddr': to_signed(text_va), '_PrelinkExecutableSourceAddr': to_signed(text_va),
            '_PrelinkExecutableSize': len(segs['__TEXT']['data']), '_PrelinkKmodInfo': to_signed(data_va),
        }
        info['_PrelinkInfoDictionary'].append(entry)
        present.add(spec['bundle'])
        for name, va in linked['symbols'].items():
            linked_exports.setdefault(name, []).append(va)
        # The kext's symbols are in no symbol table of the collection: listed here for the host rig (peek.py reads a driver's
        # state variables by name).
        report.append({'bundle': spec['bundle'], 'header': hex(text_va), 'kmod_info': hex(data_va), 'pointers': len(sites),
                       'segments': {n: [hex(s['va']), hex(len(s['data']))] for n, s in segs.items()},
                       'symbols': {n: hex(va) for n, va in sorted(linked['symbols'].items())}})
        print(f'{spec["bundle"]}: header {text_va:#x}, kmod_info {data_va:#x}, {len(sites)} pointers')
    collection.set_prelink_info(info)
    args.output.write_bytes(bytes(collection.data))
    if args.report:
        args.report.write_text(json.dumps({'kexts': report, 'sites': bound, 'kmod_start_signing': {
            'key': key, 'diversity': hex(diversity), 'address_diversity': address_diversity}}, indent=2) + '\n')


if __name__ == '__main__':
    main()

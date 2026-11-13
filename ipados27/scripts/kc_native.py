# From Droid-VM/macOS-DroidVM scripts/kc_native.py (lateautumn233, 1edf517): the static linker of the native kexts of its macOS
# kernel collection; fileset-add-kexts.py links the iPadOS board's kexts with it. One change: __DATA_CONST,__assert is placed
# (XNU's assert descriptors, which code built with the kernel's own flags refers to: Apple's IONetworkingFamily). Its
# __BOOTDATA,__static_if entries stay out: XNU rewrites only the kernel's own static branches at boot, a kext's keep what they
# were compiled with (mach_assert on, as on this DEVELOPMENT kernel). And absolute symbols (N_ABS, `.set NAME, ADDRESS`) are
# defined at their value: the board's bindings to classes of the stripped iPadOS fileset name the original objects that way
# (SoftwareAES-24A446.S: IOAESAccelerator's metaclass, reached through a GOT entry that keeps the value).
"""A small static linker: relocatable arm64e objects of a driver -> the four segments of a kext inside a fileset kernel collection.

The driver refers to classes of kexts of the collection (it derives from IOBlockStorageDevice), so it cannot be linked into the kernel
image. Its undefined symbols are resolved by name against the kernel and the collection's kexts; pointers become chained fixups
(kc-build.py encodes them), instructions are patched here. HEADER_RESERVE bytes at the start of __TEXT are left for the Mach-O header, DATA_RESERVE at the start of __DATA for kmod_info.
Handles the relocation kinds clang emits for kernel code: BRANCH26, PAGE21,
PAGEOFF12, GOT_LOAD_PAGE21/PAGEOFF12, ADDEND, UNSIGNED and AUTHENTICATED_POINTER.
"""
import struct

UNSIGNED, SUBTRACTOR, BRANCH26, PAGE21, PAGEOFF12, GOT_PAGE21, GOT_PAGEOFF12, POINTER_TO_GOT, TLVP_PAGE21, TLVP_PAGEOFF12, ADDEND, AUTH = range(12)

# (segment, section) of the object -> (segment, section) in the kext; anything else (debug info, unwind) is dropped
PLACEMENT = {
    ('__TEXT', '__text'): ('__TEXT_EXEC', '__text'),
    ('__TEXT', '__cstring'): ('__TEXT', '__cstring'),
    ('__TEXT', '__const'): ('__TEXT', '__const'),
    ('__TEXT', '__literal4'): ('__TEXT', '__literal4'),
    ('__TEXT', '__literal8'): ('__TEXT', '__literal8'),
    ('__TEXT', '__literal16'): ('__TEXT', '__literal16'),
    ('__DATA', '__const'): ('__DATA_CONST', '__const'),
    ('__DATA_CONST', '__const'): ('__DATA_CONST', '__const'),
    ('__DATA', '__auth_ptr'): ('__DATA_CONST', '__auth_ptr'),
    ('__DATA', '__kalloc_type'): ('__DATA_CONST', '__kalloc_type'),
    ('__DATA_CONST', '__kalloc_type'): ('__DATA_CONST', '__kalloc_type'),
    ('__DATA', '__kalloc_var'): ('__DATA_CONST', '__kalloc_var'),
    ('__DATA_CONST', '__kalloc_var'): ('__DATA_CONST', '__kalloc_var'),
    ('__DATA_CONST', '__assert'): ('__DATA_CONST', '__assert'),
    ('__DATA', '__mod_init_func'): ('__DATA_CONST', '__mod_init_func'),
    ('__DATA', '__mod_term_func'): ('__DATA_CONST', '__mod_term_func'),
    ('__DATA', '__data'): ('__DATA', '__data'),
    ('__DATA', '__bss'): ('__DATA', '__bss'),
    ('__DATA', '__common'): ('__DATA', '__common'),
}
SEGMENT_ORDER = ['__TEXT', '__TEXT_EXEC', '__DATA_CONST', '__DATA']


class LinkError(Exception):
    pass


def cstr(raw):
    return raw.split(b'\0')[0].decode()


class Object:
    def __init__(self, path):
        self.path = path
        data = self.data = bytearray(open(path, 'rb').read())
        magic, cpu, sub, kind, count, size, flags, _ = struct.unpack_from('<8I', data)
        if (magic, cpu, kind) != (0xfeedfacf, 0x100000c, 1):
            raise LinkError(f'{path}: not an arm64 object')
        self.sections, self.symbols = [], []
        offset = 32
        for _ in range(count):
            cmd, length = struct.unpack_from('<II', data, offset)
            if cmd == 0x19:
                for index in range(struct.unpack_from('<I', data, offset + 64)[0]):
                    name, seg, addr, size_, off, align, reloff, nreloc, sflags = struct.unpack_from('<16s16s2Q7I', data, offset + 72 + index * 80)[:9]
                    self.sections.append({'name': cstr(name), 'segment': cstr(seg), 'addr': addr, 'size': size_, 'offset': off, 'align': align,
                                          'reloff': reloff, 'nreloc': nreloc, 'flags': sflags, 'index': len(self.sections) + 1})
            elif cmd == 2:
                symoff, nsyms, stroff, strsize = struct.unpack_from('<4I', data, offset + 8)
                strings = bytes(data[stroff:stroff + strsize])
                for i in range(nsyms):
                    strx, kind_, sect, desc, value = struct.unpack_from('<IBBHQ', data, symoff + i * 16)
                    self.symbols.append({'name': strings[strx:strings.index(b'\0', strx)].decode(), 'type': kind_, 'sect': sect, 'value': value})
            offset += length

    def placed(self):
        return [s for s in self.sections if (s['segment'], s['name']) in PLACEMENT]


def link(objects, resolve, allocate, header_reserve, data_reserve, start_symbol, stop_symbol):
    """OBJECTS: paths. RESOLVE(name) -> address or None (kernel and kext symbols, new addresses). ALLOCATE(segment, size, align) -> base address.
    Returns {'segments': {name: (va, bytearray, [(section, va, size)])}, 'sites': [(site_va, target_va, auth, diversity, addr_div, key)],
    'symbols': {name: va}, 'unresolved': [...]}."""
    objs = [Object(p) for p in objects]
    # 1. what goes where
    layout = {seg: [] for seg in SEGMENT_ORDER}               # segment -> [(section name, [(obj, section)])]
    for obj in objs:
        for sect in obj.placed():
            seg, name = PLACEMENT[(sect['segment'], sect['name'])]
            entries = layout[seg]
            slot = next((e for e in entries if e[0] == name), None)
            if slot is None:
                slot = (name, [])
                entries.append(slot)
            slot[1].append((obj, sect))
    # GOT slots: one 8-byte slot per symbol that a GOT-relocation names (collected below), kept in DATA_CONST/__got
    got_names = []
    for obj in objs:
        for sect in obj.placed():
            for i in range(sect['nreloc']):
                a, w = struct.unpack_from('<II', obj.data, sect['reloff'] + i * 8)
                if w >> 28 in (GOT_PAGE21, GOT_PAGEOFF12, POINTER_TO_GOT):
                    name = obj.symbols[w & 0xffffff]['name']
                    if name not in got_names:
                        got_names.append(name)
    # 2. sizes and addresses
    placement = {}                                              # (obj id, section index) -> address
    segments = {}
    for seg in SEGMENT_ORDER:
        cursor = header_reserve if seg == '__TEXT' else data_reserve if seg == '__DATA' else 0
        order, spans = [], []
        for name, members in layout[seg]:
            start = None
            for obj, sect in members:
                align = 1 << sect['align']
                cursor = (cursor + align - 1) // align * align
                order.append((obj, sect, cursor))
                if start is None:
                    start = cursor
                cursor += sect['size']
            # one section per name: OSRuntimeInitializeCPP finds __mod_init_func by name and runs the first one only
            spans.append((name, start, cursor - start, members[0][1]['flags']))
        got_at = None
        if seg == '__DATA_CONST' and got_names:
            cursor = (cursor + 7) // 8 * 8
            got_at = cursor
            cursor += 8 * len(got_names)
        # common symbols (undefined external with a size) live in DATA
        commons = []
        if seg == '__DATA':
            for obj in objs:
                for symbol in obj.symbols:
                    if symbol['type'] & 0x0e == 0 and symbol['type'] & 1 and symbol['value']:
                        commons.append((obj, symbol))
            for obj, symbol in commons:
                cursor = (cursor + 15) // 16 * 16
                symbol['common_offset'] = cursor
                cursor += symbol['value']
        size = (cursor + 15) // 16 * 16
        base = allocate(seg, size, 16)
        segments[seg] = {'va': base, 'data': bytearray(size), 'sections': [], 'got_at': got_at}
        for obj, sect, at in order:
            placement[(id(obj), sect['index'])] = base + at
            if not sect['flags'] & 0xff in (1, 0xc, 0x12):
                segments[seg]['data'][at:at + sect['size']] = obj.data[sect['offset']:sect['offset'] + sect['size']]
        segments[seg]['sections'] = [(name, base + start, size, flags) for name, start, size, flags in spans if size]
        for obj, symbol in commons:
            symbol['address'] = base + symbol['common_offset']
        if got_at is not None:
            segments[seg]['sections'].append(('__got', base + got_at, 8 * len(got_names), 6))
    got_address = {name: segments['__DATA_CONST']['va'] + segments['__DATA_CONST']['got_at'] + 8 * i for i, name in enumerate(got_names)}
    # 3. symbols
    defined = {}
    for obj in objs:
        for symbol in obj.symbols:
            if symbol['type'] & 0x0e == 0x02 and symbol['type'] & 1 and not symbol['type'] & 0xe0:   # N_ABS, external
                defined[symbol['name']] = symbol['value']
            elif symbol['type'] & 0x0e == 0x0e and symbol['type'] & 1 and not symbol['type'] & 0xe0:
                sect = obj.sections[symbol['sect'] - 1]
                if (id(obj), sect['index']) in placement:
                    defined[symbol['name']] = placement[(id(obj), sect['index'])] + symbol['value'] - sect['addr']
            elif 'address' in symbol:
                defined[symbol['name']] = symbol['address']

    def address_of(obj, index):
        symbol = obj.symbols[index]
        if symbol['type'] & 0x0e == 0x02 and not symbol['type'] & 0xe0:   # N_ABS
            return symbol['value']
        if symbol['type'] & 0x0e == 0x0e:
            sect = obj.sections[symbol['sect'] - 1]
            key = (id(obj), sect['index'])
            if key not in placement:
                raise LinkError(f'{symbol["name"]} lives in a dropped section')
            return placement[key] + symbol['value'] - sect['addr']
        if 'address' in symbol:
            return symbol['address']
        name = symbol['name']
        if name in defined:
            return defined[name]
        found = resolve(name)
        if found is None:
            raise LinkError(f'unresolved symbol {name}')
        return found

    # 4. relocations
    sites = []
    for obj in objs:
        for sect in obj.placed():
            seg, name = PLACEMENT[(sect['segment'], sect['name'])]
            info = segments[seg]
            base = placement[(id(obj), sect['index'])]
            buf, origin = info['data'], base - info['va']
            addend = 0
            for i in range(sect['nreloc']):
                address, word = struct.unpack_from('<II', obj.data, sect['reloff'] + i * 8)
                symbolnum, pcrel, length, extern, kind = word & 0xffffff, word >> 24 & 1, word >> 25 & 3, word >> 27 & 1, word >> 28
                if kind == ADDEND:
                    addend = symbolnum - (1 << 24) if symbolnum >> 23 else symbolnum
                    continue
                if not extern:
                    raise LinkError(f'{obj.path}: local (non-extern) relocation in {name}')
                at = origin + address
                pc = base + address
                if kind == BRANCH26:
                    delta = address_of(obj, symbolnum) + addend - pc
                    if delta & 3 or not -(1 << 27) <= delta < (1 << 27):
                        raise LinkError(f'branch at {pc:#x} out of range')
                    word_ = struct.unpack_from('<I', buf, at)[0]
                    struct.pack_into('<I', buf, at, word_ & 0xfc000000 | (delta >> 2) & 0x3ffffff)
                elif kind in (PAGE21, GOT_PAGE21):
                    target = got_address[obj.symbols[symbolnum]['name']] if kind == GOT_PAGE21 else address_of(obj, symbolnum) + addend
                    imm = ((target >> 12) - (pc >> 12))
                    if not -(1 << 20) <= imm < (1 << 20):
                        raise LinkError(f'adrp at {pc:#x} out of range')
                    word_ = struct.unpack_from('<I', buf, at)[0]
                    struct.pack_into('<I', buf, at, word_ & 0x9f00001f | (imm & 3) << 29 | (imm >> 2 & 0x7ffff) << 5)
                elif kind in (PAGEOFF12, GOT_PAGEOFF12):
                    target = got_address[obj.symbols[symbolnum]['name']] if kind == GOT_PAGEOFF12 else address_of(obj, symbolnum) + addend
                    low = target & 0xfff
                    word_ = struct.unpack_from('<I', buf, at)[0]
                    if word_ & 0x3b000000 == 0x39000000:                # ldr/str unsigned immediate
                        size_, vector, opc = word_ >> 30, word_ >> 26 & 1, word_ >> 22 & 3
                        shift = (4 if size_ == 0 and opc >= 2 else size_) if vector else size_
                        if low & ((1 << shift) - 1):
                            raise LinkError(f'pageoff {low:#x} not aligned for the access at {pc:#x}')
                        low >>= shift
                    elif word_ & 0x7f800000 != 0x11000000:
                        raise LinkError(f'unexpected pageoff instruction {word_:#x} at {pc:#x}')
                    struct.pack_into('<I', buf, at, word_ & ~(0xfff << 10) | low << 10)
                elif kind == UNSIGNED and length == 3:
                    value, = struct.unpack_from('<Q', buf, at)
                    target = address_of(obj, symbolnum) + value
                    sites.append((pc, target, False, 0, False, 0))
                    struct.pack_into('<Q', buf, at, 0)
                elif kind == AUTH:
                    value, = struct.unpack_from('<Q', buf, at)
                    plus = value & 0xffffffff
                    plus = plus - (1 << 32) if plus >> 31 else plus
                    sites.append((pc, address_of(obj, symbolnum) + plus, True, value >> 32 & 0xffff, bool(value >> 48 & 1), value >> 49 & 3))
                    struct.pack_into('<Q', buf, at, 0)
                else:
                    raise LinkError(f'{obj.path}: unsupported relocation type {kind} in {name}')
                addend = 0
    for name, at in got_address.items():
        # find the symbol again through any object that names it
        target = None
        for obj in objs:
            for index, symbol in enumerate(obj.symbols):
                if symbol['name'] == name:
                    try:
                        target = address_of(obj, index)
                    except LinkError:
                        continue
                    break
            if target is not None:
                break
        if target is None:
            raise LinkError(f'unresolved GOT symbol {name}')
        sites.append((at, target, False, 0, False, 0))
    for need in (start_symbol, stop_symbol):
        if need not in defined:
            raise LinkError(f'the kext does not define {need}')
    return {'segments': segments, 'sites': sites, 'symbols': defined}

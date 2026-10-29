#!/usr/bin/env python3
"""The touch screen, keyboard and motion sensors of the iPadOS 27 board: NativeHidInput under the original IOHIDDevice of the 24A446
fileset, and the virtio-input transport that feeds it, for the display archive (build-droidvm-legacy-display.py).

As the iPadOS 17 board builds them (scripts/build-native-legacy-display.py): ShengHidInput's NativeHidInput.cpp with the board's
keyboard sink and its motion sensors (kits/droidvm/DroidVMMotionHid.inc) in a staged copy, the board probe
(kits/droidvm/HidInputProbe.cpp), the virtio-input transport (DroidVMVirtioInput.c) and the motion sysctl (DroidVMMotion.c). The
HID source is UniXNU-sheng's for iPadOS 27 (ShengHidInput 24f1f3f: 5f3a37c, the iPadOS 17 board's, and its arm64e edge-contact
work), compiled against the public IOHIDFamily headers of the OS profile's pin (refer/IOHIDFamily; NATIVE_HID_SOURCE_API), so every
IOHIDDevice virtual keeps its real name and pointer-authentication discriminator, as sheng's build-gop-legacy-display-27.py does.

The original class comes from the reviewed fixture hid-device-24A446.json (sheng's, for the same kernelcache: its 207 slot targets
and diversities, instance size and bindings). Each IOHIDDevice method the objects import is bound to the original entry of its slot
(the slot it fills in an owned vtable, or else the one slot whose diversity its name has); every owned vtable entry must carry the
original slot's diversity. An original function is reached through an owned stub that branches through an absolute literal (as the
board's display and accelerator stubs); the metaclass and the vtable are data, at their plain addresses.

Writes build/droidvm-hid-input/objects.json and sources.json (paths relative to the tree), which the display archive takes, and
report.json.
"""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
from kernel_target import ARCH, IOS_VERSION, TRIPLE
if ARCH != 'arm64e' or IOS_VERSION.split('.')[0] != '27':
    raise SystemExit('This HID builder only supports arm64e iPadOS 27 (24A446).')
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/droidvm-hid-input'
OUT.mkdir(parents=True, exist_ok=True)
HERE = Path(__file__).resolve().parent
# UniXNU-sheng's pin for iPadOS 27 (drivers/ShengHidInput of its feat/ipados27-gpu-parity 6ed0ba4).
HID_REPOSITORY = 'https://github.com/UniXNU/ShengHidInput.git'
HID_COMMIT = '24f1f3fbed6feff42e1ca02e2ce386a5c3c36da8'
RECORDS = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=300, **kw)


def require(v, why):
    if not v:
        raise SystemExit(why)


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def load_discriminator():
    """The display builder's clang ptrauth_string_discriminator, one implementation for the board."""
    text = (HERE / 'build-droidvm-legacy-display.py').read_text()
    start = text.index('def discriminator(name):')
    namespace = {'struct': struct}
    exec(text[start:text.index('\n\n\n', start)], namespace)
    return namespace['discriminator']


def hid_source():
    """ShengHidInput at the pin, cloned once into the tree's source cache."""
    checkout = ROOT / '.cache/sources/ShengHidInput'
    if not (checkout / '.git').is_dir():
        checkout.parent.mkdir(parents=True, exist_ok=True)
        run(['git', 'clone', '-q', '--filter=blob:none', '--no-checkout', HID_REPOSITORY, checkout])
    if subprocess.run(['git', '-C', checkout, 'cat-file', '-e', HID_COMMIT], capture_output=True).returncode:
        run(['git', '-C', checkout, 'fetch', '-q', 'origin', HID_COMMIT])
    if run(['git', '-C', checkout, 'rev-parse', 'HEAD'], text=True).strip() != HID_COMMIT:
        run(['git', '-C', checkout, 'checkout', '-q', '-f', '--detach', HID_COMMIT])
    require(not run(['git', '-C', checkout, 'status', '--porcelain'], text=True), f'{checkout}: must be clean')
    return checkout / 'NativeHidInput.cpp'


def record(name):
    return json.loads((RECORDS / name).read_text().strip().rstrip(','))


def command(rec, src, dst, extra=()):
    """A kernel compile record with another source and output, its warnings replaced by the board's."""
    cmd, args, i = [], rec['arguments'], 0
    while i < len(args):
        value = args[i]
        if value == rec['file']:
            cmd.append(str(src))
        elif value in ('-o', '-MF', '-MT', '-MQ', '-MJ'):
            if value == '-o':
                cmd += ['-o', str(dst)]
            i += 1
        elif value in ('-MD', '-MMD', '-MP'):
            pass
        elif value == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W'):
            cmd.append(value)
        i += 1
    cmd += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter']
    if Path(src).suffix != '.c':
        cmd += ['-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier']
    cmd += ['-mgeneral-regs-only', '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(ROOT / 'kits/droidvm'), *extra]
    run(cmd, cwd=rec['directory'])


def stage_hid(original):
    """NativeHidInput.cpp with the iPadOS 17 board's additions (build-native-legacy-display.py): report 1, the boot keyboard its
    descriptor declares, from the board's keyboard sink (DroidVMVirtioInput.c), and the motion sensors in the same translation
    unit, whose late static initializer the probe runs."""
    text = original.read_text()
    keyboard = ('    // DroidVM (build-droidvm-hid-input.py): the keyboard driver calls keyboardNotify with each boot keyboard state (modifier\n'
                '    // byte, reserved byte, six usages), sent as report 1.\n'
                '    static void keyboardNotify(void *context,const uint8_t *keys) {\n'
                '        NativeHidInput *self=static_cast<NativeHidInput *>(context);\n'
                '        if(self && self->inputLoop && keys)self->inputLoop->runAction(&NativeHidInput::keyboardAction,self,const_cast<uint8_t *>(keys));\n'
                '    }\n'
                '    static IOReturn keyboardAction(OSObject *owner,void *keys,void *,void *,void *) {\n'
                '        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return kIOReturnSuccess;\n'
                '        if(!self->hasEventDriverCached())return kIOReturnNotReady;\n'
                '        unsigned char report[9]={1};\n'
                '        memcpy(report+1,keys,8);\n'
                '        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);\n'
                '        IOReturn result=buffer?self->handleReport(buffer,kIOHIDReportTypeInput,0):kIOReturnNoMemory;\n'
                '        OSSafeReleaseNULL(buffer);\n'
                '        return result;\n'
                '    }\n')
    for anchor, replacement in (
            ('extern "C" void native_buttons_set_sink(void (*)(void *, uint32_t), void *) __attribute__((weak_import));\n',
             'extern "C" void native_buttons_set_sink(void (*)(void *, uint32_t), void *) __attribute__((weak_import));\n'
             'extern "C" void native_keyboard_set_sink(void (*)(void *, const uint8_t *), void *);\n'),
            ('    bool hasEventDriverCached() {\n', keyboard + '    bool hasEventDriverCached() {\n'),
            ('        if(!touchOnly && &native_buttons_set_sink)native_buttons_set_sink(&NativeHidInput::buttonsNotify,this);\n',
             '        if(!touchOnly && &native_buttons_set_sink)native_buttons_set_sink(&NativeHidInput::buttonsNotify,this);\n'
             '        if(!touchOnly)native_keyboard_set_sink(&NativeHidInput::keyboardNotify,this);\n'),
            ('        if(!touchOnly && &native_buttons_set_sink)native_buttons_set_sink(nullptr,nullptr);\n',
             '        if(!touchOnly && &native_buttons_set_sink)native_buttons_set_sink(nullptr,nullptr);\n'
             '        if(!touchOnly)native_keyboard_set_sink(nullptr,nullptr);\n')):
        require(text.count(anchor) == 1, 'NativeHidInput.cpp no longer has exactly one ' + anchor.strip())
        text = text.replace(anchor, replacement)
    text += '\n' + (ROOT / 'kits/droidvm/DroidVMMotionHid.inc').read_text()
    # the same file name: the late static initializer is named after it (__GLOBAL__sub_I_NativeHidInput.cpp)
    staged = OUT / 'droidvm/NativeHidInput.cpp'
    staged.parent.mkdir(exist_ok=True)
    staged.write_text(text)
    return staged


def macho(data):
    """Load commands, segments and sections of an arm64e relocatable object."""
    magic, cpu, _, _, count = struct.unpack_from('<5I', data)
    require(magic == 0xfeedfacf and cpu == 0x100000c, 'arm64 Mach-O object')
    commands, segments, offset = [], [], 32
    for _ in range(count):
        cmd, size = struct.unpack_from('<2I', data, offset)
        commands.append((cmd, offset, size))
        if cmd == 0x19:
            nsects = struct.unpack_from('<I', data, offset + 64)[0]
            sections = []
            for i in range(nsects):
                at = offset + 72 + 80 * i
                va, length, fileoff = struct.unpack_from('<QQI', data, at + 32)
                sections.append({'va': va, 'size': length, 'fileoff': fileoff, 'command_offset': at})
            segments.append({'sections': sections})
        offset += size
    return {'commands': commands, 'segments': segments}


def object_symbols(data):
    st = next(p for c, p, _ in macho(data)['commands'] if c == 2)
    sy, count, strings, _ = struct.unpack_from('<4I', data, st + 8)
    found = []
    for i in range(count):
        ix, kind, _, _, va = struct.unpack_from('<IBBHQ', data, sy + i * 16)
        found.append((data[strings + ix:data.index(b'\0', strings + ix)].decode(), va, kind))
    return found


def object_vtable(data, found, name, count):
    """[(symbol, signed word)] of the first count entries of vtable name (authenticated pointer relocations)."""
    va = next(v for n, v, _ in found if n == name)
    entries = {}
    for segment in macho(data)['segments']:
        for sec in segment['sections']:
            if sec['va'] <= va < sec['va'] + sec['size']:
                ro, rc = struct.unpack_from('<2I', data, sec['command_offset'] + 56)
                for i in range(rc):
                    offset, bits = struct.unpack_from('<2I', data, ro + i * 8)
                    # extern, 8 bytes, ARM64_RELOC_AUTHENTICATED_POINTER
                    if bits >> 27 & 1 and bits >> 25 & 3 == 3 and bits >> 28 == 11:
                        entries[sec['va'] + offset] = (found[bits & 0xffffff][0],
                                                       struct.unpack_from('<Q', data, sec['fileoff'] + offset)[0])
    return [entries.get(va + 16 + i * 8) for i in range(count)]


def main():
    discriminator = load_discriminator()
    abi = json.loads((HERE / 'hid-device-24A446.json').read_text())
    legacy = json.loads((HERE / 'legacy-display-24A446.json').read_text())
    require(abi['kernelcache_sha256'] == legacy['kernelcache_sha256'], 'Both fixtures describe the same kernelcache')
    slide = int(abi['collection_slide'], 16)
    slots = [(int(t, 16), int(d, 16)) for t, d in abi['slots']]
    # __cxa_pure_virtual: the display fixture's abstract slot 214.
    pure_target = int(legacy['slots'][214][0], 16)
    pure = {i for i, (t, _) in enumerate(slots) if t == pure_target}
    require(len(slots) == 207 and pure == {180} and abi['instance_size'] == '0xd0',
            'The original IOHIDDevice: 207 slots, newReportDescriptor (180) abstract, 0xd0 bytes')

    # The public IOHIDFamily headers of the pinned source, after the kernel's own; the macOS SDK's Kernel.framework supplies the
    # generated HIDDriverKit declarations the source checkout lacks.
    family = ROOT / 'refer/IOHIDFamily'
    family_commit = run(['git', '-C', family, 'rev-parse', 'HEAD'], text=True).strip()
    include = OUT / 'headers/include'
    if include.exists():
        shutil.rmtree(include)
    for source_dir, relative in ((family / 'IOHIDFamily', 'IOKit/hid'), (family / 'IOHIDSystem/IOKit/hidsystem', 'IOKit/hidsystem')):
        destination = include / relative
        destination.mkdir(parents=True, exist_ok=True)
        for path in sorted(source_dir.glob('*.h')):
            shutil.copyfile(path, destination / path.name)
    sdk = Path(run(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
    hid_includes = ('-I', str(include), '-idirafter', str(sdk / 'System/Library/Frameworks/Kernel.framework/Versions/A/Headers'))

    cpp, c_mach, c_bsd = record('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'), record('osfmk/DEVELOPMENT/startup.o.json'), \
        record('bsd/DEVELOPMENT/kern_exec.o.json')
    original = hid_source()
    staged = stage_hid(original)
    hid_obj = OUT / 'NativeHidInput.o'
    command(cpp, staged, hid_obj, (*hid_includes, '-DNATIVE_HID_SOURCE_API=1'))
    probe = ROOT / 'kits/droidvm/HidInputProbe.cpp'
    command(cpp, probe, OUT / 'HidInputProbe.o')
    transport = ROOT / 'kits/droidvm/DroidVMVirtioInput.c'
    # Its queue and event pages come from the kernel map (DROIDVM_INPUT_DYNAMIC): the native __DATA must fit the original kernel's slot.
    command(c_mach, transport, OUT / 'DroidVMVirtioInput.o', ('-DDROIDVM_INPUT_DYNAMIC=1',))
    motion = ROOT / 'kits/droidvm/DroidVMMotion.c'
    command(c_bsd, motion, OUT / 'DroidVMMotion.o')

    raw = hid_obj.read_bytes()
    found = object_symbols(raw)
    bindings = {n: int(v, 16) + slide for n, v in abi['bindings'].items()}
    by_diversity = {}
    for i, (_, d) in enumerate(slots):
        by_diversity.setdefault(d, []).append(i)
    vtables = [n for n, _, k in found if n.startswith('__ZTV') and k != 1 and '9MetaClassE' not in n]
    require(set(vtables) == {'__ZTV14NativeHidInput', '__ZTV16DroidVMMotionHid'}, f'The owned HID classes: {vtables}')
    # An original method in an owned vtable is bound to the original entry of that slot (a base-class override signs with the
    # discriminator of the introducing IOService declaration, so its own name does not identify the slot); any other import by
    # the slot whose discriminator its name has.
    slot_of = {}
    for vt_name in vtables:
        for i, entry in enumerate(object_vtable(raw, found, vt_name, len(slots))):
            if entry and '11IOHIDDevice' in entry[0]:
                require(slot_of.setdefault(entry[0], i) == i, f'{entry[0]}: one slot in every owned vtable')
    for name in sorted({n for n, _, k in found if k == 1 and '11IOHIDDevice' in n} - set(bindings)):
        if name in slot_of:
            slot = slot_of[name]
        else:
            matches = by_diversity.get(discriminator(name[1:]), [])
            require(len(matches) == 1, f'{name}: one original IOHIDDevice slot with its discriminator')
            slot = matches[0]
        require(slot not in pure, f'{name}: not an abstract original slot')
        bindings[name] = slots[slot][0] + slide
    owned = {}
    for vt_name in vtables:
        entries = object_vtable(raw, found, vt_name, len(slots))
        require(all(entries), f'{vt_name}: at least the {len(slots)} IOHIDDevice entries')
        for i, (name, word) in enumerate(entries):
            require(word >> 63 and word >> 48 & 1 and word >> 49 & 3 == 0 and (word >> 32) & 0xffff == slots[i][1],
                    f'{vt_name} slot {i} ({name}) signed with the original diversity {slots[i][1]:#x}')
            if '11IOHIDDevice' in name:
                require(bindings.get(name) == slots[i][0] + slide, f'{vt_name} slot {i} ({name}) is the original entry')
            elif i in pure or i >= 168:
                require(vt_name[len('__ZTV'):] in name, f'{vt_name} slot {i} ({name}): an owned implementation')
        owned[vt_name] = [i for i, (name, _) in enumerate(entries) if vt_name[len('__ZTV'):] in name]

    DATA = {'__ZN11IOHIDDevice10gMetaClassE', '__ZTV11IOHIDDevice'}
    text, addresses = '.text\n.p2align 2\n', ''
    for name, va in sorted(bindings.items()):
        if name in DATA:
            addresses += f'.globl {name}\n.set {name}, {va:#x}\n'
        else:
            # An absolute literal: neither signed nor moved by the fileset emitter, so the stub lands on the original wherever the
            # native code is placed. Original entries begin with bti c or pacibsp, which accept a BR through x16.
            addresses += f'.globl {name}.original\n.set {name}.original, {va:#x}\n'
            text += f'.globl {name}\n.p2align 3\n{name}:\n bti c\n ldr x16, 1f\n br x16\n1: .quad {name}.original\n'
    (OUT / 'hid-bindings.S').write_text(text)
    (OUT / 'hid-addresses.S').write_text(addresses)
    ios_sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    for stem in ('hid-bindings', 'hid-addresses'):
        run(['xcrun', 'clang', '-target', TRIPLE, '-isysroot', ios_sdk, '-c', OUT / (stem + '.S'), '-o', OUT / (stem + '.o')])
    llvm = Path(run(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin'
    late = OUT / 'NativeHidInput.late.o'
    run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeHidInput.cpp',
         hid_obj, late])

    objects = [late, OUT / 'hid-bindings.o', OUT / 'hid-addresses.o', OUT / 'HidInputProbe.o', OUT / 'DroidVMVirtioInput.o',
               OUT / 'DroidVMMotion.o']
    sources = [original, ROOT / 'kits/droidvm/DroidVMMotionHid.inc', probe, transport, motion, HERE / 'hid-device-24A446.json']
    (OUT / 'objects.json').write_text(json.dumps([str(p.relative_to(ROOT)) for p in objects], indent=2) + '\n')
    (OUT / 'sources.json').write_text(json.dumps([str(p.relative_to(ROOT)) if p.is_relative_to(ROOT) else str(p) for p in sources],
                                                 indent=2) + '\n')
    (OUT / 'report.json').write_text(json.dumps(dict(
        hid_source=dict(repository=HID_REPOSITORY, commit=HID_COMMIT, sha256=sha(original)), iohidfamily_commit=family_commit,
        kernelcache_sha256=abi['kernelcache_sha256'], bindings={n: hex(v) for n, v in sorted(bindings.items())},
        owned_slots=owned, objects={str(p.relative_to(ROOT)): sha(p) for p in objects}, original_instructions_modified=False),
        indent=2) + '\n')
    print(f'HID: {len(bindings)} original IOHIDDevice bindings; {", ".join(f"{v} owns {len(s)} slots" for v, s in owned.items())}')


if __name__ == '__main__':
    main()

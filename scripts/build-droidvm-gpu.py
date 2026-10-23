#!/usr/bin/env python3
"""Build the DroidVM guest's paravirtual Adreno (kits/droidvm/DroidVMGpu.cpp) with the parts of NativeAdreno it keeps.

NativeAdreno's user client, IOKit surface, execbuf and memory (NativeAdrenoUserClient, NativeAdrenoPublish, NativeAdrenoSubmit,
NativeAdrenoMemory, NativeAdrenoCpu) and the PM4 encoder they call are compiled unchanged, except for a few lines of
NativeAdrenoPublish.cpp, compiled from a staged copy: its client windows are absolute addresses of sheng's GPU address space
(0x20000000..), and here the space is the KGSL VBO the host reserved, so they are offset from droidvm_gpu_window_base() (the VBO's
start), and there are ten of them instead of four (below). The hardware classes (NativeAdrenoVm, NativeAdrenoRing, NativeAdrenoTimer)
come from DroidVMGpu.cpp instead of their own sources. The staged AGXAccelerator (build-native-adreno-accelerator.py) rides
along for droidvm-gpu-metal=1.

The objects land in build/droidvm-gpu; build-native-legacy-display.py archives them into the late display archive, which the
kernel force-loads (this board does not select CONFIG_XNU_NATIVE_ADRENO, the sheng GPU)."""
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/droidvm-gpu'
OUT.mkdir(parents=True, exist_ok=True)
ADRENO = ROOT / 'drivers/NativeAdreno'


def recipe(name):
    return json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837' / name).read_text().strip().rstrip(','))


def compile_with(record, source, obj, extra):
    command, i, arguments = [], 0, record['arguments']
    while i < len(arguments):
        value = arguments[i]
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command += ['-o', str(obj)]
            i += 1
        elif value in ('-MF', '-MJ', '-MT'):
            i += 1
        elif value == '-Xclang' and arguments[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
        i += 1
    command += extra
    result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
    obj.with_suffix('.log').write_text(result.stderr)
    if result.returncode:
        errors = [line for line in result.stderr.splitlines() if ' error: ' in line]
        sys.exit(f'{source}: {len(errors)} errors (all in {obj.with_suffix(".log")}):\n' + '\n'.join(errors[:20]))


publish = (ADRENO / 'NativeAdrenoPublish.cpp').read_text()
scope = 'core->setScope(slot*NativeAdrenoAbiMaxQueues,kClientWindows[slot].start,kClientWindows[slot].bytes)'
if publish.count(scope) != 1:
    sys.exit('NativeAdrenoPublish.cpp no longer scopes its clients with kClientWindows[slot] exactly once')
publish = publish.replace(scope, 'core->setScope(slot*NativeAdrenoAbiMaxQueues,'
                          'droidvm_gpu_window_base()+(kClientWindows[slot].start-kClientWindows[0].start),kClientWindows[slot].bytes)')
anchor = '#include "NativeAdrenoUserClient.h"\n'
if publish.count(anchor) != 1:
    sys.exit('NativeAdrenoPublish.cpp include anchor not found')
publish = publish.replace(anchor, anchor + '/* DroidVM: the clients\' windows start at the VBO (kits/droidvm/DroidVMGpu.cpp). */\n'
                          'extern "C" uint64_t droidvm_gpu_window_base(void);\n')
# Ten connections instead of NativeAdreno's four: the render server, the widget renderer, the wallpaper and the app icons take four,
# WebKit's GPU and WebContent processes (droidvm-gpu-clients com.apple.WebKit.*) more, and an app that draws with Metal itself (Maps)
# one each. Each window is 1 GiB, following the render server's; the ten take 10 GiB of the KGSL VBO (128 GiB on the SM8650 host),
# which DroidVMGpu.cpp requires. sheng's aperture ends at 0x80000000, so NativeAdreno splits it in three.
table = re.search(r'enum : unsigned \{NativeAdrenoMaxClients=4\};\n'
                  r'static const struct \{uint64_t start,bytes;\} kClientWindows\[NativeAdrenoMaxClients\]=\{\n'
                  r'    \{0x20000000ull,0x40000000ull\}(,\{0x[0-9A-Fa-f]+ull,0x[0-9A-Fa-f]+ull\}){3}\};', publish)
if not table:
    sys.exit('NativeAdrenoPublish.cpp no longer declares the four client windows')
windows = ','.join('{%#xull,0x40000000ull}' % (0x20000000 + (i << 30)) for i in range(10))
publish = publish.replace(table[0], 'enum : unsigned {NativeAdrenoMaxClients=10};\n'
                          'static const struct {uint64_t start,bytes;} kClientWindows[NativeAdrenoMaxClients]={\n    ' + windows + '};')
staged = OUT / 'NativeAdrenoPublish.cpp'
staged.write_text(publish)

cxx = recipe('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
c = recipe('bsd/DEVELOPMENT/bsd_init.o.json')
includes = ['-I', str(ADRENO), '-I', str(ROOT / 'kits/ArmSmmu4K'), '-I', str(ROOT / 'kits/AdrenoPM4'), '-I', str(ROOT / 'kits/droidvm'),
            '-I', str(ROOT / '.cache/xnu-work/osfmk')]
warnings = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-incompatible-sysroot', '-Wno-c99-designator',
            '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier']
sources = [ADRENO / name for name in ('NativeAdrenoSubmit.cpp', 'NativeAdrenoMemory.cpp', 'NativeAdrenoCpu.cpp')]
sources += [ADRENO / 'NativeAdrenoUserClient.cpp', staged, ROOT / 'kits/droidvm/DroidVMGpu.cpp']
objects = []
for source in sources:
    obj = OUT / (source.stem + '.o')
    compile_with(cxx, source, obj, includes + warnings)
    objects.append(obj)
pm4 = OUT / 'AdrenoPM4.kit.o'
compile_with(c, ROOT / 'kits/AdrenoPM4/AdrenoPM4.c', pm4, includes + warnings)
objects.append(pm4)

print(subprocess.run(['python3', ROOT / 'scripts/build-native-adreno-accelerator.py'], check=True, capture_output=True,
                     text=True).stdout, end='')
accelerator = ROOT / 'build/native-adreno-accelerator'
objects += [accelerator / 'NativeAdrenoAccelerator.late.o', accelerator / 'original-bindings.o']

defined = subprocess.run(['xcrun', 'nm', '-g', '-U', *objects], capture_output=True, text=True, check=True).stdout
required = ['_native_adreno_publish', '_sheng_gpu_power_probe', '_droidvm_gpu_window_base', '_AdrenoPM4Type7',
            '_native_adreno_accelerator_allocate', '__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp',
            '__ZN14NativeAdrenoVm3mapEP18NativeAdrenoMemoryjyjjjPy', '__ZN16NativeAdrenoRing12submitFramedEPKjmPj']
missing = [name for name in required if not any(line.endswith(' ' + name) for line in defined.splitlines())]
if missing:
    sys.exit('Definitions absent: ' + ', '.join(missing))
(OUT / 'objects.json').write_text(json.dumps([str(p.relative_to(ROOT)) for p in objects], indent=2) + '\n')
(OUT / 'sources.json').write_text(json.dumps([str(p.relative_to(ROOT)) for p in
                                              sources[:-3] + [ADRENO / 'NativeAdrenoUserClient.cpp',
                                                              ADRENO / 'NativeAdrenoPublish.cpp', sources[-1],
                                                              ROOT / 'kits/droidvm/DroidVMGpuWire.h',
                                                              ROOT / 'kits/AdrenoPM4/AdrenoPM4.c',
                                                              ADRENO / 'NativeAdrenoAccelerator.cpp']], indent=2) + '\n')
print(f'droidvm-gpu: {len(objects)} objects (NativeAdreno user client, IOKit surface, execbuf and memory over the KGSL proxy)')

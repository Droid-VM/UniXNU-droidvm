#!/usr/bin/env python3
"""Build the DroidVM guest's paravirtual Adreno (kits/droidvm/DroidVMGpu.cpp) with the parts of NativeAdreno it keeps.

NativeAdreno's user client, IOKit surface, execbuf and memory (NativeAdrenoUserClient, NativeAdrenoPublish, NativeAdrenoSubmit,
NativeAdrenoMemory, NativeAdrenoCpu) and the PM4 encoder they call are compiled unchanged, except for a few lines of two
files, compiled from staged copies: NativeAdrenoPublish.cpp's client windows are absolute addresses of sheng's GPU address space
(0x20000000..), and here the space is the KGSL VBO the host reserved, so they are offset from droidvm_gpu_window_base() (the VBO's
start), and there are six of them instead of three, and its user client frees its views the way they were allocated (below);
NativeAdrenoUserClient.cpp's setScope() refuses a new connection while the render server has jobs in flight (below). The hardware classes (NativeAdrenoVm, NativeAdrenoRing, NativeAdrenoTimer)
come from DroidVMGpu.cpp instead of their own sources. The staged AGXAccelerator (build-native-adreno-accelerator.py) rides
along for droidvm-gpu-metal=1.

The objects land in build/droidvm-gpu; build-native-legacy-display.py archives them into the late display archive, which the
kernel force-loads (this board does not select CONFIG_XNU_NATIVE_ADRENO, the sheng GPU)."""
import json
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
# Six connections instead of three: the render server, the widget renderer and the wallpaper already take three, and iconservicesagent
# (the app icons) needs a fourth. The added windows follow the others (256 MiB each), still inside the 3 GiB of the VBO that
# DroidVMGpu.cpp requires; sheng's aperture ends at the third.
table = ('enum : unsigned {NativeAdrenoMaxClients=3};\n'
         'static const struct {uint64_t start,bytes;} kClientWindows[NativeAdrenoMaxClients]={\n'
         '    {0x20000000ull,0x40000000ull},{0x60000000ull,0x10000000ull},{0x70000000ull,0x10000000ull}};')
if publish.count(table) != 1:
    sys.exit('NativeAdrenoPublish.cpp no longer declares the three client windows')
publish = publish.replace(table, 'enum : unsigned {NativeAdrenoMaxClients=6};\n'
                          'static const struct {uint64_t start,bytes;} kClientWindows[NativeAdrenoMaxClients]={\n'
                          '    {0x20000000ull,0x40000000ull},{0x60000000ull,0x10000000ull},{0x70000000ull,0x10000000ull},\n'
                          '    {0x80000000ull,0x10000000ull},{0x90000000ull,0x10000000ull},{0xa0000000ull,0x10000000ull}};')
# The user client's views are allocated typed (IONewZero: a View holds a descriptor pointer) but were freed to the data heap
# (IODeleteData), which kfree catches as heap confusion and panics on in AGXDeviceUserClient::free(). Nothing freed a user client
# while only the render server, which never exits, opened one; the granted clients (DroidVMGpu.cpp grantedProcesses) close theirs.
# A NativeAdreno bug: the same one-line fix belongs upstream.
free_views = 'if(views_) {IODeleteData(views_,View,MaxViews);views_=nullptr;}'
if publish.count(free_views) != 1:
    sys.exit('NativeAdrenoPublish.cpp no longer frees its views with IODeleteData exactly once')
publish = publish.replace(free_views, 'if(views_) {IODelete(views_,View,MaxViews);views_=nullptr;}')
staged = OUT / 'NativeAdrenoPublish.cpp'
staged.write_text(publish)

# A new connection's setScope() also asks whether its current queue range has jobs outstanding, and an unscoped core's range is 0,
# the render server's: while the render server has a job in flight every other process's open fails with kIOReturnBusy (a listed
# client then keeps the software renderer for good). The old range matters only once the core was scoped.
client = (ADRENO / 'NativeAdrenoUserClient.cpp').read_text()
check = 'if(liveObjects() || submit_->rangeOutstanding(queue_base_,NativeAdrenoAbiMaxQueues) ||'
if client.count(check) != 1:
    sys.exit('NativeAdrenoUserClient.cpp no longer checks the old queue range in setScope exactly once')
client = client.replace(check, 'if(liveObjects() || (window_bytes_ && submit_->rangeOutstanding(queue_base_,NativeAdrenoAbiMaxQueues)) ||')
staged_client = OUT / 'NativeAdrenoUserClient.cpp'
staged_client.write_text(client)

cxx = recipe('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
c = recipe('bsd/DEVELOPMENT/bsd_init.o.json')
includes = ['-I', str(ADRENO), '-I', str(ROOT / 'kits/ArmSmmu4K'), '-I', str(ROOT / 'kits/AdrenoPM4'), '-I', str(ROOT / 'kits/droidvm'),
            '-I', str(ROOT / '.cache/xnu-work/osfmk')]
warnings = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-incompatible-sysroot', '-Wno-c99-designator',
            '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier']
sources = [ADRENO / name for name in ('NativeAdrenoSubmit.cpp', 'NativeAdrenoMemory.cpp', 'NativeAdrenoCpu.cpp')]
sources += [staged_client, staged, ROOT / 'kits/droidvm/DroidVMGpu.cpp']
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

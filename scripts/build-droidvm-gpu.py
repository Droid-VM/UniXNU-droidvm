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
                          'extern "C" uint64_t droidvm_gpu_window_base(void);\n'
                          '/* DroidVM: the host gives each slot a KGSL context of its own (DroidVMGpu.cpp). */\n'
                          'extern "C" volatile uint32_t droidvm_gpu_client;\n'
                          'extern "C" void droidvm_gpu_client_opened(uint32_t slot);\n')
# Each slot has a KGSL context of its own on the host, so a client whose commands fault loses its own GPU, not the render server's
# (Maps' VectorKit once took the whole display down): every call into a connection's core says whose it is (under g.lock, as all
# dispatch is), and a slot's new user gets a fresh context.
dispatch = 'connection->live_?connection->core_->dispatch('
if publish.count(dispatch) != 2:
    sys.exit('NativeAdrenoPublish.cpp no longer dispatches to a connection\'s core in exactly two places')
publish = publish.replace(dispatch, 'connection->live_?(droidvm_gpu_client=connection->slot_<NativeAdrenoMaxClients?connection->slot_:0u,'
                          'connection->core_)->dispatch(')
mounted = '            if(kernel)g.kernel_client=true;else g.slots[slot]=true;\n'
if publish.count(mounted) != 1:
    sys.exit('NativeAdrenoPublish.cpp no longer marks a mounted slot exactly once')
publish = publish.replace(mounted, mounted + '            if(!kernel)droidvm_gpu_client_opened(slot);\n')
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
# A client that dies with its BOs mapped: a crash's corpse keeps its address space while IOKit closes the connection
# (iokit_client_died under ipc_space_terminate), so clientClose freed BO pages the dead process still mapped through their CPU
# views, and the VM's pmap_assert_free panicked the guest (WidgetRenderer_Default crashed with BOs from the scattered path; pooled
# BOs never go back to the VM). The views are physical-range descriptors that do not hold the pages, so every mapping IOKit made
# of this connection's memory is taken down first, then the connection closes as before.
declaration = '    IOReturn clientClose() APPLE_KEXT_OVERRIDE;\n'
if publish.count(declaration) != 1:
    sys.exit('NativeAdrenoPublish.cpp no longer declares AGXDeviceUserClient::clientClose exactly once')
publish = publish.replace(declaration, declaration + '    IOReturn clientDied() APPLE_KEXT_OVERRIDE;\n')
definition = 'void AGXDeviceUserClient::free()\n'
if publish.count(definition) != 1:
    sys.exit('NativeAdrenoPublish.cpp no longer defines AGXDeviceUserClient::free exactly once')
publish = publish.replace(definition, 'IOReturn AGXDeviceUserClient::clientDied()\n'
                          '{\n'
                          '    if(mappings) {\n'
                          '        if(OSCollectionIterator *iterator=OSCollectionIterator::withCollection(mappings)) {\n'
                          '            while(OSObject *object=iterator->getNextObject())if(IOMemoryMap *map=OSDynamicCast(IOMemoryMap,object))map->unmap();\n'
                          '            iterator->release();\n'
                          '        }\n'
                          '    }\n'
                          '    return IOUserClient::clientDied();\n'
                          '}\n' + definition)
# The open cap counts a name's opens in a token bucket, not in the boot: eight at once, then one more each 30 seconds. NativeAdreno
# refuses a process name's ninth open of the boot (kIOReturnBusy), against a plugin that crashes its host on every launch; but the
# wallpaper's poster extensions run one process per variant and per host (PosterBoard's gallery, SpringBoard's lock screen), and
# after eight every AegirPoster and KaleidoscopePoster got no device and crashed -- SpringBoard relaunched its wallpaper in a loop.
# Widget renderers and WebKit's processes open again in normal use too. A process that crashes every few seconds still runs dry.
if publish.count('#include <kern/thread.h>\n') != 1:
    sys.exit('NativeAdrenoPublish.cpp no longer includes <kern/thread.h> exactly once')
publish = publish.replace('#include <kern/thread.h>\n', '#include <kern/thread.h>\n#include <kern/clock.h>\n')
capped = '    const bool capped=cap && record && record->opens>=cap;\n'
if publish.count(capped) != 1:
    sys.exit('NativeAdrenoPublish.cpp no longer caps a name\'s opens with record->opens>=cap exactly once')
publish = publish.replace(capped,
    '    static uint64_t bucket_time[8];\n'
    '    static uint32_t bucket_tokens[8];\n'
    '    bool capped=false;\n'
    '    if(cap && record) {\n'
    '        const size_t index=size_t(record-native_adreno_client_log);\n'
    '        uint64_t now=0,period=0;\n'
    '        clock_get_uptime(&now);\n'
    '        nanoseconds_to_absolutetime(30000000000ull,&period);\n'
    '        if(!bucket_time[index]) {bucket_time[index]=now;bucket_tokens[index]=cap;}\n'
    '        const uint64_t earned=period?(now-bucket_time[index])/period:0;\n'
    '        if(earned) {\n'
    '            bucket_tokens[index]=uint32_t(bucket_tokens[index]+earned<cap?bucket_tokens[index]+earned:cap);\n'
    '            bucket_time[index]+=earned*period;\n'
    '        }\n'
    '        if(bucket_tokens[index])--bucket_tokens[index];else capped=true;\n'
    '    }\n')
staged = OUT / 'NativeAdrenoPublish.cpp'
staged.write_text(publish)

# Every CPU mapping of a BO's own pages goes before the pages go back to the VM. A BO's CPU views are physical-range descriptors that
# hold no reference to the pages, so a mapping that outlived the BO left pmap_assert_free a user mapping of a freed page and the
# guest panicked -- still after clientDied unmapped the dying client's own views (iconservicesagent, killed by jetsam): a copy in
# another task, or one IOKit does not track. A process that touches the memory afterwards faults instead. Pool pages stay mapped
# by the kernel and imported pages are not the BO's, so only an owned descriptor's pages are disconnected.
memory = (ADRENO / 'NativeAdrenoMemory.cpp').read_text()
release = 'IOReturn NativeAdrenoMemory::release()\n'
freed = '        native_adreno_trace_stage=33;\n        descriptor_->release();descriptor_=nullptr;\n'
if memory.count(release) != 1 or memory.count(freed) != 1:
    sys.exit('NativeAdrenoMemory.cpp no longer releases an owned descriptor where DroidVM disconnects its pages')
memory = memory.replace(release, '/* DroidVM: pmap_disconnect (osfmk/vm/pmap.h, MACH_KERNEL_PRIVATE) drops every CPU mapping of a physical page. */\n'
                        'extern "C" unsigned int pmap_disconnect(ppnum_t);\n'
                        'static void disconnectOwnedPages(const NativeAdrenoPhysicalSpan *spans,size_t count)\n'
                        '{\n'
                        '    for(size_t i=0;spans && i<count;++i)\n'
                        '        for(uint64_t at=spans[i].physical&~uint64_t(PAGE_MASK);at<spans[i].physical+spans[i].bytes;at+=PAGE_SIZE)\n'
                        '            (void)pmap_disconnect(ppnum_t(at>>PAGE_SHIFT));\n'
                        '}\n' + release)
memory = memory.replace(freed, '        native_adreno_trace_stage=33;\n        disconnectOwnedPages(spans_,span_count_);\n'
                        '        descriptor_->release();descriptor_=nullptr;\n')
(OUT / 'memory').mkdir(exist_ok=True)
staged_memory = OUT / 'memory' / 'NativeAdrenoMemory.cpp'
staged_memory.write_text(memory)

cxx = recipe('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
c = recipe('bsd/DEVELOPMENT/bsd_init.o.json')
includes = ['-I', str(ADRENO), '-I', str(ROOT / 'kits/ArmSmmu4K'), '-I', str(ROOT / 'kits/AdrenoPM4'), '-I', str(ROOT / 'kits/droidvm'),
            '-I', str(ROOT / '.cache/xnu-work/osfmk')]
warnings = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-incompatible-sysroot', '-Wno-c99-designator',
            '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier']
sources = [ADRENO / 'NativeAdrenoSubmit.cpp', staged_memory, ADRENO / 'NativeAdrenoCpu.cpp']
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

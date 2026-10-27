#!/usr/bin/env python3
"""Build the DroidVM guest's NativeAdreno face for Metal over the virtio GPU (kits/droidvm/DroidVMGpu.cpp) with the parts of
NativeAdreno it keeps.

NativeAdreno's user client, IOKit surface, execbuf and memory (NativeAdrenoUserClient, NativeAdrenoPublish, NativeAdrenoSubmit,
NativeAdrenoMemory, NativeAdrenoCpu) and the PM4 encoder they call are compiled unchanged, except for a few lines of
NativeAdrenoPublish.cpp, compiled from a staged copy (its client windows, 32 slots and the per-slot hooks below, made for the
KGSL proxy crosvm's droidvm-gpu device used to be; with that device gone the hooks refuse NativeAdreno's own KGSL-style opens).
The hardware classes (NativeAdrenoVm, NativeAdrenoRing, NativeAdrenoTimer) come from DroidVMGpu.cpp, never attached, instead of
their own sources. The staged AGXAccelerator (build-adreno740-accelerator.py) rides along for droidvm-gpu-metal=1: Metal's
user client is the accelerator's, which DroidVMVirtioGpu3D.cpp serves.

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
ADRENO = ROOT / 'drivers/Adreno740'


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
                          'extern "C" IOReturn droidvm_gpu_client_opened(uint32_t slot);\n'
                          'extern "C" IOReturn droidvm_gpu_client_health(uint32_t slot,bool refresh);\n')
# Each slot has a KGSL context of its own on the host, so a client whose commands fault loses its own GPU, not the render server's
# (Maps' VectorKit once took the whole display down): every call into a connection's core says whose it is (under g.lock, as all
# dispatch is), and a slot's new user gets a fresh context.
dispatch = 'connection->live_?connection->core_->dispatch('
if publish.count(dispatch) != 2:
    sys.exit('NativeAdrenoPublish.cpp no longer dispatches to a connection\'s core in exactly two places')
publish = publish.replace(dispatch, 'connection->live_?connection->dispatchDroidVM(')
# Global retirement includes cancelled jobs so releaseAll can reclaim their references.
# Only the external connection boundary turns cancellation into device loss. In particular,
# both WaitFence dispatch sites (including the mailbox) must check the mask AFTER the core's
# poll, since that very poll can discover a host reset. No new GPU fence or wait is inserted.
slot_field = '    unsigned slot_=NativeAdrenoMaxClients;\n'
if publish.count(slot_field) != 1:
    sys.exit('NativeAdrenoPublish.cpp slot field changed')
publish = publish.replace(slot_field, slot_field + '''    IOReturn dispatchDroidVM(uint32_t method,const void *input,size_t input_size,
        void *output,size_t output_size,const NativeAdrenoRingClock *clock) {
        droidvm_gpu_client=slot_<NativeAdrenoMaxClients?slot_:0u;
        if(!kernel_ && method==NativeAdrenoMethodGetStatus) {
            if(input_size || !output || output_size!=sizeof(NativeAdrenoAbiStatus))return kIOReturnBadArgument;
            const IOReturn health=droidvm_gpu_client_health(slot_,true);
            if(health && health!=kIOReturnAborted)return health;
            auto *reply=static_cast<NativeAdrenoAbiStatus *>(output);
            reply->faults=health==kIOReturnAborted?1:0;reply->suspends=0;
            return kIOReturnSuccess;
        }
        if(!kernel_ && (method==NativeAdrenoMethodWaitFence || method==NativeAdrenoMethodSubmit)) {
            const IOReturn health=droidvm_gpu_client_health(slot_,false);
            if(health)return health;
        }
        const IOReturn result=core_->dispatch(method,input,input_size,output,output_size,clock);
        if(!kernel_ && method==NativeAdrenoMethodWaitFence) {
            const IOReturn health=droidvm_gpu_client_health(slot_,false);
            if(health)return health;
        }
        return result;
    }
''')
scoped_line = next((line for line in publish.splitlines(keepends=True)
                    if 'const IOReturn scoped=kernel?core->setScope(' in line), None)
if not scoped_line or publish.count(scoped_line) != 1:
    sys.exit('NativeAdrenoPublish.cpp scope/reset anchor changed')
publish = publish.replace(scoped_line, scoped_line.replace('const IOReturn scoped=', 'IOReturn scoped=') +
                          '        if(!scoped && !kernel)scoped=droidvm_gpu_client_opened(slot);\n')
# 32 connections instead of NativeAdreno's four: the render server, the widget renderer, the app icons and Settings take four,
# PosterBoard and each running wallpaper poster extension one each (*Poster*: the gallery runs several at once), WebKit's GPU and
# WebContent processes (com.apple.WebKit.*) more, and an app that draws with Metal itself one each; ten were taken in normal use.
# Each window is 1 GiB, following the render server's; the 32 take 32 GiB of the KGSL VBO (128 GiB on the SM8650 host), which
# DroidVMGpu.cpp requires, and crosvm keeps a KGSL context for each (DROIDVM_GPU_MAX_CLIENTS). sheng's aperture ends at 0x80000000,
# so NativeAdreno splits it in three.
table = re.search(r'enum : unsigned \{NativeAdrenoMaxClients=4\};\n'
                  r'static const struct \{uint64_t start,bytes;\} kClientWindows\[NativeAdrenoMaxClients\]=\{\n'
                  r'    \{0x20000000ull,0x40000000ull\}(,\{0x[0-9A-Fa-f]+ull,0x[0-9A-Fa-f]+ull\}){3}\};', publish)
if not table:
    sys.exit('NativeAdrenoPublish.cpp no longer declares the four client windows')
windows = ','.join('{%#xull,0x40000000ull}' % (0x20000000 + (i << 30)) for i in range(32))
publish = publish.replace(table[0], 'enum : unsigned {NativeAdrenoMaxClients=32};\n'
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

# An owned BO's pages go back to the VM only once no user task maps them. A BO's CPU views are physical-range descriptors that hold
# no reference to the pages, and a mapping of one can outlive the BO, one IOKit does not track (clientClose already removes every
# mapping it made of the views): freeing the pages under it panicked the guest (pmap_assert_free; WidgetRenderer_Default, then
# iconservicesagent killed by jetsam), and disconnecting it instead let it fault the pages in again from their next owner, so the
# render server drew one layer's contents in another (a Settings row showed the status bar's time). Such a BO's pages are kept and
# freed by a later release once no user pmap maps them (native_pmap_user_mappings, patch zzx-pmap-user-mappings); each kept BO is
# logged with the pmap and address of the mapping, which names who holds it. Pool pages stay mapped by the kernel and imported
# pages are not the BO's, so only an owned descriptor's pages are checked.
memory = (ADRENO / 'NativeAdrenoMemory.cpp').read_text()
release = 'IOReturn NativeAdrenoMemory::release()\n{\n'
freed = '        native_adreno_trace_stage=33;\n        descriptor_->release();descriptor_=nullptr;\n'
if memory.count(release) != 1 or memory.count(freed) != 1:
    sys.exit('NativeAdrenoMemory.cpp no longer releases an owned descriptor where DroidVM keeps its mapped pages')
memory = memory.replace(release, """/* DroidVM: owned BO pages a user task still maps are kept, and freed by a later release once unmapped (build-droidvm-gpu.py). */
extern "C" unsigned int native_pmap_user_mappings(ppnum_t,void **,uint64_t *,char *);
/* [0] BOs kept, [1] freed later, [2] held now, [3] their bytes, [4] the last kept BO's mapping pmap, [5] its address. */
extern "C" {volatile uint64_t native_adreno_kept[6];}
namespace {
struct KeptBo {IOBufferMemoryDescriptor *descriptor;NativeAdrenoPhysicalSpan *spans;size_t count,capacity,bytes;};
enum {MaxKeptBos=512};
KeptBo gKeptBos[MaxKeptBos];
IOLock *gKeptLock;
}
static bool userMapped(const NativeAdrenoPhysicalSpan *spans,size_t count,void **pmap,uint64_t *va,char *name)
{
    for(size_t i=0;spans && i<count;++i)
        for(uint64_t at=spans[i].physical&~uint64_t(PAGE_MASK);at<spans[i].physical+spans[i].bytes;at+=PAGE_SIZE)
            if(native_pmap_user_mappings(ppnum_t(at>>PAGE_SHIFT),pmap,va,name))return true;
    return false;
}
static IOLock *keptLock()
{
    if(!gKeptLock) {
        IOLock *lock=IOLockAlloc();
        if(lock && !OSCompareAndSwapPtr(nullptr,lock,(void * volatile *)&gKeptLock))IOLockFree(lock);
    }
    return gKeptLock;
}
/* Keeps a BO's descriptor and spans whose pages a user task maps; false when there is no room (the caller frees them as before). */
static bool keepMapped(IOBufferMemoryDescriptor *descriptor,NativeAdrenoPhysicalSpan *spans,size_t count,size_t capacity,size_t bytes)
{
    void *pmap=nullptr;uint64_t va=0;char name[17]={};
    if(!userMapped(spans,count,&pmap,&va,name))return false;
    IOLock *lock=keptLock();
    if(!lock)return false;
    bool kept=false;
    IOLockLock(lock);
    for(size_t i=0;i<MaxKeptBos && !kept;++i)if(!gKeptBos[i].descriptor) {
        gKeptBos[i]=KeptBo{descriptor,spans,count,capacity,bytes};kept=true;
    }
    IOLockUnlock(lock);
    if(!kept) {IOLog("native-adreno: a freed BO of %zu bytes is still mapped (%s, pmap %p, %#llx) and no room is left to keep it\\n",bytes,name,pmap,va);return false;}
    const uint64_t n=__atomic_add_fetch(&native_adreno_kept[0],1,__ATOMIC_RELAXED);
    __atomic_add_fetch(&native_adreno_kept[2],1,__ATOMIC_RELAXED);__atomic_add_fetch(&native_adreno_kept[3],bytes,__ATOMIC_RELAXED);
    native_adreno_kept[4]=uint64_t(uintptr_t(pmap));native_adreno_kept[5]=va;
    if(n<=32)IOLog("native-adreno: freed BO of %zu bytes kept: still mapped by %s (pmap %p) at %#llx; freed by %s\\n",bytes,name,pmap,va,
                   get_task_pmap(current_task())==pmap?"the same task":"another task");
    return true;
}
/* Frees the kept BOs no user task maps any longer. */
static void sweepKept()
{
    if(!native_adreno_kept[2])return;
    IOLock *lock=keptLock();
    if(!lock)return;
    for(size_t i=0;i<MaxKeptBos;++i) {
        IOLockLock(lock);
        KeptBo bo=gKeptBos[i];
        if(!bo.descriptor || userMapped(bo.spans,bo.count,nullptr,nullptr,nullptr)) {IOLockUnlock(lock);continue;}
        gKeptBos[i]=KeptBo{};
        IOLockUnlock(lock);
        __atomic_add_fetch(&native_adreno_kept[1],1,__ATOMIC_RELAXED);
        __atomic_sub_fetch(&native_adreno_kept[2],1,__ATOMIC_RELAXED);__atomic_sub_fetch(&native_adreno_kept[3],bo.bytes,__ATOMIC_RELAXED);
        bo.descriptor->release();
        IODeleteData(bo.spans,NativeAdrenoPhysicalSpan,bo.capacity);
    }
}
""" + release + '    sweepKept();\n')
memory = memory.replace(freed, '        native_adreno_trace_stage=33;\n'
                        '        if(keepMapped(descriptor_,spans_,span_count_,span_capacity_,bytes_))spans_=nullptr;   /* the kept entry owns them */\n'
                        '        else descriptor_->release();\n'
                        '        descriptor_=nullptr;\n')
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

print(subprocess.run(['python3', ROOT / 'scripts/build-adreno740-accelerator.py'], check=True, capture_output=True,
                     text=True).stdout, end='')
accelerator = ROOT / 'build/adreno740-accelerator'
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
                                                              ROOT / 'kits/AdrenoPM4/AdrenoPM4.c',
                                                              ADRENO / 'NativeAdrenoAccelerator.cpp']], indent=2) + '\n')
print(f'droidvm-gpu: {len(objects)} objects (NativeAdreno user client, IOKit surface, execbuf and memory; Metal over the virtio GPU)')

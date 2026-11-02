// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeAdreno's face for the macOS VM, whose Metal draws through the virtio GPU's DRM context (DroidVMVirtioGpu3D.cpp, drm2kgsl
// on the host; vgpu-3d=1). NativeAdreno's user-client ABI, IOKit surface (NativeAdrenoPublish.cpp) and execbuf (NativeAdrenoSubmit.cpp)
// are linked unchanged; the Metal plugin's user client, open type 0x4149, is served by NativeAdrenoPublish once NativeAdreno is
// published, and the AGXAccelerator the Metal discovery finds (the kext's personality) matches the published service. So the board's
// GPU hook only publishes it.
//
// NativeAdreno's hardware classes (NativeAdrenoVm, NativeAdrenoRing, NativeAdrenoTimer) have no hardware here and are never attached:
// this file defines them as such (as upstream's 4284c6c), so every request through NativeAdreno's own KGSL-style user client
// (0x4147) is refused -- a VM, ring or timer call answers not ready. They used to run over crosvm's droidvm-gpu device, a KGSL proxy
// the VM no longer has; the virtio GPU took over everything it did.
//
// sheng_gpu_power_probe (the board's GPU hook, called by DroidVMGpuStarter) publishes from a thread. Progress goes to the kernel log.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoMemory.h"

extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *, const NativeAdrenoRingClock *,
    const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t);

// NativeAdrenoVm.cpp's definitions, which the linked NativeAdreno sources use: IO coherence of imported memory (NativeAdrenoUserClient
// maps an imported BO write-back inner-shareable when it is set), the VM's clock and the board's memory readers (none here).
extern "C" {
volatile uint32_t native_adreno_io_coherent;
uint64_t (*native_adreno_vm_now_us)(void);   // the VM's phase clock, which NativeAdrenoUserClient sets; this VM times nothing
int (*native_adreno_dram_read32)(uint64_t, uint32_t *);
int (*native_adreno_cached_read32)(uint64_t, uint32_t *);
int (*native_adreno_line_audit)(uint64_t, size_t, uint32_t *, uint32_t *);
}

// ---- NativeAdreno's hardware classes, never attached ------------------------------------------------------------------------
// Each answers as an unattached one: arguments checked and outputs cleared first, then not ready.
IOReturn NativeAdrenoVm::attach(const ArmSmmu4KIo *, ArmSmmu4KContextConfig *, ArmSmmu4KStreamState *, const NativeAdrenoVmRegion *,
    const NativeAdrenoVmRegion *, size_t, unsigned)
{
    return kIOReturnUnsupported;
}
IOReturn NativeAdrenoVm::detach() {return kIOReturnBusy;}
NativeAdrenoVm::Mapping *NativeAdrenoVm::find(uint64_t) {return nullptr;}
IOReturn NativeAdrenoVm::map(NativeAdrenoMemory *, unsigned, uint64_t, unsigned, unsigned, unsigned, uint64_t *handle)
{
    if (!handle) return kIOReturnBadArgument;
    *handle = 0;
    result_ = {};
    return kIOReturnNotReady;
}
IOReturn NativeAdrenoVm::unmap(uint64_t)
{
    result_ = {};
    return kIOReturnNotReady;
}
IOReturn NativeAdrenoVm::describe(uint64_t, uint64_t *iova, size_t *bytes, unsigned *permissions) const
{
    if (!iova || !bytes || !permissions) return kIOReturnBadArgument;
    *iova = 0;
    *bytes = 0;
    *permissions = 0;
    return kIOReturnNotReady;
}
IOReturn NativeAdrenoVm::holdJob(uint64_t) {return kIOReturnNotReady;}
IOReturn NativeAdrenoVm::retireJobAfterCompletion(uint64_t) {return kIOReturnNotReady;}
IOReturn NativeAdrenoVm::invalidateTlb() {return kIOReturnNotReady;}
IOReturn NativeAdrenoVm::publishMapped() {return kIOReturnNotReady;}
IOReturn NativeAdrenoVm::acquireImported() {return kIOReturnNotReady;}
IOReturn NativeAdrenoVm::reassertWriteCombined(uint32_t *reset) const
{
    if (!reset) return kIOReturnBadArgument;
    *reset = 0;
    return kIOReturnNotReady;
}
IOReturn NativeAdrenoVm::auditRange(uint64_t, size_t, uint32_t *lines, uint32_t *dirty, uint32_t *stale) const
{
    if (lines) *lines = 0;
    if (dirty) *dirty = 0;
    if (stale) *stale = 0;
    return kIOReturnUnsupported;
}
IOReturn NativeAdrenoVm::sampleWord(uint64_t, uint32_t *cached, uint32_t *dram) const
{
    if (!cached || !dram) return kIOReturnBadArgument;
    *cached = *dram = ~0u;
    return kIOReturnUnsupported;
}
IOReturn NativeAdrenoVm::peekWords(uint64_t, uint32_t *, size_t) const {return kIOReturnBadArgument;}

IOReturn NativeAdrenoRing::attach(const NativeAdrenoRingIo *, const NativeAdrenoRingCache *, void *, size_t, uint64_t, void *, size_t,
    uint64_t)
{
    return kIOReturnUnsupported;
}
IOReturn NativeAdrenoRing::poison()
{
    poisoned_ = true;
    return kIOReturnIOError;
}
IOReturn NativeAdrenoRing::submitFramed(const uint32_t *, size_t, uint32_t *fence)
{
    if (!fence) return kIOReturnBadArgument;
    *fence = 0;
    return kIOReturnNotReady;
}
NativeAdrenoRingFence NativeAdrenoRing::observeFence(uint32_t, const NativeAdrenoRingClock *, uint32_t, uint32_t *observed)
{
    if (observed) *observed = 0;
    return NativeAdrenoRingFencePoisoned;
}
NativeAdrenoRingFence NativeAdrenoRing::wait(uint32_t fence, const NativeAdrenoRingClock *clock, uint32_t timeout_us,
    uint32_t *observed)
{
    return observeFence(fence, clock, timeout_us, observed);
}

IOReturn NativeAdrenoTimer::attach(const NativeAdrenoTimeIo *, uint32_t) {return kIOReturnUnsupported;}
IOReturn NativeAdrenoTimer::detach() {return kIOReturnNotReady;}
IOReturn NativeAdrenoTimer::read(uint64_t *ticks) const
{
    if (!ticks) return kIOReturnBadArgument;
    *ticks = 0;
    return kIOReturnNotReady;
}

// ---- board: publication -----------------------------------------------------------------------------------------------------
// For the host rig: NativeAdreno's publish status (~0 not yet).
extern "C" volatile uint32_t droidvm_gpu_publish = ~0u;
namespace {
NativeAdrenoVm gVm;
NativeAdrenoRing gRing;
NativeAdrenoSubmit gSubmit;
NativeAdrenoTimer gTimer;
NativeAdrenoAbiDeviceInfo gDeviceInfo;

uint64_t ringNow(void *)
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}
int ringIdle(void *)
{
    IODelay(20);
    return 0;
}
const NativeAdrenoRingClock gClock = {nullptr, ringNow, ringIdle};

// The publish, nothing attached: GPU addresses below 2^48, guest physical addresses below 2^40.
void publishThread(void *, wait_result_t)
{
    const IOReturn status = native_adreno_publish(&gVm, &gSubmit, &gRing, &gClock, &gTimer, &gDeviceInfo, 48, 40);
    droidvm_gpu_publish = uint32_t(status);
    if (status) IOLog("droidvm-gpu: publish for the virtio GPU failed (%#x)\n", status);
    else IOLog("droidvm-gpu: published for the virtio GPU\n");
}
}  // namespace

// The board's GPU hook (DroidVMGpuStarter::start).
extern "C" void sheng_gpu_power_probe(void)
{
    thread_t thread = nullptr;
    if (kernel_thread_start(publishThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}

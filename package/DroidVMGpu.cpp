// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeAdreno's face for a DroidVM guest whose Metal draws through the virtio GPU's DRM context (DroidVMVirtioGpu3D.cpp, drm2kgsl
// on the host). NativeAdreno's user-client ABI, IOKit surface (NativeAdrenoPublish.cpp) and execbuf (NativeAdrenoSubmit.cpp) are
// linked unchanged (build-droidvm-gpu.py); the Metal plugin's user client, open type 0x4149, is NativeAdreno's accelerator user
// client, which serves only once NativeAdreno is published. So the board's GPU hook publishes it and registers the accelerator.
//
// NativeAdreno's hardware classes (NativeAdrenoVm, NativeAdrenoRing, NativeAdrenoTimer: the SMMU, the command processor ring and
// the always-on counter on sheng) have no hardware here and are never attached: this file defines them as such, so every request
// through NativeAdreno's own KGSL-style user client (0x4147) is refused -- its opens fail (droidvm_gpu_client_opened), and a VM,
// ring or timer call answers not ready. They used to run over crosvm's droidvm-gpu device, a KGSL proxy that is gone (the host
// offers only generic devices); the virtio GPU took over everything it did.
//
// vgpu-3d=1 droidvm-gpu-metal=1 runs it from the board's GPU hook (sheng_gpu_power_probe, called by the late display probe before
// launchd); droidvm-gpu-clients names the processes besides the render server whose sandbox lets them open the accelerator.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoMemory.h"

extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *, const NativeAdrenoRingClock *,
    const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t);
extern "C" void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
extern "C" IOService *native_adreno_accelerator_allocate(void);
extern "C" unsigned native_adreno_accelerator_instance_size(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" kmod_info_t g_kernel_kmod_info;

// NativeAdrenoVm.cpp's definitions, which the linked NativeAdreno sources use: IO coherence of imported memory (NativeAdrenoUserClient
// maps an imported BO write-back inner-shareable when it is set), the VM's clock and the board's memory readers (none here).
extern "C" {
volatile uint32_t native_adreno_io_coherent;
uint64_t (*native_adreno_vm_now_us)(void);   // the VM's phase clock, which NativeAdrenoUserClient sets; this VM times nothing
int (*native_adreno_dram_read32)(uint64_t, uint32_t *);
int (*native_adreno_cached_read32)(uint64_t, uint32_t *);
int (*native_adreno_line_audit)(uint64_t, size_t, uint32_t *, uint32_t *);
}

// For the host rig: NativeAdreno's publish status, then the Metal accelerator's stage (0 not tried .. 5 registered) and status.
extern "C" volatile uint64_t droidvm_metal_publish[3];

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
IOReturn NativeAdrenoVm::peekWordsUncached(uint64_t, uint32_t *, size_t) const {return kIOReturnBadArgument;}
IOReturn NativeAdrenoVm::faultState(uint32_t out[4]) const
{
    if (!out) return kIOReturnBadArgument;
    for (unsigned i = 0; i < 4; ++i) out[i] = 0;
    return kIOReturnNotReady;
}
uint64_t NativeAdrenoVm::handleAt(uint64_t) const {return 0;}

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
IOReturn NativeAdrenoRing::stallState(uint32_t *out)
{
    if (!out) return kIOReturnBadArgument;
    for (unsigned i = 0; i < NativeAdrenoRingStallWords; ++i) out[i] = 0;
    return kIOReturnNotReady;
}
IOReturn NativeAdrenoRing::kick() {return kIOReturnNotReady;}
IOReturn NativeAdrenoRing::ibBefore(uint32_t, uint64_t *iova, uint32_t *dwords) const
{
    if (!iova || !dwords) return kIOReturnBadArgument;
    *iova = 0;
    *dwords = 0;
    return kIOReturnNotReady;
}
IOReturn NativeAdrenoRing::drainBus(uint32_t *ack)
{
    if (!ack) return kIOReturnBadArgument;
    ack[0] = ack[1] = 0;
    return kIOReturnNotReady;
}

IOReturn NativeAdrenoTimer::attach(const NativeAdrenoTimeIo *, uint32_t) {return kIOReturnUnsupported;}
IOReturn NativeAdrenoTimer::detach() {return kIOReturnNotReady;}
IOReturn NativeAdrenoTimer::read(uint64_t *ticks) const
{
    if (!ticks) return kIOReturnBadArgument;
    *ticks = 0;
    return kIOReturnNotReady;
}

// ---- NativeAdrenoPublish's hooks (build-droidvm-gpu.py) ---------------------------------------------------------------------
// The clients' GPU windows start at 0 (they were offsets into the droidvm-gpu device's KGSL VBO); a slot's new user would have
// been given a fresh KGSL context, which there is none of, so its open is refused; and no client is ever lost.
extern "C" uint64_t droidvm_gpu_window_base(void) {return 0;}
extern "C" volatile uint32_t droidvm_gpu_client = 0;
extern "C" IOReturn droidvm_gpu_client_opened(uint32_t) {return kIOReturnNotReady;}
extern "C" IOReturn droidvm_gpu_client_health(uint32_t slot, bool)
{
    return slot >= 32 ? kIOReturnBadArgument : kIOReturnSuccess;   // build-droidvm-gpu.py gives NativeAdrenoPublish 32 slots
}

// ---- board: publication -----------------------------------------------------------------------------------------------------
extern "C" volatile uint64_t droidvm_metal_publish[3] = {};
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

uint32_t bootArg(const char *name, uint32_t fallback)
{
    uint32_t value = fallback;
    PE_parse_boot_argn(name, &value, sizeof value);
    return value;
}

// The processes the accelerator's user client is granted to in place of the sandbox's verdict (NativeSandboxGrantProcess, a
// comma-separated list, patch zzw-native-adreno-sandbox-grant): the render server, and each process droidvm-gpu-clients gives a native
// Metal device of its own (DroidVMLaunchPolicy.c writes the same list for the plugin). Their own profiles refuse this driver's user
// client as well: without the grant WidgetRenderer_Default and iconservicesagent got no device (Vulkan -3), and CoreImage then crashed
// in its GL fallback, which left the widgets and every Settings icon blank. "*" (every process) gets no grant; only named processes
// pass the sandbox, a name longer than a process name's 16 characters by its first 16 (com.apple.WebKit.* names WebKit's processes).
OSString *grantedProcesses()
{
    char names[256] = "backboardd";
    char clients[200] = {};
    if (PE_parse_boot_argn("droidvm-gpu-clients", clients, sizeof clients - 1)) {
        char *name = clients;
        for (char *at = clients;; ++at) {
            if (*at && *at != ',') continue;
            const bool last = !*at;
            *at = 0;
            if (*name && strcmp(name, "*") != 0) {
                strlcat(names, ",", sizeof names);
                strlcat(names, name, sizeof names);
            }
            if (last) break;
            name = at + 1;
        }
    }
    return OSString::withCString(names);
}

// sheng's publishMetal (UniXNU-sheng GpuPowerProbe.cpp): the staged AGXAccelerator class registered after the original IOGPUFamily
// loaded, with the two keys the original Metal discovery reads and the sandbox grant (grantedProcesses).
void publishMetal()
{
    droidvm_metal_publish[1] = 1;
    kern_return_t status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if (status) {droidvm_metal_publish[2] = uint32_t(status); return;}
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!parent || parent->getClassSize() != 136) {droidvm_metal_publish[2] = uint32_t(kIOReturnNotFound); return;}
    droidvm_metal_publish[1] = 2;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {droidvm_metal_publish[2] = uint32_t(kIOReturnNoResources); return;}
    native_adreno_accelerator_static_init();
    status = OSMetaClass::postModLoad(handle);
    if (status) {droidvm_metal_publish[2] = uint32_t(status); return;}
    if (native_adreno_accelerator_instance_size() != 136) {droidvm_metal_publish[2] = uint32_t(kIOReturnInvalid); return;}
    droidvm_metal_publish[1] = 3;
    IOService *accelerator = native_adreno_accelerator_allocate();
    if (!accelerator) {droidvm_metal_publish[2] = uint32_t(kIOReturnNoMemory); return;}
    if (!accelerator->init()) {accelerator->release(); droidvm_metal_publish[2] = uint32_t(kIOReturnError); return;}
    accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
    if (OSString *granted = grantedProcesses()) {
        accelerator->setProperty("NativeSandboxGrantProcess", granted);
        granted->release();
    } else {
        accelerator->setProperty("NativeSandboxGrantProcess", "backboardd");
    }
    accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    // OpenGL ES finds its renderer the same way: GLEngine loads the bundle this names. The stock accelerator
    // (AGXAcceleratorG9G) names AppleMetalGLRenderer, Apple's GL on Metal, which then runs on the process's Metal device.
    // Without it every EAGL context fails, and CoreImage's GL path calls through an empty dispatch table.
    accelerator->setProperty("IOGLESBundleName", "AppleMetalGLRenderer");
    droidvm_metal_publish[1] = 4;
    if (!accelerator->attach(IOService::getPlatform())) {accelerator->release(); droidvm_metal_publish[2] = uint32_t(kIOReturnNotAttached); return;}
    accelerator->registerService();
    droidvm_metal_publish[1] = 5;
}

// The publish (nothing attached) and the accelerator.
void publishThread(void *, wait_result_t)
{
    const IOReturn status = native_adreno_publish(&gVm, &gSubmit, &gRing, &gClock, &gTimer, &gDeviceInfo, 48, 40);
    droidvm_metal_publish[0] = uint32_t(status);
    if (status) {IOLog("droidvm-gpu: publish for the virtio GPU failed (%#x)\n", status); return;}
    IOLog("droidvm-gpu: published for the virtio GPU\n");
    publishMetal();
    IOLog("droidvm-gpu: Metal accelerator stage %llu status %#llx\n", droidvm_metal_publish[1], droidvm_metal_publish[2]);
}
}  // namespace

// The board's GPU hook, from the late display probe (before launchd starts the render server).
extern "C" void sheng_gpu_power_probe(void)
{
    if (!bootArg("vgpu-3d", 0) || bootArg("droidvm-gpu-metal", 0) != 1) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(publishThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}

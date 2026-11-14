// SPDX-License-Identifier: BSD-2-Clause-Patent
// From Droid-VM/macOS-DroidVM overlay/kext/DroidVMDisplay/DroidVMDisplay.cpp (9db2f6f, lateautumn233), built here as a kext of the
// DroidVM board's macOS collection (ipados27/kexts/DroidVMDisplay.macos.json) on this repository's virtio-gpu transport.
// DroidVMDisplay: the macOS display of a DroidVM guest, an IOMobileFramebufferLegacy (IOMobileGraphicsFamily) over the firmware
// framebuffer, as AppleParavirtDisplay (AppleParavirtGPUIOGPUFamily, the vma2 display) is over the paravirtual GPU. The family runs
// the swap queue, the user client and the notifications; the driver supplies the hardware hooks AppleParavirtDisplay overrides.
//
// Stage 2: crosvm's virtio-gpu (--virtio-mmio-gpu, /arm-io/virtio-gpu) scans out the swap's surface itself: its pages become a
// guest-handle blob resource, the host blits them to the app's window on each RESOURCE_FLUSH (no copy in the guest). Without the device,
// or for a format it cannot show, stage 1 copies.
//
// Stage 1: the GOP framebuffer the bootloader set up (boot_args Video: PE_Video of the platform) is the scanout. A 16 ms timer takes
// the queued swaps (AppleParavirtDisplay::swap_begin_gated) and the CPU copies the swap's main IOSurface into it, then completes the
// swap. Display attributes, timing and color elements are published as AppleParavirtDisplay::createDisplayAttributes does.
//
// Discovery: IOMobileFramebuffer.framework (iomfb_populate_all_display_infos) expects as many framebuffers as the DisplayPortCount
// of the AppleParavirtGPU services when an /arm-io node is compatible "paravirtualizedgraphics,gpu", and waits for them. Apple's
// AppleParavirtGPUIOGPUFamily is not in the collection, so the class of that name here is the nub of that node and the provider
// of the display.
//
// Offsets into IOMobileFramebufferLegacy and its swap requests are those AppleParavirtDisplay of the same kernelcache (kc265)
// uses; the addresses noted are its code (research/re/apd/).
#include "IOMobileFramebufferABI.h"
#include "DroidVMVirtioGpu.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/device_tree.h>

extern "C" task_t kernel_task;
extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);   // machine_routines.h declares it to the kernel's own files only

// macOS 27's offsets. The addresses in these comments are those of the 26.5 collection where each was first found; k27/TODO.md
// names the same instruction of the same function in 27's.
// IOMobileFramebufferLegacy (instance 0x1dd8)
static constexpr size_t kCurrentSwap = 6720;       // the swap being programmed (swap_begin_gated 0x...dba5c, clean_queue_gated)
static constexpr size_t kSwapQueueFirst = 6728;    // TAILQ head of the pending swaps
static constexpr size_t kSwapQueueLast = 6736;     // its tqh_last
static constexpr size_t kDisplayWidth = 7152;      // display_width() / display_height() (set_digital_out_mode block 0x...d6a60)
static constexpr size_t kDisplayHeight = 7156;
static constexpr size_t kSystemPowerState = 7180;  // u32: setPowerState's state (do_power_state_change_action_gated, request type 0)
static constexpr size_t kAPIPowerState = 7184;     // u32: setAPIPowerState's state (power_state_change_interrupt, request type 1)
static constexpr size_t kHardwarePowered = 7188;   // u32: the display is powered ("PowerStates-Cntr" in swap_wait_gated's report); with
                                                   // kHardwareStarted set, swap_wait fails kIOReturnOffline while it is 0 (0x...2d1e4)
static constexpr size_t kSwapInfoHeld = 7634;      // u16: swap info notifications are held (notify_swap_done_gated 0x...d9430)
static constexpr size_t kSwapInfoTime = 264;       // the swap info's time (notify_swap_done_gated 0x...d93f0)
// IOMobileFramebuffer (instance 144): set before power management in AppleParavirtDisplay's start_hardware block (0x...d8e90)
static constexpr size_t kHardwareStarted = 140;
// IOMFBSwapIORequest
static constexpr size_t kSwapLayers = 984;         // bit 0: main surface, bit 1: cursor surface (program_hardware_gated 0x...db858)
static constexpr size_t kSwapDropReasons = 1004;    // programmed when nothing but bit 3 is set, else dropped (swap_begin_gated 0x...dbac0)
static constexpr size_t kSwapInfoA = 912;         // swap info words 3 and 4 (notify_swap_done_gated 0x...d93f4)
static constexpr size_t kSwapInfoB = 976;
static constexpr size_t kSwapMainSurface = 2944;   // layer i's surface at + 8 * i
static constexpr unsigned kSwapLayerCount = 4;
static constexpr size_t kSwapNext = 3384;          // TAILQ entry of the queue
static constexpr size_t kSwapPrev = 3392;
// IOMFBNotifyRequest
static constexpr size_t kNotifyAsyncReference = 24;   // OSAsyncReference64 of the user client (do_swap_info_notify_gated 0x...d9370)
static constexpr size_t kNotifyMask = 120;            // which swap info words the client wants; bit 31: as 32-bit halves
static constexpr size_t kNotifyLastTime = 128;        // the time of the previous notification (word 1 is the delta)
// IOAVElement: the element ID and its score (createTimingElementWithDimensionsAndRate 0x...d8974, createColorElementWithDepthAndEDR)
static constexpr size_t kElementID = 16;
static constexpr size_t kElementScore = 24;

template <typename T> static T &at(void *base, size_t offset) {return *reinterpret_cast<T *>(static_cast<uint8_t *>(base) + offset);}

// Kernel interfaces of the collection's kexts with no header here.
void iomfbUpdateSwapInfoState(IOMobileFramebufferLegacy *, IOMFBSwapIORequest *, uint32_t)
    __asm("__ZN25IOMobileFramebufferLegacy28update_swap_info_state_gatedEP18IOMFBSwapIORequest24IOMFBSwapCompletionState");
void iomfbSwapInfoDispatch(IOMobileFramebufferLegacy *, void *)
    __asm("__ZN25IOMobileFramebufferLegacy31swap_info_notify_dispatch_gatedEPNS_12SwapInfoBlobE");
IOReturn userClientSendAsyncResult(io_user_reference_t *reference, IOReturn, io_user_reference_t *args, UInt32 count, IOOptionBits)
    __asm("__ZN12IOUserClient28sendAsyncResult64WithOptionsEPyiS0_jj");   // protected in IOUserClient.h
uint64_t ioSurfaceWidth(const IOSurface *) __asm("__ZNK9IOSurface8getWidthEv");
uint64_t ioSurfaceHeight(const IOSurface *) __asm("__ZNK9IOSurface9getHeightEv");
uint64_t ioSurfaceBytesPerRow(const IOSurface *) __asm("__ZNK9IOSurface14getBytesPerRowEv");
uint64_t ioSurfaceBaseOffset(const IOSurface *) __asm("__ZNK9IOSurface13getBaseOffsetEv");
uint32_t ioSurfacePixelFormat(const IOSurface *) __asm("__ZNK9IOSurface14getPixelFormatEv");
IOMemoryDescriptor *ioSurfaceMemory(const IOSurface *) __asm("__ZNK9IOSurface19getMemoryDescriptorEv");
uint32_t ioSurfacePlaneCount(const IOSurface *) __asm("__ZNK9IOSurface13getPlaneCountEv");
uint64_t ioSurfacePlaneOffset(const IOSurface *, uint32_t) __asm("__ZNK9IOSurface14getPlaneOffsetEj");
uint64_t ioSurfacePlaneBytesPerRow(const IOSurface *, uint32_t) __asm("__ZNK9IOSurface19getPlaneBytesPerRowEj");
uint64_t ioSurfacePlaneBytesPerElement(const IOSurface *, uint32_t) __asm("__ZNK9IOSurface23getPlaneBytesPerElementEj");
uint64_t ioSurfacePlaneWidth(const IOSurface *, uint32_t) __asm("__ZNK9IOSurface13getPlaneWidthEj");
uint64_t ioSurfacePlaneHeight(const IOSurface *, uint32_t) __asm("__ZNK9IOSurface14getPlaneHeightEj");
// IOAVVideoTimingData: per axis (horizontal, vertical) 32 bytes of which total +4, active +8, sync width +12, back porch +16,
// front porch +20, sync rate +24, sync polarity +28, then the pixel clock at +68 (IOAVVideoTimingElement's getters; the element keeps
// the data at +32).
OSObject *ioavTimingWithData(const void *data, uint32_t standardType, uint32_t timingStandard)
    __asm("__ZN22IOAVVideoTimingElement8withDataEPK19IOAVVideoTimingData16IOAVStandardType23IOAVVideoTimingStandard");
void ioavTimingSetColorElements(OSObject *, OSArray *) __asm("__ZN22IOAVVideoTimingElement16setColorElementsEP7OSArray");
OSObject *ioavColorWithCapabilities(uint32_t standardType, uint32_t depth, uint32_t encoding, uint32_t range, uint32_t eotf,
                                    uint32_t colorimetry, bool, const OSDictionary *, const OSDictionary *, const OSDictionary *,
                                    const OSSet *)
    __asm("__ZN21IOAVVideoColorElement16withCapabilitiesE16IOAVStandardTypej22IOAVVideoPixelEncoding26IOAVVideoColorDynamicRange"
          "18IOAVVideoColorEOTF20IOAVVideoColorimetrybPK12OSDictionaryS7_S7_PK5OSSet");

// The virtio-gpu of /arm-io/virtio-gpu (crosvm --virtio-mmio-gpu): the DroidVM board's transport (DroidVMVirtioGpu.cpp, exported by
// k27/build-native-display.sh), one control queue shared by the display and the 3D contexts of DroidVMVirtioGpu3D.cpp, which serve
// the GPU's user clients (NativeAdreno's methods 0x7200 on); this class is the display's view of it.
using VirtioGpuHeader = DroidVMVirtioGpu::Header;
using VirtioGpuEntry = DroidVMVirtioGpu::Entry;
struct VirtioGpuRect {uint32_t x, y, width, height;};
extern "C" volatile uint64_t droidvm_virtio_gpu_commands;
// DroidVMVirtioGpu3D.cpp: the number of the latest fenced 3D submission of any client, and whether every one up to a number has finished.
extern "C" uint64_t droidvm_virtgpu_submitted(void);
extern "C" bool droidvm_virtgpu_finished(uint64_t);

class VirtioGpu {
public:
    static constexpr uint32_t kRequestBytes = DroidVMVirtioGpu::MaxCommand;   // a blob's page list: 8 K entries
    static constexpr uint32_t kMaxEntries = (kRequestBytes - 64) / sizeof(VirtioGpuEntry);
    const char *start() {return DroidVMVirtioGpu::start() ? nullptr : "the transport did not start (see virtio-gpu: lines)";}
    bool ready() const {return DroidVMVirtioGpu::ready();}
    uint64_t features() const {return DroidVMVirtioGpu::features();}
    IOReturn command(const void *, uint32_t, const void *extra = nullptr, uint32_t extraSize = 0, bool fenced = true);
    uint64_t commands() const {return droidvm_virtio_gpu_commands;}
};

// Sends a command (and the entries that follow it) and waits for its answer. Fenced, as the iOS board's: the host answers once it has
// done the command; a fenced RESOURCE_FLUSH, once the host's blit to the window has finished (crosvm defers it to the display fence).
// Unfenced, it answers once it has taken the command.
IOReturn VirtioGpu::command(const void *data, uint32_t size, const void *extra, uint32_t extraSize, bool fenced)
{
    if (!ready())
        return kIOReturnNotReady;
    VirtioGpuHeader reply;
    if (fenced)
        return DroidVMVirtioGpu::command(data, size, extra, extraSize, &reply, sizeof(reply));
    uint8_t copy[256];
    if (size < sizeof(VirtioGpuHeader) || size > sizeof(copy))
        return kIOReturnBadArgument;
    memcpy(copy, data, size);
    reinterpret_cast<VirtioGpuHeader *>(copy)->flags &= ~3u;
    uint32_t ticket = 0;
    if (IOReturn status = DroidVMVirtioGpu::post(copy, size, extra, extraSize, sizeof(reply), false, &ticket))
        return status;
    return DroidVMVirtioGpu::wait(ticket, 1000000, &reply, sizeof(reply));
}

class AppleParavirtGPU : public IOService {
    OSDeclareDefaultStructors(AppleParavirtGPU)
public:
    bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(AppleParavirtGPU, IOService)

bool AppleParavirtGPU::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    setProperty("DisplayPortCount", 1ULL, 32);
    registerService();
    IOLog("DroidVMDisplay: AppleParavirtGPU on %s, 1 display port\n", provider->getName());
    return true;
}

class DroidVMDisplay : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(DroidVMDisplay)
public:
    bool start(IOService *) override;
    void stop(IOService *) override;
    bool start_hardware() override;
    IOReturn stop_hardware_gated() override;
    IOReturn swap_begin_gated() override;
    IOReturn swap_wait(IOMobileFramebufferUserClient *, unsigned int, unsigned int, unsigned int) override;
    IOReturn swap_submit(IOMFBSwapRec *, IOUserClient *, unsigned int *) override;
    IOReturn verify_swap(IOMFBSwapIORequest *) override;
    IOReturn swap_layer_map(IOMFBSwapIORequest *, unsigned int) override;
    IOReturn surface_layer_map(IOMFB::AP::DisplayedData *, unsigned int, IOSurface *) override;
    IOReturn surface_layer_unmap(IOMFB::AP::DisplayedData *, unsigned int) override;
    IOReturn set_display_device(unsigned int) override;
    IOReturn set_digital_out_mode(unsigned int, unsigned int) override;
    IOReturn get_dot_pitch(unsigned int *) override;
    IOReturn get_gamma_table(IOMFBGammaTable *) override;
    IOReturn set_gamma_table(IOMFBGammaTable *) override;
    IOReturn set_color_remap_mode(DisplayColorRemapMode) override;
    IOReturn get_color_remap_mode(DisplayColorRemapMode *) const override;
    IOReturn swap_set_color_matrix(IOMFBColorFixedMatrix *, IOMFBColorMatrixFunction, unsigned int) override;
    IOReturn set_notify_tag_mask_gated(IOMFB_NotificationType, IOMFBNotifyRequest *, unsigned int) override;
    IOReturn supported_frame_info(unsigned int, unsigned int *, char (*)[64], unsigned int *) override;
    IOReturn factory_portal(void *, task *) override;
    IOReturn executeFactoryConfigurations_gated() override;
    IOReturn set_frame_info_notification_state_gated(IOMFBNotifyRequest *, bool) override;
    IOReturn do_swap_info_notify_gated(IOMFBNotifyRequest *, const SwapInfoBlob *) override;
    IOReturn notify_swap_done_gated(unsigned int, ::SwapInfoBlob *, unsigned int *, unsigned int) override;
    IOReturn notify_crc_ready_gated(unsigned int) override;
    bool are_timings_enabled() const override;
    IOReturn do_power_state_change_gated() override;

private:
    // The refresh rate of the timing published (boot-arg droidvm-display-hz, 30 to 240): the app's window shows 120 Hz (its gpu-0
    // screen's refresh rate).
    uint32_t fRefreshHz;
    static constexpr uint32_t kWidthMM = 286, kHeightMM = 179;   // a 13.3-inch 16:10 panel: about 114 dpi at 1280x800
    static constexpr uint32_t kColorID = 100, kTimingID = 1000;  // AppleParavirtDisplay's IDs of its first color and timing element

    bool clean_queue_gated();
    void program_hardware_gated();
    void copySurface(IOSurface *, bool refresh = false);
    bool scanOut(IOSurface *);
    struct ScanoutResource;
    ScanoutResource *scanoutResource(IOSurface *, uint32_t format);
    bool releaseResource(ScanoutResource &);
    void requestCopy(IOSurface *, bool refresh, uint64_t work = 0);
    void copyLoop();
    static void copyThreadMain(void *, wait_result_t);
    const uint8_t *surfaceBytes(IOSurface *, IOMemoryDescriptor *);
    void releaseSurfaceMaps();
    void stopCopies();
    bool createDisplayAttributes();
    static void vblFired(OSObject *, IOTimerEventSource *);

    struct SwapInfo {   // IOMobileFramebufferLegacy::SwapInfoBlob as AppleParavirtDisplay fills it (48 bytes)
        uint32_t swapID;
        uint64_t words[5];
    };
    static_assert(sizeof(SwapInfo) == 48, "IOMobileFramebufferLegacy::SwapInfoBlob");
    static constexpr unsigned kSwapInfoRing = 6;

    // Stage 2: the blob resources of the last surfaces shown (WindowServer swaps a few framebuffers in turn), each holding its
    // surface retained and its memory prepared (the host reads the pages) until RESOURCE_UNREF.
    struct ScanoutResource {
        IOSurface *surface;
        IOMemoryDescriptor *memory;
        uint64_t offset, used;
        uint32_t id, format, width, height, stride;
    };
    static constexpr unsigned kScanoutResources = 8;
    VirtioGpu fGpu;
    ScanoutResource fResources[kScanoutResources];
    VirtioGpuEntry *fEntries;   // a blob's page list being built (copy thread)
    uint64_t fResourceClock, fFlushes, fFlushNs, fFlushMaxNs;
    uint64_t fLastFrameTime, fFrameGapMaxNs, fHitches;   // main frames apart by 50 to 500 ms (a still screen is longer)
    uint64_t fSecondStart, fSecondFrames, fPeakFps;      // main frames in each second, the most of a report's seconds
    // Scheduling latency in the guest: the copy thread from its wakeup to running, the VBL timer past its period.
    uint64_t fCopyRequestTime, fWakeMaxNs, fWakeLate, fVBLDue, fVBLLateMaxNs, fVBLLate;
    uint64_t fSubmitTime[16], fFenceWaits, fFenceWaitNs, fFenceWaitMaxNs;   // diagnostic: swap submitted -> programmed (IOSurface fences)
    // A swap's GPU work: the 3D submissions made before it (droidvm_virtgpu_submitted at swap_submit). The host reads the surface for
    // the screen in another context, which does not wait for the guest's renderers: shown before that work has finished, a frame came
    // out half drawn (the wallpaper twitched). The copy thread waits for it; the waits, their time and the ones given up after 200 ms.
    uint64_t fSubmitWork[16], fCopyWork, fGpuWaits, fGpuWaitNs, fGpuWaitMaxNs, fGpuLate;
    uint32_t fScanoutResource;
    unsigned fScanoutFailures;

    PE_Video fVideo;
    IOMemoryMap *fScanoutMap;
    uint8_t *fHalfToByte;     // 'RGhA' surfaces: the 8-bit value of each half float >> 2 (a channel), clamped to [0, 1]
    uint16_t *fRow;           // one 'RGhA' row
    uint32_t *fLine;          // one converted row, written to the scanout at once (it is write-combined)
    uint8_t fXr10ToByte[1024];   // 'b3a8' surfaces: the 8-bit value of each 10-bit extended-range channel, clamped to [0, 1]
    uint64_t fRowBytes;
    uint8_t *fScanout;
    IOTimerEventSource *fVBL;
    DisplayColorRemapMode fColorRemapMode;
    uint32_t fSurfaceFormat;
    bool fDisplayOn;
    uint64_t fFrames;
    uint64_t fLayerShape;             // bring-up: the layers of the last swap, logged when they change
    uint64_t fLastLit, fLastCenter;   // bring-up: lit pixels and centre pixel of the last frame shown
    uint64_t fTicks, fDropped, fReported;   // swap_begin_gated runs, swaps dropped, fSubmitted + fFrames + fDropped + fStalled at the last report
    uint64_t fWaitFailures;                 // diagnostic: swap_wait calls that failed (WindowServer's display readiness)
    uint64_t fWaitReleased;                 // swap_wait calls answered for the swap on screen (swap_wait)
    uint64_t fCopies, fCopyNs, fCopyMaxNs;  // diagnostic: copySurface runs and their time since the last report
    uint64_t fPrepareNs, fReadNs, fConvertNs, fWriteNs;   // diagnostic: the parts of fCopyNs
    // The kernel mappings of the last surfaces copied (WindowServer swaps a few framebuffers in turn); readBytes of each row of an
    // unmapped descriptor was most of the copy's time.
    struct SurfaceMap {
        IOSurface *surface;   // retained while mapped
        IOMemoryMap *map;
        uint64_t used;
    };
    static constexpr unsigned kSurfaceMaps = 4;
    SurfaceMap fSurfaceMaps[kSurfaceMaps];
    uint64_t fSurfaceMapClock;
    // The copies run on a thread of their own: a copy is tens of milliseconds of reading write-combined GPU memory (the surfaces'
    // pages are WC for the GPU, a cached alias reads stale lines), and in the work loop it held the command gate WindowServer swaps
    // through. A swap completes at once; the thread copies the newest surface asked for, the ones it had no time for are skipped.
    IOLock *fCopyLock;
    IOSurface *fCopyNext;             // retained; the next surface to copy
    bool fCopyRefresh, fCopyStop, fCopyThreadDone;
    uint64_t fCopiesSkipped;
    uint64_t fSubmitted, fStalled;          // swaps verified, VBL ticks with a queued swap clean_queue_gated did not hand over
    SwapInfo fSwapInfo[kSwapInfoRing];   // held while kSwapInfoHeld is set, sent in order afterwards
    // The surface on screen, copied again on the ticks without a swap, as a display controller scans out the buffer it shows. Core
    // Animation swaps as soon as it has committed the frame's GPU work, not when the GPU is done, so the copy at the swap can take a
    // frame the GPU is still drawing; once the screen is still (no more swaps) the half-drawn frame stayed (a menu's upper half
    // missing, the outline of a closed window left over the desktop). WindowServer does not draw into the surface on screen until a
    // later swap replaces it.
    static constexpr unsigned kRefreshTicks = 30;   // about half a second after each swap, on its ticks 1, 2, 4, 8, 16 and 30 (a copy
                                                    // takes milliseconds of the work loop)
    IOSurface *fShown;
    unsigned fRefreshLeft;
    unsigned fSwapInfoRead, fSwapInfoWrite;
};

// Registered by the name of vma2's display class: WindowServer (SLCADisplay::read_pv_preferences, GenerateClientUUIDForDisplay)
// takes an external display whose service is an AppleParavirtDisplay with a ParavirtDisplayPrefs dictionary for paravirtualized,
// and names it by its CADisplay uniqueId (IOMFBUUID) rather than a random UUID kept in the Data volume. BackBoard's hit tests look
// the touchscreen's displayUUID (DroidVMInput: IOMFBUUID, as HSTouchHIDService reads a display service's) up among those names.
OSDefineMetaClassWithInit(DroidVMDisplay, IOMobileFramebufferLegacy, )
DroidVMDisplay::MetaClass::MetaClass() : OSMetaClass("AppleParavirtDisplay", DroidVMDisplay::superClass, sizeof(DroidVMDisplay)) {}
OSDefineDefaultStructors(DroidVMDisplay, IOMobileFramebufferLegacy)

static IOPMPowerState gPowerStates[2] = {   // AppleParavirtDisplay's gPowerState
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0},
};

bool DroidVMDisplay::start(IOService *provider)
{
    fRefreshHz = 120;
    PE_parse_boot_argn("droidvm-display-hz", &fRefreshHz, sizeof(fRefreshHz));
    if (fRefreshHz < 30 || fRefreshHz > 240)
        fRefreshHz = 120;
    if (getPlatform()->getConsoleInfo(&fVideo) != kIOReturnSuccess || !fVideo.v_baseAddr || fVideo.v_depth != 32) {
        IOLog("DroidVMDisplay: no 32-bit boot framebuffer (base %#lx depth %lu)\n", fVideo.v_baseAddr, fVideo.v_depth);
        return false;
    }
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withPhysicalAddress(fVideo.v_baseAddr & ~3UL, fVideo.v_rowBytes * fVideo.v_height,
                                                                         kIODirectionInOut);
    if (memory) {
        fScanoutMap = memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
        memory->release();
    }
    if (!fScanoutMap) {
        IOLog("DroidVMDisplay: cannot map the framebuffer at %#lx\n", fVideo.v_baseAddr);
        return false;
    }
    fScanout = reinterpret_cast<uint8_t *>(fScanoutMap->getVirtualAddress());
    IOLog("DroidVMDisplay: scanout %#lx %lux%lu row %lu\n", fVideo.v_baseAddr, fVideo.v_width, fVideo.v_height, fVideo.v_rowBytes);
    // Clear to a solid colour until the first frame: the boot console's text goes away and the screen shows that this mapping reaches it.
    for (unsigned long y = 0; y < fVideo.v_height; y++) {
        auto *row = reinterpret_cast<volatile uint32_t *>(fScanout + y * fVideo.v_rowBytes);
        for (unsigned long x = 0; x < fVideo.v_width; x++)
            row[x] = 0xff203050;
    }
    unsigned useGpu = 1;   // droidvm-display-virtio-gpu=0: stage 1 (the CPU copy) even with the device
    PE_parse_boot_argn("droidvm-display-virtio-gpu", &useGpu, sizeof(useGpu));
    if (useGpu) {
        const char *why = fGpu.start();
        if (!why && (fGpu.features() & 0x48) != 0x48)
            why = "no guest-handle blobs";
        if (!why)
            fEntries = static_cast<VirtioGpuEntry *>(IOMalloc(VirtioGpu::kMaxEntries * sizeof(VirtioGpuEntry)));
        if (!why && !fEntries)
            why = "no page list";
        IOLog("DroidVMDisplay: virtio-gpu scanout %s%s (features %#llx)\n", why ? "off: " : "on", why ? why : "", fGpu.features());
        if (why)
            fGpu = VirtioGpu();
        DroidVMVirtioGpu::selftest3D();   // vgpu-selftest=1: one DRM context through the host's drm2kgsl
    }
    fCopyLock = IOLockAlloc();
    thread_t copier = nullptr;
    if (!fCopyLock || kernel_thread_start(copyThreadMain, this, &copier) != KERN_SUCCESS) {
        IOLog("DroidVMDisplay: no copy thread\n");
        if (fCopyLock)
            IOLockFree(fCopyLock);
        fCopyLock = nullptr;
        OSSafeReleaseNULL(fScanoutMap);
        return false;
    }
    thread_deallocate(copier);
    if (!IOMobileFramebufferLegacy::start(provider)) {
        IOLog("DroidVMDisplay: IOMobileFramebufferLegacy::start failed\n");
        stopCopies();
        OSSafeReleaseNULL(fScanoutMap);
        return false;
    }
    IOLog("DroidVMDisplay: started, display %ux%u at %u Hz\n", display_width(), display_height(), fRefreshHz);
    return true;
}

void DroidVMDisplay::stop(IOService *provider)
{
    IOMobileFramebufferLegacy::stop(provider);
    stopCopies();
    releaseSurfaceMaps();
    for (ScanoutResource &resource : fResources)
        releaseResource(resource);
    if (fEntries)
        IOFree(fEntries, VirtioGpu::kMaxEntries * sizeof(VirtioGpuEntry));
    fEntries = nullptr;
    if (fHalfToByte)
        IOFree(fHalfToByte, 16384);
    if (fRow)
        IOFree(fRow, fRowBytes);
    if (fLine)
        IOFree(fLine, fVideo.v_width * 4);
    fHalfToByte = nullptr;
    fRow = nullptr;
    fLine = nullptr;
    if (fShown)
        reinterpret_cast<OSObject *>(fShown)->release();
    fShown = nullptr;
    OSSafeReleaseNULL(fScanoutMap);
    fScanout = nullptr;
}

// Called by IOMobileFramebufferLegacy::start before it creates the default surface and registers the service (it tests bit 0).
bool DroidVMDisplay::start_hardware()
{
    if (!createDisplayAttributes())
        return false;
    fVBL = IOTimerEventSource::timerEventSource(this, vblFired);
    if (!fVBL || getWorkLoop()->addEventSource(fVBL) != kIOReturnSuccess) {   // the work loop of the family's command gate
        IOLog("DroidVMDisplay: Failed to create VBL timer\n");
        OSSafeReleaseNULL(fVBL);
        return false;
    }
    fVBL->enable();
    at<uint8_t>(this, kHardwareStarted) = 1;
    PMinit();
    getPlatform()->PMRegisterDevice(getProvider(), this);   // the provider is no power-managed AppleARMIODevice here
    registerPowerDriver(this, gPowerStates, 2);
    // The panel is always there: report it plugged in (the digital out state IOMobileFramebufferGetDigitalOutState answers), as
    // AppleParavirtDisplay does when the host reports its display. Unplugged (0), WindowServer kept this display disabled and composited
    // a virtual one as the main display ("get_hotplug_state - not active").
    hotPlug_notify(1);
    IOLog("DroidVMDisplay: hardware started\n");
    return true;
}

IOReturn DroidVMDisplay::stop_hardware_gated()
{
    if (fVBL) {
        fVBL->cancelTimeout();
        getWorkLoop()->removeEventSource(fVBL);
        OSSafeReleaseNULL(fVBL);
    }
    return kIOReturnSuccess;
}

void DroidVMDisplay::vblFired(OSObject *owner, IOTimerEventSource *)
{
    auto *display = static_cast<DroidVMDisplay *>(owner);
    uint64_t now = mach_absolute_time(), late = 0;
    if (display->fVBLDue && now > display->fVBLDue)
        absolutetime_to_nanoseconds(now - display->fVBLDue, &late);
    if (late > 20000000) {
        display->fVBLLate++;
        if (late > display->fVBLLateMaxNs)
            display->fVBLLateMaxNs = late;
    }
    display->swap_begin_gated();
}

// AppleParavirtDisplay::clean_queue_gated: whether a queued swap is ready to be programmed.
bool DroidVMDisplay::clean_queue_gated()
{
    if (at<IOMFBSwapIORequest *>(this, kCurrentSwap) || !at<IOMFBSwapIORequest *>(this, kSwapQueueFirst))
        return false;
    if (dropObsoleteSwaps_gated())
        return false;
    dropNoOpSwaps_gated();
    dropAbortedSwaps_gated(false);
    dropDummySwaps_gated();
    return at<IOMFBSwapIORequest *>(this, kSwapQueueFirst) != nullptr;
}

// AppleParavirtDisplay::swap_begin_gated: the family calls it when a swap is submitted, the VBL timer every refresh.
IOReturn DroidVMDisplay::swap_begin_gated()
{
    if (!fVBL)
        return kIOReturnNotReady;
    fVBL->cancelTimeout();
    uint64_t frames = fFrames;
    while (clean_queue_gated()) {
        IOMFBSwapIORequest *swap = at<IOMFBSwapIORequest *>(this, kSwapQueueFirst);
        at<IOMFBSwapIORequest *>(this, kCurrentSwap) = swap;
        iomfbUpdateSwapInfoState(this, swap, 2);
        IOMFBSwapIORequest *next = at<IOMFBSwapIORequest *>(swap, kSwapNext);
        IOMFBSwapIORequest **prev = at<IOMFBSwapIORequest **>(swap, kSwapPrev);
        if (next)
            at<IOMFBSwapIORequest **>(next, kSwapPrev) = prev;
        else
            at<IOMFBSwapIORequest **>(this, kSwapQueueLast) = prev;
        *prev = next;
        if ((at<uint32_t>(swap, kSwapDropReasons) & ~8u) == 0) {
            program_hardware_gated();
        } else {
            dropSwap_gated(swap, false);
            fDropped++;
        }
    }
    if (fFrames == frames && fShown && fRefreshLeft) {
        unsigned elapsed = kRefreshTicks - --fRefreshLeft;
        if (!(elapsed & (elapsed - 1)) || !fRefreshLeft)
            requestCopy(fShown, true);
    } else if (fFrames == frames && fShown && fGpu.ready() && fTicks % fRefreshHz == 0) {
        // A still screen sends nothing, and the app's window (re)created meanwhile (the activity resumed, rotated) stayed black:
        // flush the surface on screen once a second, a single command when the host scans it out.
        requestCopy(fShown, true);
    }
    if (at<IOMFBSwapIORequest *>(this, kSwapQueueFirst))
        fStalled++;
    if (++fTicks % (10 * fRefreshHz) == 0 && fSubmitted + fFrames + fDropped + fStalled != fReported) {   // about every 10 s while swaps arrive
        fReported = fSubmitted + fFrames + fDropped + fStalled;
        IOLog("DroidVMDisplay: %llu swaps submitted, %llu shown, %llu dropped, %llu ticks stalled (current %p), last frame %llu lit centre %#llx; "
              "%llu copies mean %llu us (prepare %llu read %llu convert %llu write %llu) max %llu us, %llu waits released\n",
              fSubmitted, fFrames, fDropped, fStalled, at<IOMFBSwapIORequest *>(this, kCurrentSwap), fLastLit, fLastCenter,
              fCopies, fCopies ? fCopyNs / fCopies / 1000 : 0, fCopies ? fPrepareNs / fCopies / 1000 : 0,
              fCopies ? fReadNs / fCopies / 1000 : 0, fCopies ? fConvertNs / fCopies / 1000 : 0, fCopies ? fWriteNs / fCopies / 1000 : 0,
              fCopyMaxNs / 1000, fWaitReleased);
        if (fCopiesSkipped)
            IOLog("DroidVMDisplay: %llu copies skipped for newer ones\n", fCopiesSkipped);
        if (fFlushes)
            IOLog("DroidVMDisplay: %llu virtio-gpu flushes mean %llu us max %llu us, %llu commands; %llu frame gaps of 50-500 ms, longest %llu ms\n",
                  fFlushes, fFlushNs / fFlushes / 1000, fFlushMaxNs / 1000, fGpu.commands(), fHitches, fFrameGapMaxNs / 1000000);
        IOLog("DroidVMDisplay: at most %llu frames in a second\n", fPeakFps);
        if (fFenceWaits)
            IOLog("DroidVMDisplay: %llu swaps from submit to programmed mean %llu us max %llu us\n", fFenceWaits,
                  fFenceWaitNs / fFenceWaits / 1000, fFenceWaitMaxNs / 1000);
        fFenceWaits = fFenceWaitNs = fFenceWaitMaxNs = 0;
        if (fGpuWaits)
            IOLog("DroidVMDisplay: %llu frames waited for their GPU work, mean %llu us max %llu us, %llu given up\n", fGpuWaits,
                  fGpuWaitNs / fGpuWaits / 1000, fGpuWaitMaxNs / 1000, fGpuLate);
        fGpuWaits = fGpuWaitNs = fGpuWaitMaxNs = fGpuLate = 0;
        fPeakFps = 0;
        IOLog("DroidVMDisplay: scheduling: %llu copy wakeups over 20 ms (longest %llu ms), %llu VBL ticks over 20 ms late (latest by %llu ms)\n",
              fWakeLate, fWakeMaxNs / 1000000, fVBLLate, fVBLLateMaxNs / 1000000);
        fFlushes = fFlushNs = fFlushMaxNs = fHitches = fFrameGapMaxNs = 0;
        fWakeLate = fWakeMaxNs = fVBLLate = fVBLLateMaxNs = 0;
        fCopies = fCopyNs = fCopyMaxNs = fPrepareNs = fReadNs = fConvertNs = fWriteNs = 0;
    }
    uint64_t period;
    nanoseconds_to_absolutetime(1000000000ULL / fRefreshHz, &period);
    fVBLDue = mach_absolute_time() + period;
    return fVBL->setTimeoutUS(1000000 / fRefreshHz);
}

// CoreAnimation asks swap_wait, without blocking, whether its last swaps are done before it composes the next frame
// (IOMFBDisplay::can_update_); after three swaps WindowServer found the display unready for good.
// The family answers kIOReturnBusy for the last completed swap (+7164, swap_complete_gated) as long as no later one replaces it: a
// display controller is still scanning out its surface (swap_wait_gated 0x...d3dc). Here the swap's surface was copied to the
// scanout when it was programmed, so it is free; WindowServer, told it is busy, waited for its next display update to ask again,
// and the screen updated at about 10 frames a second however little it drew.
IOReturn DroidVMDisplay::swap_wait(IOMobileFramebufferUserClient *client, unsigned int swapID, unsigned int options, unsigned int timeout)
{
    IOReturn result = IOMobileFramebufferLegacy::swap_wait(client, swapID, options, timeout);
    if (result == kIOReturnBusy && (options & 0x7fffffffu) == 0 && swapID == at<uint32_t>(this, 7164)) {
        fWaitReleased++;
        return kIOReturnSuccess;
    }
    if (result != kIOReturnSuccess && (++fWaitFailures <= 20 || fWaitFailures % 1000 == 0))
        IOLog("DroidVMDisplay: swap_wait(%u, 0x%x, %u) = 0x%x (%llu failures): +7164 %u power %u/%u +7188 %u +140 0x%x current %p queue %p\n",
              swapID, options, timeout, result, fWaitFailures, at<uint32_t>(this, 7164), at<uint32_t>(this, kSystemPowerState),
              at<uint32_t>(this, kAPIPowerState), at<uint32_t>(this, kHardwarePowered), at<uint8_t>(this, kHardwareStarted),
              at<IOMFBSwapIORequest *>(this, kCurrentSwap), at<IOMFBSwapIORequest *>(this, kSwapQueueFirst));
    return result;
}

// Diagnostic: when each swap arrives. The family holds a swap until the GPU has written its surfaces (swap_apply_fences_gated:
// IOSurface::createFence on each layer), so submit -> programmed is how long the swap waited for its frame's GPU work.
IOReturn DroidVMDisplay::swap_submit(IOMFBSwapRec *rec, IOUserClient *client, unsigned int *out)
{
    if (rec)
    {
        fSubmitTime[reinterpret_cast<const uint32_t *>(rec)[38] % 16] = mach_absolute_time();
        fSubmitWork[reinterpret_cast<const uint32_t *>(rec)[38] % 16] = droidvm_virtgpu_submitted();
    }
    return IOMobileFramebufferLegacy::swap_submit(rec, client, out);
}

// AppleParavirtDisplay::do_power_state_change_gated: the display is on while both the system and WindowServer (setAPIPowerState)
// want it on; each change goes to WindowServer as a power state notification, which CoreAnimation waits for before it swaps.
IOReturn DroidVMDisplay::do_power_state_change_gated()
{
    bool on = at<uint32_t>(this, kSystemPowerState) && at<uint32_t>(this, kAPIPowerState);
    if (on == fDisplayOn)
        return kIOReturnSuccess;
    fDisplayOn = on;
    at<uint32_t>(this, kHardwarePowered) = on;
    IOLog("DroidVMDisplay: display %s\n", on ? "on" : "off");
    powerstate_notify(true, on);
    return kIOReturnSuccess;
}

// AppleParavirtDisplay::program_hardware_gated (its slot 473): show the swap, then complete it. There is no cursor plane (no
// "CursorPlane" property), so WindowServer draws the cursor into the main surface.
void DroidVMDisplay::program_hardware_gated()
{
    IOMFBSwapIORequest *swap = at<IOMFBSwapIORequest *>(this, kCurrentSwap);
    uint64_t &submitted = fSubmitTime[at<uint32_t>(swap, 980) % 16];
    const uint64_t work = fSubmitWork[at<uint32_t>(swap, 980) % 16];
    if (submitted) {
        uint64_t waited = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - submitted, &waited);
        submitted = 0;
        fFenceWaits++;
        fFenceWaitNs += waited;
        if (waited > fFenceWaitMaxNs)
            fFenceWaitMaxNs = waited;
    }
    // Layer i's surface follows the main one (AppleParavirtDisplay: bit 1, the cursor, at main + 8). After the login transition
    // WindowServer swaps a 128x128 BGRA cursor where the main surface is: the screen is the largest surface of the swap, smaller
    // ones are not drawn over it (the screen keeps its last full frame).
    uint8_t layers = at<uint8_t>(swap, kSwapLayers);
    IOSurface *surface = nullptr;
    uint64_t area = 0, shape = layers;
    for (unsigned layer = 0; layer < kSwapLayerCount; layer++) {
        IOSurface *candidate = (layers >> layer & 1) ? at<IOSurface *>(swap, kSwapMainSurface + 8 * layer) : nullptr;
        if (!candidate)
            continue;
        uint64_t width = ioSurfaceWidth(candidate), height = ioSurfaceHeight(candidate);
        shape = shape * 31 + width * 65537 + height;
        if (width * height > area) {
            area = width * height;
            surface = candidate;
        }
    }
    if (shape != fLayerShape) {   // bring-up: each new combination of layers
        fLayerShape = shape;
        IOLog("DroidVMDisplay: swap layers %#x:", layers);
        for (unsigned layer = 0; layer < kSwapLayerCount; layer++) {
            IOSurface *candidate = (layers >> layer & 1) ? at<IOSurface *>(swap, kSwapMainSurface + 8 * layer) : nullptr;
            if (candidate)
                IOLog(" %u: %llux%llu %#x", layer, ioSurfaceWidth(candidate), ioSurfaceHeight(candidate), ioSurfacePixelFormat(candidate));
        }
        IOLog("\n");
    }
    if (surface && area * 4 >= static_cast<uint64_t>(fVideo.v_width) * fVideo.v_height) {   // a quarter of the screen at least
        if (!fFrames++)   // WindowServer's first frame: the verbose boot console stops drawing over the framebuffer (it goes to serial)
            PE_initialize_console(nullptr, kPEDisableScreen);
        uint64_t now = mach_absolute_time(), gap = 0;
        if (fLastFrameTime)
            absolutetime_to_nanoseconds(now - fLastFrameTime, &gap);
        fLastFrameTime = now;
        uint64_t second = 0;
        absolutetime_to_nanoseconds(now - fSecondStart, &second);
        if (second >= 1000000000) {
            if (fSecondFrames > fPeakFps)
                fPeakFps = fSecondFrames;
            fSecondStart = now;
            fSecondFrames = 0;
        }
        fSecondFrames++;
        if (gap >= 50000000 && gap < 500000000) {
            fHitches++;
            if (gap > fFrameGapMaxNs)
                fFrameGapMaxNs = gap;
        }
        requestCopy(surface, false, work);
        if (surface != fShown) {
            reinterpret_cast<OSObject *>(surface)->retain();
            if (fShown)
                reinterpret_cast<OSObject *>(fShown)->release();
            fShown = surface;
        }
        fRefreshLeft = kRefreshTicks;
    }
    swap_complete_gated();
}

// 8-bit BGRA, 16-bit half-float RGBA, and 'b3a8': two planes, 10-bit extended-range BGR in 32 bits and 8-bit alpha (QuartzCore's
// four_cc_to_mtl_format: Metal's format 550), what WindowServer draws with a deep framebuffer (CADeviceUseCIF10).
static constexpr uint32_t kFormatBGRA = 'BGRA', kFormatRGhA = 'RGhA', kFormatB3A8 = 'b3a8';

// A half float's value in [0, 1] as a byte, in integer arithmetic (no floating point in the kernel).
static uint8_t halfToByte(uint16_t half)
{
    unsigned exponent = (half >> 10) & 31, mantissa = half & 1023;
    if (half & 0x8000 || (exponent == 31 && mantissa))   // negative or NaN
        return 0;
    if (exponent >= 15)                                  // 1.0 and above
        return 255;
    uint32_t scaled = (exponent ? 1024 + mantissa : mantissa) * 255;   // value * 255 = scaled * 2^(exponent - 25), subnormals 2^-24
    unsigned shift = exponent ? 25 - exponent : 24;
    return static_cast<uint8_t>((scaled + (1u << (shift - 1))) >> shift);
}

void DroidVMDisplay::requestCopy(IOSurface *surface, bool refresh, uint64_t work)
{
    reinterpret_cast<OSObject *>(surface)->retain();
    IOLockLock(fCopyLock);
    IOSurface *skipped = fCopyNext;
    if (skipped && refresh && !fCopyRefresh) {   // a swap's surface waiting is newer than the one on screen
        IOLockUnlock(fCopyLock);
        reinterpret_cast<OSObject *>(surface)->release();
        return;
    }
    fCopyNext = surface;
    fCopyRefresh = refresh;
    fCopyWork = work;
    if (!skipped)
        fCopyRequestTime = mach_absolute_time();
    IOLockWakeup(fCopyLock, &fCopyNext, true);
    IOLockUnlock(fCopyLock);
    if (skipped) {
        fCopiesSkipped++;
        reinterpret_cast<OSObject *>(skipped)->release();
    }
}

void DroidVMDisplay::copyThreadMain(void *owner, wait_result_t)
{
    static_cast<DroidVMDisplay *>(owner)->copyLoop();
    thread_terminate(current_thread());
}

void DroidVMDisplay::copyLoop()
{
    IOLockLock(fCopyLock);
    while (!fCopyStop) {
        IOSurface *surface = fCopyNext;
        if (!surface) {
            IOLockSleep(fCopyLock, &fCopyNext, THREAD_UNINT);
            continue;
        }
        bool refresh = fCopyRefresh;
        const uint64_t work = fCopyWork;
        fCopyNext = nullptr;
        uint64_t waited = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - fCopyRequestTime, &waited);
        IOLockUnlock(fCopyLock);
        if (waited > 20000000) {
            fWakeLate++;
            if (waited > fWakeMaxNs)
                fWakeMaxNs = waited;
        }
        if (!droidvm_virtgpu_finished(work)) {
            // Sleep in steps of 100 us (the vCPU goes idle; IOSleep's 1 ms would add half a millisecond a frame), 200 ms at most.
            const uint64_t begin = mach_absolute_time();
            uint64_t took = 0;
            bool drawn = false;
            while (!drawn && took < 200000000) {
                delay_for_interval(100, NSEC_PER_USEC);
                drawn = droidvm_virtgpu_finished(work);
                absolutetime_to_nanoseconds(mach_absolute_time() - begin, &took);
            }
            fGpuWaits++;
            fGpuWaitNs += took;
            if (took > fGpuWaitMaxNs)
                fGpuWaitMaxNs = took;
            if (!drawn)
                fGpuLate++;
        }
        if (!scanOut(surface))
            copySurface(surface, refresh);
        reinterpret_cast<OSObject *>(surface)->release();
        IOLockLock(fCopyLock);
    }
    fCopyThreadDone = true;
    IOLockWakeup(fCopyLock, &fCopyThreadDone, false);
    IOLockUnlock(fCopyLock);
}

// The kernel address of a surface's memory, mapped once for the next copies.
const uint8_t *DroidVMDisplay::surfaceBytes(IOSurface *surface, IOMemoryDescriptor *memory)
{
    SurfaceMap *slot = &fSurfaceMaps[0];
    for (SurfaceMap &entry : fSurfaceMaps) {
        if (entry.surface == surface && entry.map->getMemoryDescriptor() == memory) {
            entry.used = ++fSurfaceMapClock;
            return reinterpret_cast<const uint8_t *>(entry.map->getVirtualAddress());
        }
        if (entry.used < slot->used)
            slot = &entry;
    }
    if (slot->surface) {
        slot->map->release();
        reinterpret_cast<OSObject *>(slot->surface)->release();
        *slot = {};
    }
    IOMemoryMap *map = memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly);
    if (!map)
        return nullptr;
    reinterpret_cast<OSObject *>(surface)->retain();
    *slot = {surface, map, ++fSurfaceMapClock};
    return reinterpret_cast<const uint8_t *>(map->getVirtualAddress());
}

void DroidVMDisplay::stopCopies()
{
    if (!fCopyLock)
        return;
    IOLockLock(fCopyLock);
    fCopyStop = true;
    IOLockWakeup(fCopyLock, &fCopyNext, true);
    while (!fCopyThreadDone)
        IOLockSleep(fCopyLock, &fCopyThreadDone, THREAD_UNINT);
    IOSurface *left = fCopyNext;
    fCopyNext = nullptr;
    IOLockUnlock(fCopyLock);
    if (left)
        reinterpret_cast<OSObject *>(left)->release();
    IOLockFree(fCopyLock);
    fCopyLock = nullptr;
}

void DroidVMDisplay::releaseSurfaceMaps()
{
    for (SurfaceMap &entry : fSurfaceMaps) {
        if (!entry.surface)
            continue;
        entry.map->release();
        reinterpret_cast<OSObject *>(entry.surface)->release();
        entry = {};
    }
}

// virtio-gpu formats: crosvm's B8G8R8X8, and its half-float RGBA ('RGhA', DRM_FORMAT_ABGR16161616F on the host).
static constexpr uint32_t kVirtioFormatBGRX = 2, kVirtioFormatRGhA = 0x52476841;

bool DroidVMDisplay::releaseResource(ScanoutResource &resource)
{
    if (!resource.surface)
        return true;
    struct {VirtioGpuHeader h; uint32_t id, padding;} unref = {{0x102, 0, 0, 0, 0, {}}, resource.id, 0};
    if (fGpu.command(&unref, sizeof(unref)))
        return false;   // the host may still read the pages: they stay prepared
    if (fScanoutResource == resource.id)
        fScanoutResource = 0;
    resource.memory->complete();
    reinterpret_cast<OSObject *>(resource.surface)->release();
    resource = {};
    return true;
}

// The blob resource of a surface: its pages as a guest-handle blob (RESOURCE_CREATE_BLOB, VIRTIO_GPU_BLOB_MEM_GUEST).
DroidVMDisplay::ScanoutResource *DroidVMDisplay::scanoutResource(IOSurface *surface, uint32_t format)
{
    uint64_t width = ioSurfaceWidth(surface), height = ioSurfaceHeight(surface), stride = ioSurfaceBytesPerRow(surface);
    uint64_t offset = ioSurfaceBaseOffset(surface);
    IOMemoryDescriptor *memory = ioSurfaceMemory(surface);
    for (ScanoutResource &resource : fResources) {
        if (resource.surface == surface && resource.memory == memory && resource.format == format && resource.width == width &&
            resource.height == height && resource.stride == stride && resource.offset == offset) {
            resource.used = ++fResourceClock;
            return &resource;
        }
    }
    ScanoutResource *slot = nullptr;   // a free one, else the least recently used one not on screen
    for (ScanoutResource &resource : fResources) {
        if (!resource.surface) {
            slot = &resource;
            break;
        }
        if (resource.id != fScanoutResource && (!slot || resource.used < slot->used))
            slot = &resource;
    }
    if (!slot || !releaseResource(*slot))
        return nullptr;
    uint64_t pixelBytes = format == kVirtioFormatRGhA ? 8 : 4;
    uint64_t bytes = (offset + stride * height + 4095) & ~4095ULL;
    if (!memory || !width || !height || width > 16384 || height > 16384 || stride < width * pixelBytes || stride > 65536 ||
        bytes > memory->getLength() || memory->prepare() != kIOReturnSuccess)
        return nullptr;
    uint32_t count = 0;
    for (uint64_t at = 0; at < bytes;) {
        IOByteCount length = 0;
        uint64_t pa = memory->getPhysicalSegment(at, &length, kIOMemoryMapperNone);
        if (length > bytes - at)
            length = bytes - at;
        if (count && fEntries[count - 1].address + fEntries[count - 1].length == pa && fEntries[count - 1].length + length <= UINT32_MAX) {
            fEntries[count - 1].length += static_cast<uint32_t>(length);   // physically contiguous with the last entry
        } else {
            if (!pa || !length || length > UINT32_MAX || count == VirtioGpu::kMaxEntries || ((pa | length) & 4095)) {
                memory->complete();
                return nullptr;
            }
            fEntries[count++] = {pa, static_cast<uint32_t>(length), 0};
        }
        at += length;
    }
    uint32_t id = DroidVMVirtioGpu::resourceId();   // the 3D contexts' blobs take theirs from the same counter
    struct {VirtioGpuHeader h; uint32_t id, memory, flags, count; uint64_t blob, size;}
        create = {{0x10c, 0, 0, 0, 0, {}}, id, 1 /* VIRTIO_GPU_BLOB_MEM_GUEST */, 8 /* crosvm: a guest handle */, count, 0, bytes};
    if (IOReturn status = fGpu.command(&create, sizeof(create), fEntries, count * sizeof(VirtioGpuEntry))) {
        IOLog("DroidVMDisplay: virtio-gpu blob of %llu bytes in %u entries: %#x\n", bytes, count, status);
        memory->complete();
        return nullptr;
    }
    reinterpret_cast<OSObject *>(surface)->retain();
    *slot = {surface, memory, offset, ++fResourceClock, id, format, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
             static_cast<uint32_t>(stride)};
    IOLog("DroidVMDisplay: scanout resource %u: %llux%llu row %llu offset %llu format %#x, %llu bytes in %u entries\n", id, width,
          height, stride, offset, format, bytes, count);
    return slot;
}

// Stage 2: the host shows the surface (SET_SCANOUT_BLOB when it changes) and blits it now (RESOURCE_FLUSH). False: copy it instead.
bool DroidVMDisplay::scanOut(IOSurface *surface)
{
    if (!fGpu.ready() || fScanoutFailures >= 8)
        return false;
    uint32_t pixelFormat = ioSurfacePixelFormat(surface);
    uint32_t format = pixelFormat == kFormatBGRA ? kVirtioFormatBGRX : pixelFormat == kFormatRGhA ? kVirtioFormatRGhA : 0;
    if (!format)
        return false;
    uint64_t began = mach_absolute_time();
    ScanoutResource *resource = scanoutResource(surface, format);
    IOReturn status = resource ? kIOReturnSuccess : kIOReturnNoResources;
    uint32_t width = resource ? (resource->width < fVideo.v_width ? resource->width : static_cast<uint32_t>(fVideo.v_width)) : 0;
    uint32_t height = resource ? (resource->height < fVideo.v_height ? resource->height : static_cast<uint32_t>(fVideo.v_height)) : 0;
    if (resource && fScanoutResource != resource->id) {
        struct {VirtioGpuHeader h; VirtioGpuRect rect; uint32_t scanout, id, width, height, format, padding, strides[4], offsets[4];}
            scanout = {{0x10d, 0, 0, 0, 0, {}}, {0, 0, width, height}, 0, resource->id, resource->width, resource->height, format, 0,
                       {resource->stride}, {static_cast<uint32_t>(resource->offset)}};
        status = fGpu.command(&scanout, sizeof(scanout), nullptr, 0, false);
        if (!status)
            fScanoutResource = resource->id;
    }
    if (!status) {
        struct {VirtioGpuHeader h; VirtioGpuRect rect; uint32_t id, padding;} flush = {{0x104, 0, 0, 0, 0, {}}, {0, 0, width, height}, resource->id, 0};
        // Unfenced: waiting for the host's blit (5 ms on average, 25 at worst) held this thread while newer swaps arrived, and the
        // frames it had no time for were skipped -- the hitches of a window drag. The host reads a surface WindowServer does not
        // draw into before a later swap replaces it.
        status = fGpu.command(&flush, sizeof(flush), nullptr, 0, false);
    }
    if (status) {
        if (++fScanoutFailures == 8)
            IOLog("DroidVMDisplay: virtio-gpu scanout failed 8 times (last %#x); copying from now on\n", status);
        return false;
    }
    if (pixelFormat != fSurfaceFormat) {
        IOLog("DroidVMDisplay: virtio-gpu scanout of format %#x\n", pixelFormat);
        fSurfaceFormat = pixelFormat;
    }
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    fFlushes++;
    fFlushNs += ns;
    if (ns > fFlushMaxNs)
        fFlushMaxNs = ns;
    return true;
}

void DroidVMDisplay::copySurface(IOSurface *surface, bool refresh)
{
    uint64_t width = ioSurfaceWidth(surface), height = ioSurfaceHeight(surface), rowBytes = ioSurfaceBytesPerRow(surface);
    uint64_t base = ioSurfaceBaseOffset(surface);
    uint32_t format = ioSurfacePixelFormat(surface);
    IOMemoryDescriptor *memory = ioSurfaceMemory(surface);
    if (format != fSurfaceFormat || !fFrames) {
        IOLog("DroidVMDisplay: surface %llux%llu row %llu offset %llu format %#x memory %p\n", width, height, rowBytes, base, format, memory);
        for (uint32_t plane = 0, planes = ioSurfacePlaneCount(surface); plane < planes; plane++)
            IOLog("DroidVMDisplay:  plane %u: %llux%llu offset %llu row %llu element %llu\n", plane, ioSurfacePlaneWidth(surface, plane),
                  ioSurfacePlaneHeight(surface, plane), ioSurfacePlaneOffset(surface, plane), ioSurfacePlaneBytesPerRow(surface, plane),
                  ioSurfacePlaneBytesPerElement(surface, plane));
        fSurfaceFormat = format;
    }
    if (format == kFormatB3A8) {   // the colour plane; alpha is not shown
        if (ioSurfacePlaneCount(surface) < 1 || ioSurfacePlaneBytesPerElement(surface, 0) != 4)
            return;
        base = ioSurfacePlaneOffset(surface, 0);
        rowBytes = ioSurfacePlaneBytesPerRow(surface, 0);
    }
    uint64_t pixelBytes = format == kFormatRGhA ? 8 : 4;
    if (!memory || !fScanout || rowBytes < width * pixelBytes || (format != kFormatBGRA && format != kFormatRGhA && format != kFormatB3A8))
        return;
    uint64_t copyWidth = width < fVideo.v_width ? width : fVideo.v_width;
    uint64_t copyHeight = height < fVideo.v_height ? height : fVideo.v_height;
    if (!fLine) {
        fRowBytes = fVideo.v_width * 8;
        fRow = static_cast<uint16_t *>(IOMalloc(fRowBytes));
        fLine = static_cast<uint32_t *>(IOMalloc(fVideo.v_width * 4));
        fHalfToByte = static_cast<uint8_t *>(IOMalloc(16384));
        if (!fRow || !fLine || !fHalfToByte) {
            if (fRow)
                IOFree(fRow, fRowBytes);
            if (fLine)
                IOFree(fLine, fVideo.v_width * 4);
            if (fHalfToByte)
                IOFree(fHalfToByte, 16384);
            fRow = nullptr;
            fLine = nullptr;
            fHalfToByte = nullptr;
            return;
        }
        // Indexed by the half float >> 2 (16 KiB, in the L1 cache where all 64 KiB were not): 8 bits of mantissa left, finer than
        // a byte's step anywhere in [0, 1].
        for (uint32_t index = 0; index < 16384; index++)
            fHalfToByte[index] = halfToByte(static_cast<uint16_t>(index << 2 | 2));
        for (uint32_t value = 0; value < 1024; value++)   // extended range: 384 is 0.0, 894 is 1.0
            fXr10ToByte[value] = value <= 384 ? 0 : value >= 894 ? 255 : static_cast<uint8_t>((value - 384) * 255 / 510);
    }
    uint64_t began = mach_absolute_time(), mark = began, now;
    auto lap = [&](uint64_t &total) {
        now = mach_absolute_time();
        uint64_t ns;
        absolutetime_to_nanoseconds(now - mark, &ns);
        total += ns;
        mark = now;
    };
    if (memory->prepare() != kIOReturnSuccess)
        return;
    const uint8_t *bytes = surfaceBytes(surface, memory);
    lap(fPrepareNs);
    if (!bytes) {
        memory->complete();
        return;
    }
    uint64_t lit = 0, raw = 0, center = 0;
    bool sample = !refresh;   // bring-up: the first frames and every 50th are logged, the last one goes into the periodic report
    for (uint64_t y = 0; y < copyHeight; y++) {
        uint8_t *line = fScanout + y * fVideo.v_rowBytes;
        const uint8_t *source = bytes + base + y * rowBytes;
        if (format == kFormatBGRA) {
            memcpy(fLine, source, copyWidth * 4);
            lap(fReadNs);
            memcpy(line, fLine, copyWidth * 4);
            lap(fWriteNs);
            continue;
        }
        if (format == kFormatB3A8) {   // blue in the low bits, as BGR10A2
            memcpy(fLine, source, copyWidth * 4);
            lap(fReadNs);
            for (uint64_t x = 0; x < copyWidth; x++) {
                uint32_t bgr = fLine[x];
                fLine[x] = 0xff000000u | fXr10ToByte[bgr >> 20 & 1023] << 16 | fXr10ToByte[bgr >> 10 & 1023] << 8 | fXr10ToByte[bgr & 1023];
                if (sample) {
                    lit += (fLine[x] & 0xffffff) != 0;
                    raw += bgr != 0;
                    if (y == copyHeight / 2 && x == copyWidth / 2)
                        center = bgr;
                }
            }
            lap(fConvertNs);
            memcpy(line, fLine, copyWidth * 4);
            lap(fWriteNs);
            continue;
        }
        memcpy(fRow, source, copyWidth * 8);
        lap(fReadNs);
        const uint64_t *pixels = reinterpret_cast<const uint64_t *>(fRow);
        for (uint64_t x = 0; x < copyWidth; x++) {
            uint64_t rgba = pixels[x];
            fLine[x] = 0xff000000u | fHalfToByte[rgba >> 2 & 0x3fff] << 16 | fHalfToByte[rgba >> 18 & 0x3fff] << 8 |
                       fHalfToByte[rgba >> 34 & 0x3fff];
            if (sample) {
                lit += (fLine[x] & 0xffffff) != 0;
                raw += rgba != 0;
                if (y == copyHeight / 2 && x == copyWidth / 2)
                    center = rgba;
            }
        }
        lap(fConvertNs);
        memcpy(line, fLine, copyWidth * 4);
        lap(fWriteNs);
    }
    memory->complete();
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    fCopies++;
    fCopyNs += ns;
    if (ns > fCopyMaxNs)
        fCopyMaxNs = ns;
    if (refresh)
        return;
    fLastLit = lit;
    fLastCenter = center;
    if (format != kFormatBGRA && (fFrames <= 8 || fFrames % 50 == 0))
        IOLog("DroidVMDisplay: frame %llu: %llu pixels lit, %llu nonzero, centre %#llx\n", fFrames, lit, raw, center);
}

static bool appendNumber(OSDictionary *dictionary, const char *key, uint64_t value, unsigned bits = 32)
{
    OSNumber *number = OSNumber::withNumber(value, bits);
    bool done = number && dictionary->setObject(key, number);
    OSSafeReleaseNULL(number);
    return done;
}

static bool appendString(OSDictionary *dictionary, const char *key, const char *value)
{
    OSString *string = OSString::withCString(value);
    bool done = string && dictionary->setObject(key, string);
    OSSafeReleaseNULL(string);
    return done;
}

static bool appendDictionary(OSDictionary *dictionary, const char *key, OSDictionary *value)
{
    bool done = value && dictionary->setObject(key, value);
    OSSafeReleaseNULL(value);
    return done;
}

static OSDictionary *point(uint32_t x, uint32_t y)
{
    OSDictionary *dictionary = OSDictionary::withCapacity(3);
    if (dictionary && !(appendNumber(dictionary, "X", x) && appendNumber(dictionary, "Y", y)))
        OSSafeReleaseNULL(dictionary);
    return dictionary;
}

// AppleParavirtDisplay::createDisplayAttributes, with the display of the boot framebuffer: one timing (the GOP mode) with one
// 8-bit SDR color element. Chromaticities are sRGB's (16.16 fixed point, as the panel's EDID would give them).
bool DroidVMDisplay::createDisplayAttributes()
{
    uint32_t width = static_cast<uint32_t>(fVideo.v_width), height = static_cast<uint32_t>(fVideo.v_height);
    bool done = false;
    OSDictionary *attributes = OSDictionary::withCapacity(16), *product = OSDictionary::withCapacity(8);
    OSDictionary *chromaticity = OSDictionary::withCapacity(3), *whitePoint = point(20493, 21561);
    OSDictionary *transport = OSDictionary::withCapacity(2), *sourceLimits = OSDictionary::withCapacity(5);
    OSDictionary *scalingLimits = OSDictionary::withCapacity(2);
    OSArray *colors = OSArray::withCapacity(1), *timings = OSArray::withCapacity(1), *timingColors = OSArray::withCapacity(1);
    OSObject *color = ioavColorWithCapabilities(2, 8, 0, 0, 0, 16, false, nullptr, nullptr, nullptr, nullptr);
    // CVT reduced blanking as IOAVFamily's sCVTTimingData has it (1280x800@60: 1440x823, 71 MHz). Sync rates are 16.16 fixed point
    // (IOAVVideoTimingGetSyncRateRounded); the table leaves the horizontal one 0.
    uint8_t timingData[80] = {};
    uint32_t hTotal = width + 160, vTotal = height + 23;
    at<uint32_t>(timingData, 4) = hTotal;
    at<uint32_t>(timingData, 8) = width;
    at<uint32_t>(timingData, 12) = 32;                   // horizontal sync width, back porch, front porch
    at<uint32_t>(timingData, 16) = 80;
    at<uint32_t>(timingData, 20) = 48;
    at<uint32_t>(timingData, 28) = 1;                    // horizontal sync polarity (positive)
    at<uint32_t>(timingData, 36) = vTotal;
    at<uint32_t>(timingData, 40) = height;
    at<uint32_t>(timingData, 44) = 6;                    // vertical sync width, back porch, front porch
    at<uint32_t>(timingData, 48) = 14;
    at<uint32_t>(timingData, 52) = 3;
    at<uint32_t>(timingData, 56) = fRefreshHz << 16;
    at<uint32_t>(timingData, 68) = hTotal * vTotal * fRefreshHz;   // pixel clock in Hz
    OSObject *timing = ioavTimingWithData(timingData, 2, 4);
    do {
        if (!attributes || !product || !chromaticity || !whitePoint || !transport || !sourceLimits || !scalingLimits || !colors ||
            !timings || !timingColors || !color || !timing)
            break;
        at<uint64_t>(color, kElementID) = kColorID;
        at<uint32_t>(color, kElementScore) = kColorID;
        at<uint64_t>(timing, kElementID) = kTimingID;
        at<uint32_t>(timing, kElementScore) = kTimingID;
        if (!colors->setObject(color) || !timingColors->setObject(color) || !timings->setObject(timing))
            break;
        ioavTimingSetColorElements(timing, timingColors);
        if (!(appendString(product, "ManufacturerID", "APP") && appendNumber(product, "ProductID", 0) &&
              appendNumber(product, "SerialNumber", 0) && appendString(product, "ProductName", "DroidVM Display") &&
              appendNumber(product, "WeekOfManufacture", 24) && appendNumber(product, "YearOfManufacture", 2021)))
            break;
        if (!(appendDictionary(chromaticity, "Red", point(41943, 21627)) && appendDictionary(chromaticity, "Green", point(19661, 39322)) &&
              appendDictionary(chromaticity, "Blue", point(9830, 3932))))
            break;
        if (!appendNumber(whitePoint, "Gamma", 144179))   // 2.2
            break;
        if (!(attributes->setObject("SupportsPQEOTF", kOSBooleanFalse) && attributes->setObject("ProductAttributes", product) &&
              attributes->setObject("Chromaticity", chromaticity) && attributes->setObject("DefaultColorSpaceIsSRGB", kOSBooleanFalse) &&
              attributes->setObject("DefaultWhitePoint", whitePoint) && attributes->setObject("SupportsStandby", kOSBooleanTrue) &&
              appendNumber(attributes, "ContinuousFrequencySupport", 3) &&
              appendNumber(attributes, "MaxHorizontalImageSize", (kWidthMM + 5) / 10) &&
              appendNumber(attributes, "MaxVerticalImageSize", (kHeightMM + 5) / 10) &&
              appendNumber(attributes, "NativeFormatHorizontalPixels", width) && appendNumber(attributes, "NativeFormatVerticalPixels", height) &&
              appendNumber(attributes, "PreciseAspectRatio", (static_cast<uint64_t>(width) << 16) / height)))
            break;
        if (!(appendString(transport, "Upstream", "Other") && appendString(transport, "Downstream", "Other")))
            break;
        if (!(appendNumber(sourceLimits, "MaxSrcBufferWidth", 16384) && appendNumber(sourceLimits, "MaxSrcBufferHeight", 16384) &&
              appendNumber(sourceLimits, "MaxSrcRectWidth", 8192) && appendNumber(sourceLimits, "MaxSrcRectTotal", 0) &&
              appendNumber(sourceLimits, "IOMFBMaxCompressedSizeInBytes", 0)))
            break;
        if (!(appendNumber(scalingLimits, "RGBLayer_MinScaleFraction", 4) && appendNumber(scalingLimits, "RGBLayer_MaxScale", 4)))
            break;
        setProperty("IOMFBUUID", "44726F69-6456-4D00-0000-000000000001");
        if (OSDictionary *prefs = OSDictionary::withCapacity(0)) {   // paravirtualized for WindowServer (see the class registration)
            setProperty("ParavirtDisplayPrefs", prefs);
            prefs->release();
        }
        setProperty("external", true);
        setProperty("primary", 0ULL, 32);   // AppleParavirtDisplay's display index
        done = setProperty("DisplayAttributes", attributes) && setProperty("Transport", transport) &&
            setProperty("ColorElements", colors) && setProperty("TimingElements", timings) &&
            setProperty("IOMFBMaxSrcPixels", sourceLimits) && setProperty("IOMFBScalingLimits", scalingLimits);
    } while (false);
    OSSafeReleaseNULL(attributes);
    OSSafeReleaseNULL(product);
    OSSafeReleaseNULL(chromaticity);
    OSSafeReleaseNULL(whitePoint);
    OSSafeReleaseNULL(transport);
    OSSafeReleaseNULL(sourceLimits);
    OSSafeReleaseNULL(scalingLimits);
    OSSafeReleaseNULL(colors);
    OSSafeReleaseNULL(timings);
    OSSafeReleaseNULL(timingColors);
    OSSafeReleaseNULL(color);
    OSSafeReleaseNULL(timing);
    if (!done || set_digital_out_mode(kColorID, kTimingID) != kIOReturnSuccess) {
        IOLog("DroidVMDisplay: createDisplayAttributes failed\n");
        return false;
    }
    return true;
}

// The mode of a color and a timing element (the IDs published above); AppleParavirtDisplay's block takes the timing's active size.
IOReturn DroidVMDisplay::set_digital_out_mode(unsigned int colorID, unsigned int timingID)
{
    if (colorID != kColorID || timingID != kTimingID)
        return kIOReturnBadArgument;
    at<uint32_t>(this, kDisplayWidth) = static_cast<uint32_t>(fVideo.v_width);
    at<uint32_t>(this, kDisplayHeight) = static_cast<uint32_t>(fVideo.v_height);
    setProperty("DisplayWidth", fVideo.v_width, 32);
    setProperty("DisplayHeight", fVideo.v_height, 32);
    setProperty("DPTimingModeId", timingID, 32);
    return kIOReturnSuccess;
}

// AppleParavirtDisplay: dots per inch from the physical width (IOMobileFramebufferLegacy's reads "dot-pitch" of the AppleARMIODevice).
IOReturn DroidVMDisplay::get_dot_pitch(unsigned int *pitch)
{
    *pitch = static_cast<unsigned int>(fVideo.v_width * 25.46 / kWidthMM + 0.5);
    return kIOReturnSuccess;
}

// AppleParavirtDisplay's hooks with nothing to do on a CPU-copied scanout: the swap and its layers need no mapping, there is no
// gamma or color hardware (the family's defaults are unsupported, AppleParavirtDisplay accepts them).
IOReturn DroidVMDisplay::verify_swap(IOMFBSwapIORequest *)   // AppleParavirtDisplay returns 1 (IOMobileFramebufferLegacy: 0)
{
    if (!fSubmitted++)
        IOLog("DroidVMDisplay: first swap submitted\n");
    return 1;
}
IOReturn DroidVMDisplay::swap_layer_map(IOMFBSwapIORequest *, unsigned int) {return kIOReturnSuccess;}
IOReturn DroidVMDisplay::surface_layer_map(IOMFB::AP::DisplayedData *, unsigned int, IOSurface *) {return kIOReturnSuccess;}
IOReturn DroidVMDisplay::surface_layer_unmap(IOMFB::AP::DisplayedData *, unsigned int) {return kIOReturnSuccess;}
IOReturn DroidVMDisplay::set_display_device(unsigned int) {return kIOReturnSuccess;}
IOReturn DroidVMDisplay::get_gamma_table(IOMFBGammaTable *) {return kIOReturnUnsupported;}
IOReturn DroidVMDisplay::set_gamma_table(IOMFBGammaTable *) {return kIOReturnSuccess;}
IOReturn DroidVMDisplay::set_color_remap_mode(DisplayColorRemapMode mode)
{
    fColorRemapMode = mode;
    return kIOReturnSuccess;
}
IOReturn DroidVMDisplay::get_color_remap_mode(DisplayColorRemapMode *mode) const
{
    *mode = fColorRemapMode;
    return kIOReturnSuccess;
}
IOReturn DroidVMDisplay::swap_set_color_matrix(IOMFBColorFixedMatrix *, IOMFBColorMatrixFunction, unsigned int) {return kIOReturnSuccess;}

// The slots IOMobileFramebufferLegacy leaves pure, as AppleParavirtDisplay implements them.
IOReturn DroidVMDisplay::set_notify_tag_mask_gated(IOMFB_NotificationType type, IOMFBNotifyRequest *request, unsigned int mask)
{
    if (type != 3)
        return kIOReturnBadArgument;
    at<uint32_t>(request, 120) = mask;
    return kIOReturnSuccess;
}
IOReturn DroidVMDisplay::supported_frame_info(unsigned int, unsigned int *, char (*)[64], unsigned int *) {return kIOReturnUnsupported;}
IOReturn DroidVMDisplay::factory_portal(void *, task *) {return kIOReturnUnsupported;}
IOReturn DroidVMDisplay::executeFactoryConfigurations_gated() {return kIOReturnUnsupported;}
IOReturn DroidVMDisplay::set_frame_info_notification_state_gated(IOMFBNotifyRequest *, bool) {return kIOReturnSuccess;}

// AppleParavirtDisplay::notify_swap_done_gated: the family calls it as the current swap completes; the swap info goes to the
// clients (swap_info_notify_dispatch_gated, which calls do_swap_info_notify_gated per request) unless notifications are held.
IOReturn DroidVMDisplay::notify_swap_done_gated(unsigned int swapID, ::SwapInfoBlob *, unsigned int *, unsigned int)
{
    IOMFBSwapIORequest *swap = at<IOMFBSwapIORequest *>(this, kCurrentSwap);
    if (at<uint32_t>(swap, kSwapDropReasons))
        return kIOReturnUnsupported;
    SwapInfo *info = &fSwapInfo[fSwapInfoWrite];
    *info = {swapID, {at<uint64_t>(this, kSwapInfoTime), 0, at<uint64_t>(swap, kSwapInfoA), at<uint32_t>(swap, kSwapInfoB), 0}};
    if (at<uint16_t>(this, kSwapInfoHeld)) {
        fSwapInfoWrite = (fSwapInfoWrite + 1) % kSwapInfoRing;
        return kIOReturnSuccess;
    }
    for (; fSwapInfoRead != fSwapInfoWrite; fSwapInfoRead = (fSwapInfoRead + 1) % kSwapInfoRing)
        iomfbSwapInfoDispatch(this, &fSwapInfo[fSwapInfoRead]);
    iomfbSwapInfoDispatch(this, info);
    return kIOReturnSuccess;
}

// AppleParavirtDisplay::do_swap_info_notify_gated: the request's async result is the mask, the swap ID and the words it asks for.
IOReturn DroidVMDisplay::do_swap_info_notify_gated(IOMFBNotifyRequest *request, const SwapInfoBlob *blob)
{
    const SwapInfo *info = reinterpret_cast<const SwapInfo *>(blob);
    uint32_t mask = at<uint32_t>(request, kNotifyMask);
    io_user_reference_t args[12] = {mask, info->swapID};
    unsigned count = 2;
    for (unsigned word = 0; word < 5; word++) {
        if (!(mask >> word & 1))
            continue;
        uint64_t value = word == 1 ? info->words[0] - at<uint64_t>(request, kNotifyLastTime) : info->words[word];
        unsigned need = mask >> 31 ? 2 : 1;
        if (count + need > 12) {
            IOLog("IOMFB Packer put: ran out of space\n");
            continue;
        }
        if (need == 2) {
            args[count++] = static_cast<uint32_t>(value);
            args[count++] = value >> 32;
        } else {
            args[count++] = value;
        }
    }
    at<uint64_t>(request, kNotifyLastTime) = info->words[0];
    return userClientSendAsyncResult(&at<io_user_reference_t>(request, kNotifyAsyncReference), kIOReturnSuccess, args, count,
                                     kIOUserNotifyOptionCanDrop);
}
IOReturn DroidVMDisplay::notify_crc_ready_gated(unsigned int) {return kIOReturnSuccess;}
bool DroidVMDisplay::are_timings_enabled() const {return false;}

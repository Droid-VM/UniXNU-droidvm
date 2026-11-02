// gfxprobe: make the IOSurfaces the system's renderers make, inside the guest, and ask the Metal device for a texture on each.
//
//   gfxprobe rb W H FORMAT[:FLAGS]...   RenderBox's own RB::create_iosurface(MTLPixelFormat, w, h, flags) (what an RBLayer --
//                                       SwiftUI in a widget -- draws into), then newTextureWithDescriptor:iosurface:plane:0
//   gfxprobe ca W H FOURCC...           QuartzCore's CAIOSurfaceCreateDefault(w, h, fourcc), the same texture request
//   gfxprobe plain W H FOURCC...        a plain IOSurfaceCreate (width, height, 4 bytes per element, the pixel format)
//   gfxprobe open W H SERVICE...        IOServiceOpen of each IOKit service class (type 0): the kern_return_t
//   gfxprobe cg W H FOURCC...           CoreGraphics into a surface as WebKit paints a layer: CAIOSurfaceCreateDefault, then
//                                       CGIOSurfaceContextCreate with WebKit's parameters for the format ('BGRA', 'b3a8' RGB10A8,
//                                       'w30r' RGB10), a red fill and a flush; the bytes of each plane that are not 0 after
//   gfxprobe user UID VERB ARG...      the verb as that user, without a sandbox (ethernet: what Settings asks before it lists Ethernet)
//   gfxprobe clocks                     mach and POSIX clocks side by side, and a 1 s dispatch timer on uptime, wall and monotonic time
//   gfxprobe scnotify KEY...            configd tells the SCDynamicStore sessions watching each key that it changed (value untouched)
//   gfxprobe families                   each GPU family the Metal device answers (supportsFamily:), and the queries a family check uses
//   gfxprobe msaa [W H DRAWS FRAMES]    the GPU time of one screen-sized pass (default 2048 x 2732, 64 overlapping full-screen
//                                       triangles, 30 frames) at 1 sample, at 4 into a memoryless multisample target resolved, and
//                                       at 4 into a private one resolved (TU_DEBUG=gmem or sysmem forces Turnip's render mode)
//   gfxprobe dontcare [W H]             what an attachment stored DontCare or MultisampleResolve holds after its pass (metal2vulkan
//                                       hands DONT_CARE only to a driver measured to keep one of Metal's two answers: every
//                                       texel and sample the earlier one or the one Store keeps): BGRA8 at 1 and 4 samples
//                                       (resolved and not), RGBA16Float, Depth32Float at 1 and 4, each loaded Clear, Load and
//                                       DontCare, drawing nothing, the left half or everything; counts per answer, a third
//                                       answer listed
//   gfxprobe vtdecode [hw] [paravirt] [FILE [N]]
//                                       VideoToolbox decodes one 64x64 H.264 IDR frame (embedded), or the first N access units
//                                       of an Annex B H.264 or HEVC FILE: the session's decoder, each frame's status, the first
//                                       frame's format and a few luma samples, frames out and time; hw requires a hardware
//                                       decoder (the paravirtualized one counts); paravirt first publishes the kernel's
//                                       paravirtualization service (kern.droidvm_vt_paravirt=1, DroidVMVTParavirt.cpp)
//   gfxprobe vtencode [paravirt] [hevc] [W H N]
//                                       VideoToolbox encodes N synthetic NV12 frames (default 1280x720, 30): each frame's size
//                                       and key-frame flag, the time, and the stream as Annex B in /private/var/ngpu/enc.h264
//                                       (or .hevc); paravirt publishes the paravirtualization service first and asks for the
//                                       paravirtualized encoder (DroidVMVTHost.cpp)
//   gfxprobe argbuf                     an argument buffer (a buffer, a texture, a sampler, a constant) encoded by a descriptor-made and a
//                                       function-made MTLArgumentEncoder, read by a kernel and by a fragment shader
//   gfxprobe sysctl [NAME...]           sysconf's memory figures and each sysctl (in a sandbox: which the profile refuses)
//   gfxprobe box PROFILE[:UID] VERB ARG...  the verb inside the named sandbox profile (sandbox_init), e.g. com.apple.WebKit.GPU:501:
//                                       what a sandboxed process can do with the GPU; whether the plugin's files and its
//                                       shader-cache directories can be read and written
//   gfxprobe coherence W H ROUNDS [MODE] the CPU fills a BGRA surface with a new pattern each round and the GPU copies it to a buffer
//                                       (a blit of a texture over the surface): the words the GPU did not see as the CPU left them
//   gfxprobe gpubw W H ROUNDS [KIND...] the GPU's copy rate over each kind of memory: two W x H BGRA images of a KIND (private and
//                                       shared Metal textures, the device's own pages; iosurface, iosurface-wc and ca, IOSurfaces
//                                       whose pages the GPU takes from the process; buffer), then ROUNDS copies between them in one
//                                       command buffer by blit, by a draw that reads one into the other as CoreAnimation composites a
//                                       layer, and by a 9-tap horizontal blur; GPU time (GPUStartTime..GPUEndTime), wall time, GB/s
//   gfxprobe rotbench W H PASSES ROT [CLEAR]  CoreAnimation's framebuffer: a W x H framebuffer-only render target of rotation ROT
//                                       (0-3) over an IOSurface of the extent it stores, and PASSES render passes into it in one
//                                       command buffer, each loading it (CLEAR=1: clearing), drawing a full-frame triangle and storing:
//                                       the GPU time per pass, against ROT 0 for what the turned copies cost; then every texel of
//                                       the surface checked against where Metal stores the frame's texel (x, y) for ROT, and the
//                                       texels that land as each other turn or mirror would put them, to name a wrong one
//   gfxprobe cpu SECONDS [THREADS]      CPU time each process used over SECONDS (proc_pid_rusage), busiest first, as a share of one
//                                       CPU; then the threads (PROC_PIDTHREADINFO, named where they have a name) of the THREADS
//                                       (default 3) busiest processes: what the guest's vCPUs run during an animation
//   gfxprobe display                    what CoreAnimation makes of the main display: CADisplay's refresh rates, minimum frame
//                                       duration, modes and VRR, as QuartzCore answers them (KVC, each key that answers)
//   gfxprobe mg [KEY...]                MobileGestalt's answers (MGCopyAnswer) to KEY, by default the main screen's: its scale,
//                                       size, class, the per-display extended properties QuartzCore takes the display's scale from
//                                       (DisplayExtendedProperties[i] DisplayScale before main-screen-scale) and the artwork traits
//   gfxprobe sbscreen                   what SpringBoard picks its home screen layout from: the screen type SpringBoardHome
//                                       looks up (SBHScreenTypeForCurrentDevice) and its inputs (the effective device class, home
//                                       button type, artwork subtype, corner radius, and the main screen's reference bounds and scale,
//                                       which outside SpringBoard come from UIKit's main screen as printed)
//   gfxprobe wired [N]                  where the kernel's wired memory is (mach_memory_info, as zprint reads it; root's host port
//                                       is the privileged one): the N (default 40) largest allocation sites and tags, the zones in
//                                       all, and vm_statistics64's wired, free, active, inactive and compressor pages
//   gfxprobe sync PATH...               F_FULLFSYNC each file, then sync(): a file staged into the Data overlay (export.py put)
//                                       reaches the store before the VM stops (vm.sh stop is no clean shutdown)
//   gfxprobe msl SOURCE OUT [OPTIONS]   compile Metal source with the system's own front end (MTLCompiler, request 13, as Metal's
//                                       newLibraryWithSource: does through MTLCompilerService) and write the metallib it returns
//
//   gfxprobe ci [SIZE [DIR]]            Core Image on the Metal device against its software renderer: blue over red through a
//                                       horizontal gradient mask (CIBlendWithAlphaMask, how IconServices lays a graphic icon's glyph
//                                       over its fill), a Gaussian blur of that, and an icon-like image (rounded rectangle, a glyph)
//                                       drawn at SIZE and scaled from twice SIZE (Lanczos, affine); the largest channel difference of
//                                       each, the rows that differ and a few pixels; DIR gets each render as raw RGBA (ci-N-metal.rgba,
//                                       ci-N-software.rgba). CI_ENABLE_FUNCTION_STITCHING=0 takes Core Image's other kernel path
//
//   gfxprobe orient N                   tell backboardd the device is held at UIDeviceOrientation N (1 portrait, 2 upside down,
//                                       3 landscape left, 4 landscape right), as XCTest rotates a device; backboardd takes it only
//                                       with com.apple.backboardd.setDeviceOrientation. The guest's panel is landscape-native, so
//                                       iPadOS's portrait (1) is its wide, upright screen and 3 and 4 turn everything sideways
//
//   gfxprobe orient-follow              stay running and keep backboardd's device orientation where the guest's motion sensors say
//                                       the device is held (kern.droidvm_motion, DroidVMMotion.c): an owner service, started at
//                                       boot by ownervisor. Lying flat, or between two orientations, changes nothing
//
//   gfxprobe memcheck MIB SECONDS       a process that never touches the GPU keeps MIB of anonymous memory, each word holding its
//                                       own address mixed with its chunk's salt, and checks all of it again and again for SECONDS;
//                                       every round a sixteenth of the chunks goes back to the kernel and comes anew, so the process
//                                       keeps taking pages others gave back. A changed word is a write from outside the process (a
//                                       GPU or the host writing pages the guest handed to someone else, a kernel bug): each is
//                                       printed with what it held, what it should hold and, decoded, the address its value names
//
//   gfxprobe memidle MIB SECONDS PERIOD the other half: MIB of compressible content (mostly zeros and repeated small values, as a
//                                       heap holds) left alone, checked every PERIOD seconds for SECONDS. Pages nobody touches are
//                                       what the pageout daemon compresses or frees under memory pressure, so a page that comes back
//                                       different points at the compressor or at a mapping the pageout path tore down; each bad page
//                                       is printed with how many of its words differ and the first of them
//
//   gfxprobe regcheck THREADS SECONDS   a thread's own state across preemption: THREADS threads (more than the CPUs, so they are
//                                       preempted) each load a pattern of their own into v0-v31, spin, and read the registers
//                                       back, and check that TPIDRRO_EL0 (the TSD base) and pthread_self() stay theirs and that
//                                       TPIDR_EL0's CPU number is one the system has; each mismatch is printed. A kernel that
//                                       restores another thread's SIMD registers or TSD base corrupts any process's memory at
//                                       random (memcpy, malloc and locks run on them)
//
//   gfxprobe atomcheck THREADS SECONDS  atomics between CPUs: THREADS threads add to the same counters on a fresh page each round
//                                       (an atomic add, a compare-and-swap loop, and a plain add under an os_unfair_lock), then
//                                       the totals are compared with what was added; each short total is printed with the page's
//                                       physical-looking address. A round sleeps a little, so the vCPUs idle (vmguard)
//
//   gfxprobe filecheck DIR SECONDS      the read path: every regular file under DIR read again and again with F_NOCACHE (from the
//                                       device each time, past the buffer cache), a hash of each 1 MiB compared with the first
//                                       pass's; each chunk that reads differently is printed (file, offset, both hashes). A block
//                                       path that returns other data now and then feeds parsers garbage (GFXBench's assets, dyld's
//                                       __LINKEDIT), which then overrun their own memory
//
//   gfxprobe tsd                        this new process's main-thread TSD as it starts, before main's autorelease pool or any
//                                       Objective-C: every nonzero slot of the first 256. The main thread's pthread and TSD are
//                                       libsystem_pthread's _main_thread, in a shared-cache __DATA page every process copies on
//                                       its first write; a slot objc sets lazily (40-49) holding a value here means that page's
//                                       shared source was written, and every new process starts with it
//
//   gfxprobe regfault THREADS SECONDS   regcheck's v0-v31 held across a system call (getpid, svc) and a page fault (the first store
//                                       to a page just mapped), the two ways into the kernel regcheck's spin did not take; each
//                                       register that comes back different is printed. memcpy holds its data in these registers
//                                       while its stores fault in new pages
//
//   gfxprobe machcheck SECONDS          the kernel's mach message receive against the receiver's buffer: messages of every size from
//                                       24 bytes to 16 KiB (4-byte steps) sent to a port of this process and received with each
//                                       trailer a receiver may ask for (basic, seqno, sender, audit, ctx, av) into exactly the
//                                       room they need, 256 canary bytes after it; a byte past the room that changed, a size that
//                                       differs, or a receive one byte too small that is not refused, is printed
//
//   gfxprobe reusecheck MIB PRESSURE SECONDS  pages marked reusable and written again: MIB of patterned memory, all of it
//                                       madvise(MADV_FREE_REUSABLE), the first half written again at once (how a workqueue
//                                       thread's stack is reused: the kernel marks a parked thread's stack reusable and the thread
//                                       writes it when it runs again), the second half MADV_FREE_REUSE'd first (how libmalloc
//                                       reuses), then PRESSURE MiB touched and given back so the pageout daemon reclaims, and both
//                                       halves checked. A page the kernel discarded after it was written again reads as zeros:
//                                       the modified state of a reusable page was lost (refmod tracking, a stale writable TLB entry).
//                                       A control region of the same size is marked reusable and never written again: its pages
//                                       coming back zero show the kernel did reclaim reusable pages meanwhile (without that the
//                                       test proves nothing)
//
//   gfxprobe reusethreads MIB PRESSURE SECONDS THREADS  the same with THREADS workers, each writing its share from its own
//                                       CPU before and after the main thread marks the region reusable: the TLB maintenance of
//                                       the mark comes from another CPU than the writes after it
//
//   gfxprobe textcheck SECONDS PERIOD   stray writes into memory nothing writes: the dyld shared cache's pages this process maps
//                                       read-only (__TEXT, __DATA_CONST, __LINKEDIT; every process shares them). Each PERIOD the
//                                       pages resident now (mincore) are hashed; a page whose hash differs from the one it had when
//                                       first seen resident is printed and its bytes saved to /private/var/tmp/textcheck-ADDR-N.bin
//                                       (a wrong page-in reads the same as a stray write: both are bugs). Of a __DATA_CONST page (one
//                                       that could be made writable: GOTs, objc metadata) the first copy is kept, and the words that
//                                       changed are printed, old and new. Unslid = ADDR - slide.
//
//   gfxprobe asidcheck PROCS SECONDS    a process seeing another's memory through a stale TLB entry: PROCS processes (gfxprobe
//                                       asidchild) each map a page at the same address and fill it with their own tag, then read
//                                       and rewrite it while they switch in and out on every CPU. A tag that is not the reader's is
//                                       printed (whose it was): the reader translated the address with another process's entry
//                                       (two address spaces sharing a hardware ASID, one of them flushed on a different CPU)
//
//   gfxprobe zerocheck MIB SECONDS      fresh pages that are not zero: MIB of anonymous memory mapped again and again, each page's
//                                       first word written (the zero-fill fault; every other page read first, which maps the zero
//                                       page and copies it on the write), the rest of the page checked for zero. A page that is not
//                                       is printed with its first non-zero words: the content of its last owner, handed out without
//                                       being cleared, or written after it was cleared
//
//   gfxprobe datacheck SECONDS PERIOD_MS  another process's writes in this one's own data: every writable data segment of the
//                                       loaded images (__DATA, __DATA_DIRTY, __AUTH of the shared cache's dylibs and this
//                                       program's) copied once, then compared every PERIOD_MS while this process only sleeps (it
//                                       writes none of them; the report is made with write(2) and no stdio). Each changed word is
//                                       printed with its image, segment, offset and old and new value
//
//   gfxprobe cowcheck PROCS LIFE_MS SECONDS  another process's copy of a shared-cache data page in this one: PROCS processes at
//                                       a time (gfxprobe cowchild), each LIFE_MS old when it exits and replaced by a new one, watch
//                                       their own __stack_chk_guard (libsystem_c __DATA_DIRTY; the kernel gives every process its
//                                       own) and environ pointer. A guard or pointer that changes is printed, old and new: the
//                                       page now is another process's (a GFXBench __stack_chk_fail showed exactly that). Each
//                                       child prints START with its pid and guard as it begins and END as it ends, so a foreign
//                                       guard names the process it came from and whether that one had ended; the child also keeps
//                                       a copy of the pages holding its guard, its environ pointer and os_alloc_once's table and
//                                       prints, per page, how many words differ from it and the first few
//
//   gfxprobe tess                       Metal tessellation, drawn and checked: a triangle patch and a quad patch (control points
//                                       as per-patch-control-point vertex attributes, half factors from a buffer) at factors 1, 4
//                                       and 16, integer and fractional-even partitioning, into a 128 x 128 target cleared to black;
//                                       each frame is compared with the shape rasterized on the CPU (texels lit outside it: stray
//                                       geometry; dark inside it: holes) and saved to /private/var/tmp/tess-CASE.ppm. Then GFXBench Car
//                                       Chase's own tessellation, one feature added at a time: fractional-odd partitioning; its control
//                                       points (buffer 7, 36 bytes: float3 position, two uchar3-normalized vectors, two float2); a
//                                       uint16 control-point index buffer; factors per patch and instance. Each control point's first
//                                       vector is a colour (red, green, blue, yellow), so every texel's colour is checked too. Last,
//                                       Car Chase's Bezier car body: a patch(quad, 0) function, no control points, each patch's
//                                       corners read from a buffer by patch_id (its patchType and control point count printed)
//
//   gfxprobe virtgpu                    one DRM context through the accelerator's virtgpu methods (DroidVMVirtgpuABI.h), as mesa's
//                                       vdrm drives them: capset, context, the msm shmem, GET_PARAM ccmds, a guest-alloc BO (GEM_NEW
//                                       and a HOST3D_GUEST blob) written through its mapping, a fenced NOP waited for, then closed
//
// FORMAT is an MTLPixelFormat number (80 BGRA8Unorm, 81 _sRGB, 115 RGBA16Float, 552-555 the XR formats; RenderBox sets bit 31
// for a variant of its own), FLAGS RenderBox's create flags (hex). Each line prints the surface's IOSurface pixel format, plane
// count, bytes per row, allocation and base address, and whether the device made a texture. RB::create_iosurface is not
// exported: its address is RenderBox's slide (from -[RBDrawable size]) plus its address in the iPadOS 17.7.11 (21H461) cache.
#import <CoreGraphics/CoreGraphics.h>
#import <CoreImage/CoreImage.h>
#import <CoreMotion/CoreMotion.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>
#import <dlfcn.h>
#import <mach-o/dyld.h>
#import <mach-o/loader.h>
#import <fcntl.h>
#import <mach/mach_time.h>
#import <sys/resource.h>
#import <sys/ioctl.h>
#import <sys/socket.h>
#import <netinet/in.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <pthread.h>
#import <spawn.h>
#import <sys/wait.h>
#import <mach/mach.h>
#import <mach/mach_host.h>
#import <mach_debug/mach_debug.h>
#import <stdatomic.h>
#import <os/lock.h>
#import <sys/mman.h>
#import <sys/stat.h>
#import <notify.h>
#import <sys/sysctl.h>
#import <unistd.h>

static const uintptr_t kRBDrawableSize = 0x1e2687330;      // -[RBDrawable size], unslid
static const uintptr_t kRBCreateIOSurface = 0x1e2638c84;   // RB::create_iosurface(MTLPixelFormat, unsigned, unsigned, unsigned)

typedef IOSurfaceRef (*CreateIOSurface)(unsigned long format, unsigned width, unsigned height, unsigned flags);
extern IOSurfaceRef CAIOSurfaceCreateDefault(unsigned width, unsigned height, unsigned fourcc);   // QuartzCore, private
extern uint32_t IOSurfaceGetCacheMode(IOSurfaceRef surface);                                    // IOSurface, private

// IOKit's C interface; the iphoneos SDK ships the framework without its headers.
typedef mach_port_t io_object_t;
extern const mach_port_t kIOMainPortDefault;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_object_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_object_t service, task_port_t owningTask, uint32_t type, io_object_t *connect);
extern kern_return_t IOServiceClose(io_object_t connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOServiceGetMatchingServices(mach_port_t mainPort, CFDictionaryRef matching, io_object_t *iterator);
extern io_object_t IOIteratorNext(io_object_t iterator);
extern CFTypeRef IORegistryEntryCreateCFProperty(io_object_t entry, CFStringRef key, CFAllocatorRef allocator, uint32_t options);
extern kern_return_t IORegistryEntryCreateCFProperties(io_object_t entry, CFMutableDictionaryRef *properties, CFAllocatorRef allocator,
                                                       uint32_t options);
extern kern_return_t IORegistryEntryGetChildIterator(io_object_t entry, const char *plane, io_object_t *iterator);
extern kern_return_t IOObjectGetClass(io_object_t object, char className[128]);

// CoreGraphics' context over an IOSurface (private), as WebKit makes one for a layer's backing store (IOSurface::createPlatformContext).
extern CGContextRef CGIOSurfaceContextCreate(IOSurfaceRef, size_t width, size_t height, size_t bitsPerComponent, size_t bitsPerPixel,
                                             CGColorSpaceRef, CGBitmapInfo);

// The bytes of each plane of a surface that are not 0.
static void litPlanes(IOSurfaceRef surface, size_t lit[2]) {
    lit[0] = lit[1] = 0;
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
    size_t planes = IOSurfaceGetPlaneCount(surface);
    for (size_t p = 0; p < (planes ? planes : 1) && p < 2; p++) {
        const uint8_t *bytes = planes ? IOSurfaceGetBaseAddressOfPlane(surface, p) : IOSurfaceGetBaseAddress(surface);
        size_t size = planes ? IOSurfaceGetBytesPerRowOfPlane(surface, p) * IOSurfaceGetHeightOfPlane(surface, p)
                             : IOSurfaceGetBytesPerRow(surface) * IOSurfaceGetHeight(surface);
        for (size_t i = 0; bytes && i < size; i++)
            lit[p] += bytes[i] != 0;
    }
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
}

static OSType parseFourcc(const char *code) {
    return strlen(code) == 4 ? (OSType)code[0] << 24 | (OSType)code[1] << 16 | (OSType)code[2] << 8 | (OSType)code[3]
                             : (OSType)strtoul(code, NULL, 0);
}

static NSString *fourcc(OSType code) {
    char text[5] = {(char)(code >> 24), (char)(code >> 16), (char)(code >> 8), (char)code, 0};
    for (int i = 0; i < 4; i++)
        if (text[i] < 32 || text[i] > 126) return [NSString stringWithFormat:@"%#x", code];
    return [NSString stringWithFormat:@"'%s'", text];
}

static void describe(NSString *what, IOSurfaceRef surface, id<MTLDevice> device, MTLPixelFormat format, unsigned w, unsigned h) {
    if (!surface) {
        printf("%s: no surface\n", what.UTF8String);
        return;
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
    id<MTLTexture> texture = [device newTextureWithDescriptor:d iosurface:surface plane:0];
    printf("%s: surface %s planes=%zu bpr=%zu alloc=%zu base=%p -> texture %s\n", what.UTF8String,
           fourcc(IOSurfaceGetPixelFormat(surface)).UTF8String, IOSurfaceGetPlaneCount(surface), IOSurfaceGetBytesPerRow(surface),
           IOSurfaceGetAllocSize(surface), IOSurfaceGetBaseAddress(surface), texture ? "YES" : "NO");
    CFRelease(surface);
}

// libsystem_sandbox: enter a sandbox profile by name (SANDBOX_NAMED), as a process launchd starts with one does.
extern int sandbox_init(const char *profile, uint64_t flags, char **errorbuf);

#include "DroidVMVirtgpuABI.h"   // NativeAdreno's
extern kern_return_t IOConnectCallStructMethod(mach_port_t, uint32_t, const void *, size_t, void *, size_t *);
static mach_port_t vgConnection;
static uint32_t vgSelector;
static kern_return_t vgCall(uint32_t op, const void *in, size_t inSize, void *out, size_t outSize) {
    NSMutableData *buffer = [NSMutableData dataWithLength:8 + inSize];
    const uint32_t header[2] = {0x4e544e4c, DROIDVM_VIRTGPU_METHOD + op};
    memcpy(buffer.mutableBytes, header, 8);
    if (inSize) memcpy((uint8_t *)buffer.mutableBytes + 8, in, inSize);
    size_t outBytes = outSize;
    return IOConnectCallStructMethod(vgConnection, vgSelector, buffer.bytes, buffer.length, outSize ? out : NULL, outSize ? &outBytes : NULL);
}
// One msm ccmd through EXECBUFFER (no fence), answered in the shmem: the response's length is written first (the host copies back
// at most that), then the host's seqno must reach the request's.
static kern_return_t vgCcmd(volatile uint8_t *shmem, uint32_t *seqno, void *request, uint32_t size, uint32_t responseBytes) {
    uint32_t *header = request;   // vdrm_ccmd_req: cmd, len, seqno, rsp_off
    header[1] = size; header[2] = ++*seqno; header[3] = 0;
    const uint32_t responseOffset = *(volatile uint32_t *)(shmem + 4);
    if (responseBytes) *(volatile uint32_t *)(shmem + responseOffset) = responseBytes;
    struct { struct droidvm_virtgpu_execbuffer e; uint8_t command[256]; } exec = {{0, 0, size, 0}, {0}};
    memcpy(exec.command, request, size);
    struct droidvm_virtgpu_execbuffer_reply reply = {0};
    kern_return_t kr = vgCall(DROIDVM_VIRTGPU_EXECBUFFER, &exec, sizeof exec.e + size, &reply, sizeof reply);
    for (int i = 0; !kr && *(volatile uint32_t *)shmem < *seqno && i < 1000; i++) usleep(1000);
    return kr ? kr : *(volatile uint32_t *)shmem >= *seqno ? KERN_SUCCESS : KERN_OPERATION_TIMED_OUT;
}
static uint64_t vgGetParam(volatile uint8_t *shmem, uint32_t *seqno, uint32_t param, kern_return_t *kr) {
    uint8_t request[16 + 4 + 24] = {0};
    const uint32_t cmd = 2 /* IOCTL_SIMPLE */, ioctl = 0xc0186440 /* DRM_IOCTL_MSM_GET_PARAM */, msm[2] = {0x10 /* 3D0 */, param};
    memcpy(request, &cmd, 4); memcpy(request + 16, &ioctl, 4); memcpy(request + 20, msm, 8);
    *kr = vgCcmd(shmem, seqno, request, sizeof request, 8 + 24);
    const volatile uint8_t *response = shmem + *(volatile uint32_t *)(shmem + 4);
    uint64_t value = 0; int32_t ret = 0;
    memcpy(&ret, (const uint8_t *)response + 4, 4); memcpy(&value, (const uint8_t *)response + 16, 8);
    if (!*kr && ret) *kr = (kern_return_t)ret;
    return value;
}
// memcheck: 1 MiB chunks of anonymous memory, word w of chunk i holding (its address) ^ salt[i]; a new salt each time a chunk
// is taken anew, so a word that holds another chunk's or an older generation's value decodes to an address of its own.
static int memoryCheck(unsigned long mib, unsigned long seconds) {
    enum { Chunk = 1u << 20, Words = Chunk / 8 };
    if (!mib) mib = 256;
    uint64_t **chunk = calloc(mib, sizeof *chunk), *salt = calloc(mib, sizeof *salt), bad = 0, rounds = 0;
    __block uint64_t generation = 0;
    if (!chunk || !salt) return 1;
    void (^take)(unsigned long) = ^(unsigned long i) {
        uint64_t *words = mmap(NULL, Chunk, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (words == MAP_FAILED) { chunk[i] = NULL; return; }
        const uint64_t mix = 0x9e3779b97f4a7c15ull * (++generation) | 1;
        for (unsigned long w = 0; w < Words; ++w) words[w] = (uint64_t)(uintptr_t)&words[w] ^ mix;
        chunk[i] = words; salt[i] = mix;
    };
    for (unsigned long i = 0; i < mib; ++i) take(i);
    const time_t begin = time(NULL);
    time_t reported = begin;
    unsigned long next = 0;
    printf("memcheck: %lu MiB in 1 MiB chunks for %lu s\n", mib, seconds);
    fflush(stdout);
    while ((unsigned long)(time(NULL) - begin) < seconds) {
        for (unsigned long i = 0; i < mib; ++i) {
            uint64_t *words = chunk[i];
            if (!words) continue;
            for (unsigned long w = 0; w < Words; ++w) {
                const uint64_t expected = (uint64_t)(uintptr_t)&words[w] ^ salt[i], got = words[w];
                if (got == expected) continue;
                if (++bad <= 256) {
                    // The value decoded with this chunk's salt: an address names the word whose value it is (a misplaced page).
                    printf("BAD round %llu chunk %lu word %#lx (page offset %#lx) at %p: got %#018llx want %#018llx diff %#018llx decoded %#llx\n",
                           rounds, i, w, (w * 8) & 4095, (void *)&words[w], got, expected, got ^ expected, got ^ salt[i]);
                }
                words[w] = expected;
            }
        }
        ++rounds;
        // A sixteenth of the chunks back to the kernel and taken anew, round-robin.
        for (unsigned long k = 0; k < (mib + 15) / 16; ++k, next = (next + 1) % mib) {
            if (chunk[next]) munmap(chunk[next], Chunk);
            take(next);
        }
        if (time(NULL) - reported >= 30) {
            reported = time(NULL);
            printf("memcheck: %llu rounds, %llu bad words, %llu chunks taken\n", rounds, bad, generation);
            fflush(stdout);
        }
        usleep(200000);
    }
    printf("memcheck: done, %llu rounds, %llu bad words\n", rounds, bad);
    return bad ? 2 : 0;
}

// memidle: word w of page p is compressible and still names its page: most words 0, every 8th a small value, every 64th the
// page number with the word index.
static uint64_t idleWord(uint64_t page, uint64_t w) {
    if (w % 64 == 0) return page << 16 | w;
    if (w % 8 == 0) return 0x1234 + (w & 0x38);
    return 0;
}
static int memoryIdle(unsigned long mib, unsigned long seconds, unsigned long period) {
    const uint64_t bytes = (uint64_t)(mib ? mib : 512) << 20, words = bytes / 8, pages = bytes / 4096;
    uint64_t *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (memory == MAP_FAILED) { printf("memidle: no memory\n"); return 1; }
    for (uint64_t w = 0; w < words; ++w) memory[w] = idleWord(w / 512, w % 512);
    printf("memidle: %llu MiB (%llu pages) filled; checked every %lu s for %lu s\n", bytes >> 20, pages, period, seconds);
    fflush(stdout);
    const time_t begin = time(NULL);
    uint64_t checks = 0, badPages = 0;
    while ((unsigned long)(time(NULL) - begin) < seconds) {
        sleep((unsigned)(period ? period : 60));
        ++checks;
        uint64_t badNow = 0;
        for (uint64_t p = 0; p < pages; ++p) {
            uint64_t *page = memory + p * 512, differ = 0, first = 0;
            for (uint64_t w = 0; w < 512; ++w) if (page[w] != idleWord(p, w)) { if (!differ++) first = w; }
            if (!differ) continue;
            ++badNow;
            if (++badPages <= 200)
                printf("BAD check %llu page %llu at %p: %llu words differ, first word %llu got %#018llx want %#018llx (word 0 holds %#llx)\n",
                       checks, p, (void *)page, differ, first, page[first], idleWord(p, first), page[0]);
            for (uint64_t w = 0; w < 512; ++w) page[w] = idleWord(p, w);
        }
        printf("memidle: check %llu after %ld s, %llu bad pages now, %llu in all\n", checks, (long)(time(NULL) - begin), badNow, badPages);
        fflush(stdout);
    }
    return badPages ? 2 : 0;
}

// regcheck: one thread's round, in assembly so that nothing but the kernel touches v0-v31 between the load and the store.
static void vectorRound(const uint8_t *in, uint8_t *out, uint64_t spins) {
    __asm__ volatile(
        "ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%[in]], #64\n"
        "ld1 {v4.16b, v5.16b, v6.16b, v7.16b}, [%[in]], #64\n"
        "ld1 {v8.16b, v9.16b, v10.16b, v11.16b}, [%[in]], #64\n"
        "ld1 {v12.16b, v13.16b, v14.16b, v15.16b}, [%[in]], #64\n"
        "ld1 {v16.16b, v17.16b, v18.16b, v19.16b}, [%[in]], #64\n"
        "ld1 {v20.16b, v21.16b, v22.16b, v23.16b}, [%[in]], #64\n"
        "ld1 {v24.16b, v25.16b, v26.16b, v27.16b}, [%[in]], #64\n"
        "ld1 {v28.16b, v29.16b, v30.16b, v31.16b}, [%[in]], #64\n"
        "1: subs %[spins], %[spins], #1\n"
        "b.ne 1b\n"
        "st1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%[out]], #64\n"
        "st1 {v4.16b, v5.16b, v6.16b, v7.16b}, [%[out]], #64\n"
        "st1 {v8.16b, v9.16b, v10.16b, v11.16b}, [%[out]], #64\n"
        "st1 {v12.16b, v13.16b, v14.16b, v15.16b}, [%[out]], #64\n"
        "st1 {v16.16b, v17.16b, v18.16b, v19.16b}, [%[out]], #64\n"
        "st1 {v20.16b, v21.16b, v22.16b, v23.16b}, [%[out]], #64\n"
        "st1 {v24.16b, v25.16b, v26.16b, v27.16b}, [%[out]], #64\n"
        "st1 {v28.16b, v29.16b, v30.16b, v31.16b}, [%[out]], #64\n"
        : [in] "+r"(in), [out] "+r"(out), [spins] "+r"(spins)
        :
        : "memory", "cc", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14",
          "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
}
// regfault: load v0-v31, enter the kernel twice (a system call, then a store that faults in a fresh page), store v0-v31.
static void vectorAcrossKernel(const uint8_t *in, uint8_t *out, uint64_t *fresh) {
    __asm__ volatile(
        "ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%[in]], #64\n"
        "ld1 {v4.16b, v5.16b, v6.16b, v7.16b}, [%[in]], #64\n"
        "ld1 {v8.16b, v9.16b, v10.16b, v11.16b}, [%[in]], #64\n"
        "ld1 {v12.16b, v13.16b, v14.16b, v15.16b}, [%[in]], #64\n"
        "ld1 {v16.16b, v17.16b, v18.16b, v19.16b}, [%[in]], #64\n"
        "ld1 {v20.16b, v21.16b, v22.16b, v23.16b}, [%[in]], #64\n"
        "ld1 {v24.16b, v25.16b, v26.16b, v27.16b}, [%[in]], #64\n"
        "ld1 {v28.16b, v29.16b, v30.16b, v31.16b}, [%[in]], #64\n"
        "mov x16, #20\n"                      // SYS_getpid
        "svc #0x80\n"
        "str x16, [%[fresh]]\n"               // the first store to a page just mapped: a zero-fill fault
        "st1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%[out]], #64\n"
        "st1 {v4.16b, v5.16b, v6.16b, v7.16b}, [%[out]], #64\n"
        "st1 {v8.16b, v9.16b, v10.16b, v11.16b}, [%[out]], #64\n"
        "st1 {v12.16b, v13.16b, v14.16b, v15.16b}, [%[out]], #64\n"
        "st1 {v16.16b, v17.16b, v18.16b, v19.16b}, [%[out]], #64\n"
        "st1 {v20.16b, v21.16b, v22.16b, v23.16b}, [%[out]], #64\n"
        "st1 {v24.16b, v25.16b, v26.16b, v27.16b}, [%[out]], #64\n"
        "st1 {v28.16b, v29.16b, v30.16b, v31.16b}, [%[out]], #64\n"
        : [in] "+r"(in), [out] "+r"(out)
        : [fresh] "r"(fresh)
        : "memory", "cc", "x0", "x1", "x16", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13",
          "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
}
static volatile uint64_t faultRounds, faultBad;
static volatile int faultStop;
static void *faultWorker(void *argument) {
    const uint64_t id = (uint64_t)(uintptr_t)argument;
    uint8_t in[512] __attribute__((aligned(16))), out[512] __attribute__((aligned(16)));
    uint64_t generation = 0;
    while (!faultStop) {
        uint8_t *pages = mmap(NULL, 64 * 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (pages == MAP_FAILED) { usleep(1000); continue; }
        for (unsigned k = 0; k < 64 && !faultStop; ++k) {
            ++generation;
            for (unsigned i = 0; i < 64; ++i) ((uint64_t *)in)[i] = (id << 56) ^ (generation << 16) ^ (i * 0x0101010101010101ull) ^ 0x5a;
            vectorAcrossKernel(in, out, (uint64_t *)(pages + k * 4096));
            for (unsigned r = 0; r < 32; ++r) {
                if (!memcmp(in + r * 16, out + r * 16, 16)) continue;
                if (__atomic_add_fetch(&faultBad, 1, __ATOMIC_RELAXED) <= 100)
                    printf("BAD thread %llu round %llu v%u: got %016llx%016llx want %016llx%016llx\n", id, generation, r,
                           ((uint64_t *)out)[r * 2 + 1], ((uint64_t *)out)[r * 2], ((uint64_t *)in)[r * 2 + 1], ((uint64_t *)in)[r * 2]);
            }
            __atomic_add_fetch(&faultRounds, 1, __ATOMIC_RELAXED);
        }
        munmap(pages, 64 * 4096);
        usleep(500);   // the vCPUs idle now and then (vmguard)
    }
    return NULL;
}
static int faultCheck(unsigned threads, unsigned long seconds) {
    if (!threads) threads = 8;
    if (threads > 64) threads = 64;
    pthread_t workers[64];
    for (unsigned i = 0; i < threads; ++i) pthread_create(&workers[i], NULL, faultWorker, (void *)(uintptr_t)(i + 1));
    printf("regfault: %u threads for %lu s\n", threads, seconds);
    fflush(stdout);
    for (unsigned long t = 0; t < seconds; t += 30) {
        sleep(seconds - t < 30 ? (unsigned)(seconds - t) : 30);
        printf("regfault: %llu rounds (a system call and a page fault each), %llu bad\n", faultRounds, faultBad);
        fflush(stdout);
    }
    faultStop = 1;
    for (unsigned i = 0; i < threads; ++i) pthread_join(workers[i], NULL);
    return faultBad ? 2 : 0;
}

// machcheck: the room a receive needs for a message of `size` bytes with trailer elements `elements`.
static int machCheck(unsigned long seconds) {
    mach_port_t port = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) ||
        mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND)) { printf("machcheck: no port\n"); return 1; }
    const int elements[] = {MACH_RCV_TRAILER_NULL, MACH_RCV_TRAILER_SEQNO, MACH_RCV_TRAILER_SENDER, MACH_RCV_TRAILER_AUDIT,
                            MACH_RCV_TRAILER_CTX, MACH_RCV_TRAILER_AV};
    const size_t trailers[] = {sizeof(mach_msg_trailer_t), sizeof(mach_msg_seqno_trailer_t), sizeof(mach_msg_security_trailer_t),
                               sizeof(mach_msg_audit_trailer_t), sizeof(mach_msg_context_trailer_t), sizeof(mach_msg_mac_trailer_t)};
    enum { Max = 16384, Canary = 256 };
    uint8_t *send = calloc(1, Max), *recv = malloc(Max + 128 + Canary);
    uint64_t rounds = 0, bad = 0, sent = 0;
    const time_t begin = time(NULL);
    printf("machcheck: %lu s\n", seconds);
    fflush(stdout);
    while ((unsigned long)(time(NULL) - begin) < seconds) {
        for (uint32_t size = sizeof(mach_msg_header_t); size <= Max - 128; size += 4) {
            for (unsigned t = 0; t < sizeof elements / sizeof elements[0]; ++t) {
                for (int small = 0; small < 2; ++small) {
                    mach_msg_header_t *h = (mach_msg_header_t *)send;
                    memset(send + sizeof *h, (int)(size & 0xff), size - sizeof *h);
                    *h = (mach_msg_header_t){.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0), .msgh_size = size,
                                             .msgh_remote_port = port, .msgh_id = (int)size};
                    if (mach_msg(h, MACH_SEND_MSG, size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL)) continue;
                    ++sent;
                    const size_t room = size + trailers[t] - (small ? 4 : 0);
                    memset(recv, 0xa5, room + Canary);
                    const mach_msg_option_t options = MACH_RCV_MSG | MACH_RCV_LARGE | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
                                                      MACH_RCV_TRAILER_ELEMENTS(elements[t]);
                    kern_return_t kr = mach_msg((mach_msg_header_t *)recv, options, 0, (mach_msg_size_t)room, port, MACH_MSG_TIMEOUT_NONE,
                                                MACH_PORT_NULL);
                    bool wrong = false;
                    for (size_t i = room; i < room + Canary; ++i) if (recv[i] != 0xa5) { wrong = true; break; }
                    if (small) {
                        if (kr != MACH_RCV_TOO_LARGE) wrong = true;
                        // MACH_RCV_LARGE left the message queued: take it with room enough.
                        mach_msg((mach_msg_header_t *)recv, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, Max + 128, port, 0, MACH_PORT_NULL);
                    } else if (kr != KERN_SUCCESS || ((mach_msg_header_t *)recv)->msgh_size != size) wrong = true;
                    if (wrong && ++bad <= 100) {
                        size_t first = room;
                        while (first < room + Canary && recv[first] == 0xa5) ++first;
                        printf("BAD size %u trailer %u%s: kr %#x, first byte past the %zu-byte room that changed at +%zd\n", size,
                               elements[t] >> 24, small ? " (room 4 too small)" : "", kr, room, (ssize_t)(first - room));
                    }
                    ++rounds;
                }
            }
        }
        printf("machcheck: %llu receives of %llu sent, %llu bad\n", rounds, sent, bad);
        fflush(stdout);
    }
    return bad ? 2 : 0;
}

// reusecheck: word w of the region holds its address mixed with the round, so a zeroed page and an old round tell apart.
static int reuseCheck(unsigned long mib, unsigned long pressureMib, unsigned long seconds) {
    const size_t bytes = (size_t)(mib ? mib : 256) << 20, half = bytes / 2, words = bytes / 8;
    uint64_t *region = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    uint64_t *control = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (region == MAP_FAILED || control == MAP_FAILED) { printf("reusecheck: no memory\n"); return 1; }
    uint64_t round = 0, badDirect = 0, badReuse = 0, reclaimed = 0;
    const time_t begin = time(NULL);
    printf("reusecheck: %zu MiB, %lu MiB of pressure a round, %lu s\n", bytes >> 20, pressureMib, seconds);
    fflush(stdout);
    while ((unsigned long)(time(NULL) - begin) < seconds) {
        ++round;
        const uint64_t before = 0x9e3779b97f4a7c15ull * (2 * round), after = 0x9e3779b97f4a7c15ull * (2 * round + 1);
        for (size_t w = 0; w < words; ++w) region[w] = (uint64_t)(uintptr_t)&region[w] ^ before;
        for (size_t w = 0; w < words; ++w) control[w] = (uint64_t)(uintptr_t)&control[w] ^ before;
        madvise(control, bytes, MADV_FREE_REUSABLE);
        madvise(region, bytes, MADV_FREE_REUSABLE);
        madvise((uint8_t *)region + half, half, MADV_FREE_REUSE);
        for (size_t w = 0; w < words; ++w) region[w] = (uint64_t)(uintptr_t)&region[w] ^ after;
        // Pressure: touch memory and give it back, so the pageout daemon looks for pages to reclaim.
        if (pressureMib) {
            const size_t pressure = (size_t)pressureMib << 20;
            uint8_t *p = mmap(NULL, pressure, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
            if (p != MAP_FAILED) { for (size_t o = 0; o < pressure; o += 4096) p[o] = (uint8_t)o | 1; munmap(p, pressure); }
        }
        for (size_t page = 0; page < bytes / 4096; ++page) {
            const uint64_t *q = region + page * 512;
            size_t differ = 0, zero = 0, old = 0;
            for (size_t w = 0; w < 512; ++w) {
                const uint64_t want = (uint64_t)(uintptr_t)&q[w] ^ after;
                if (q[w] == want) continue;
                ++differ;
                if (!q[w]) ++zero; else if (q[w] == ((uint64_t)(uintptr_t)&q[w] ^ before)) ++old;
            }
            if (!differ) continue;
            const bool direct = page * 4096 < half;
            uint64_t n = direct ? ++badDirect : ++badReuse;
            if (n <= 50)
                printf("BAD round %llu page %zu (%s half) at %p: %zu words differ, %zu zero, %zu from before the reuse\n", round, page,
                       direct ? "written again directly" : "MADV_FREE_REUSE first", (void *)q, differ, zero, old);
        }
        uint64_t gone = 0;   // control pages the kernel took back (zero now)
        for (size_t page = 0; page < bytes / 4096; ++page) gone += !control[page * 512] && !control[page * 512 + 511];
        madvise(control, bytes, MADV_FREE_REUSE);
        reclaimed += gone;
        printf("reusecheck: round %llu after %ld s: bad pages %llu written directly, %llu reused first; control pages reclaimed %llu"
               " (%llu in all)\n", round, (long)(time(NULL) - begin), badDirect, badReuse, gone, reclaimed);
        fflush(stdout);
    }
    return (badDirect || badReuse) ? 2 : 0;
}

// reusecheck's threaded form: each worker writes its share from its own CPU (its TLB holds the writable entries), the main
// thread marks the whole region reusable (the TLB maintenance comes from another CPU), and each worker writes its share again.
struct ReuseShare { uint64_t *words; size_t count; volatile uint64_t mix; pthread_t thread; };
static volatile uint64_t reusePhase;   // odd: workers write with the share's mix; the main thread waits for them
static volatile uint64_t reuseDone;
static void *reuseWorker(void *argument) {
    struct ReuseShare *share = argument;
    uint64_t seen = 0;
    for (;;) {
        uint64_t phase;
        while ((phase = __atomic_load_n(&reusePhase, __ATOMIC_ACQUIRE)) == seen) usleep(100);
        if (phase == ~0ull) return NULL;
        seen = phase;
        for (size_t w = 0; w < share->count; ++w) share->words[w] = (uint64_t)(uintptr_t)&share->words[w] ^ share->mix;
        __atomic_add_fetch(&reuseDone, 1, __ATOMIC_ACQ_REL);
    }
}
static void reuseWriteAll(struct ReuseShare *shares, unsigned threads, uint64_t mix) {
    for (unsigned i = 0; i < threads; ++i) shares[i].mix = mix;
    reuseDone = 0;
    __atomic_add_fetch(&reusePhase, 1, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&reuseDone, __ATOMIC_ACQUIRE) < threads) usleep(200);
}
static int reuseThreaded(unsigned long mib, unsigned long pressureMib, unsigned long seconds, unsigned threads) {
    const size_t bytes = (size_t)(mib ? mib : 256) << 20, words = bytes / 8;
    if (!threads || threads > 16) threads = 6;
    uint64_t *region = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (region == MAP_FAILED) { printf("reusethreads: no memory\n"); return 1; }
    struct ReuseShare shares[16];
    for (unsigned i = 0; i < threads; ++i) {
        shares[i].words = region + i * (words / threads); shares[i].count = words / threads;
        pthread_create(&shares[i].thread, NULL, reuseWorker, &shares[i]);
    }
    uint64_t round = 0, bad = 0;
    const time_t begin = time(NULL);
    printf("reusethreads: %zu MiB in %u shares, %lu MiB of pressure a round, %lu s\n", bytes >> 20, threads, pressureMib, seconds);
    fflush(stdout);
    while ((unsigned long)(time(NULL) - begin) < seconds) {
        ++round;
        const uint64_t before = 0x9e3779b97f4a7c15ull * (2 * round), after = 0x9e3779b97f4a7c15ull * (2 * round + 1);
        reuseWriteAll(shares, threads, before);
        madvise(region, bytes, MADV_FREE_REUSABLE);
        reuseWriteAll(shares, threads, after);
        if (pressureMib) {
            const size_t pressure = (size_t)pressureMib << 20;
            uint8_t *p = mmap(NULL, pressure, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
            if (p != MAP_FAILED) { for (size_t o = 0; o < pressure; o += 4096) p[o] = (uint8_t)o | 1; munmap(p, pressure); }
        }
        for (unsigned i = 0; i < threads; ++i)
            for (size_t page = 0; page < shares[i].count / 512; ++page) {
                const uint64_t *q = shares[i].words + page * 512;
                size_t differ = 0, zero = 0;
                for (size_t w = 0; w < 512; ++w)
                    if (q[w] != ((uint64_t)(uintptr_t)&q[w] ^ after)) { ++differ; zero += !q[w]; }
                if (differ && ++bad <= 50)
                    printf("BAD round %llu share %u page %zu at %p: %zu words differ, %zu zero\n", round, i, page, (void *)q, differ, zero);
            }
        printf("reusethreads: round %llu after %ld s: %llu bad pages\n", round, (long)(time(NULL) - begin), bad);
        fflush(stdout);
    }
    reusePhase = ~0ull;
    for (unsigned i = 0; i < threads; ++i) pthread_join(shares[i].thread, NULL);
    return bad ? 2 : 0;
}

// textcheck: a 64-bit hash of one page.
static uint64_t pageHash(const uint64_t *words, size_t count) {
    uint64_t h = 0x9e3779b97f4a7c15ull;
    for (size_t i = 0; i < count; ++i) h = (h ^ words[i]) * 0x100000001b3ull + (h >> 29);
    return h ? h : 1;
}
extern const void *_dyld_get_shared_cache_range(size_t *length);
// mach_vm.h is refused on iOS; the call is there (libsystem_kernel's MIG stub).
extern kern_return_t mach_vm_region_recurse(vm_map_read_t, mach_vm_address_t *, mach_vm_size_t *, natural_t *, vm_region_recurse_info_t,
                                            mach_msg_type_number_t *);
static int textCheck(unsigned long seconds, unsigned long period) {
    size_t length = 0;
    const uint8_t *cache = _dyld_get_shared_cache_range(&length);
    if (!cache || !length) { printf("textcheck: no shared cache\n"); return 1; }
    const size_t page = (size_t)getpagesize(), pages = length / page;
    // dyld_cache_header: mappingOffset at 0x10, the first dyld_cache_mapping_info's address (unslid) at its start
    const uint64_t unslid = *(const uint64_t *)(cache + *(const uint32_t *)(cache + 0x10)), slide = (uint64_t)(uintptr_t)cache - unslid;
    uint64_t *hashes = calloc(pages, sizeof *hashes);
    uint8_t *ranges = calloc(pages, 1);   // 1: a page of a mapping that can never be written, 2: one mapped read-only that could be
    char *vec = malloc(pages);
    if (!hashes || !ranges || !vec) { printf("textcheck: no memory\n"); return 1; }
    uint64_t readonlyPages = 0;
    // The shared region is a submap: its mappings' own protections are one level down.
    const mach_vm_address_t start = (mach_vm_address_t)(uintptr_t)cache, end = start + length;
    natural_t depth = 0;
    for (mach_vm_address_t address = start; address < end;) {
        mach_vm_size_t size = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &address, &size, &depth, (vm_region_recurse_info_t)&info, &count)) break;
        if (info.is_submap) { ++depth; continue; }
        if (address >= end) break;
        if (address < start) { size -= start - address; address = start; }
        if (!(info.protection & VM_PROT_WRITE) && (info.protection & VM_PROT_READ)) {
            const size_t first = (size_t)(address - start) / page;
            size_t last = (size_t)(address + size - start) / page;
            if (last > pages) last = pages;
            for (size_t i = first; i < last; ++i) readonlyPages += !ranges[i], ranges[i] = (info.max_protection & VM_PROT_WRITE) ? 2 : 1;
        }
        address += size;
    }
    uint8_t **copies = calloc(pages, sizeof *copies);   // the first copy of each __DATA_CONST page
    uint64_t constPages = 0;
    for (size_t i = 0; i < pages; ++i) constPages += ranges[i] == 2;
    printf("textcheck: shared cache %p+%zx (slide %#llx), %llu of its %zu pages read-only here (%llu of them could be made writable),"
           " %lu s every %lu s\n", cache, length, slide, readonlyPages, pages, constPages, seconds, period);
    fflush(stdout);
    uint64_t changed = 0, saved = 0;
    const time_t begin = time(NULL);
    for (unsigned round = 1;; ++round) {
        uint64_t resident = 0, fresh = 0, bad = 0;
        if (mincore(cache, length, vec)) { printf("textcheck: mincore %d\n", errno); return 1; }
        for (size_t i = 0; i < pages; ++i) {
            if (!ranges[i] || !(vec[i] & MINCORE_INCORE)) continue;
            ++resident;
            const uint8_t *at = cache + i * page;
            const uint64_t h = pageHash((const uint64_t *)at, page / 8);
            if (!hashes[i]) {
                hashes[i] = h; ++fresh;
                if (ranges[i] == 2 && copies && (copies[i] = malloc(page))) memcpy(copies[i], at, page);
                continue;
            }
            if (hashes[i] == h) continue;
            ++bad; ++changed;
            printf("CHANGED round %u page %#llx (unslid %#llx, %s): hash %016llx, was %016llx\n", round, (unsigned long long)(uintptr_t)at,
                   (unsigned long long)((uintptr_t)at - slide), ranges[i] == 2 ? "could be writable" : "never writable", h, hashes[i]);
            if (copies && copies[i]) {
                unsigned shown = 0, words = 0;
                for (size_t w = 0; w < page / 8; ++w) {
                    const uint64_t was = ((const uint64_t *)copies[i])[w], now = ((const uint64_t *)at)[w];
                    if (was == now) continue;
                    ++words;
                    if (shown++ < 8) printf("    +%#05zx (unslid %#llx): %016llx -> %016llx\n", w * 8,
                                           (unsigned long long)((uintptr_t)at - slide + w * 8), was, now);
                }
                printf("    %u words changed\n", words);
                memcpy(copies[i], at, page);
            }
            if (saved < 64) {
                char path[96];
                snprintf(path, sizeof path, "/private/var/tmp/textcheck-%llx-%llu.bin", (unsigned long long)((uintptr_t)at - slide), saved++);
                const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) { (void)!write(fd, at, page); close(fd); }
            }
            hashes[i] = h;
        }
        printf("textcheck: round %u after %ld s: %llu resident read-only pages, %llu seen first, %llu changed (%llu in all)\n", round,
               (long)(time(NULL) - begin), resident, fresh, bad, changed);
        fflush(stdout);
        if ((unsigned long)(time(NULL) - begin) >= seconds) break;
        sleep((unsigned)(period ? period : 10));
    }
    return changed ? 2 : 0;
}

// asidcheck: one child; exits 0 clean, 1 when it read a tag not its own, 3 when its page did not land at the shared address.
enum : uint64_t { AsidWords = 4096 / 8 };
static int asidChild(unsigned long seconds, unsigned index, uint64_t address) {
    uint64_t *page = mmap((void *)address, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (page != (uint64_t *)address) {
        if (page != MAP_FAILED) munmap(page, 4096);
        page = mmap((void *)address, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
    }
    if (page != (uint64_t *)address) return 3;
    const uint64_t tag = (uint64_t)getpid() << 32 | index;
    for (unsigned w = 0; w < AsidWords; ++w) page[w] = tag;
    uint64_t foreign = 0;
    const time_t begin = time(NULL);
    for (uint64_t round = 0; (unsigned long)(time(NULL) - begin) < seconds; ++round) {
        for (unsigned w = 0; w < AsidWords; ++w) {
            const uint64_t value = ((volatile uint64_t *)page)[w];
            if (value != tag && ++foreign <= 4) {
                char line[160];
                const int n = snprintf(line, sizeof line, "FOREIGN child %u pid %d round %llu word %u: read %016llx (pid %llu, child %llu)\n",
                                       index, getpid(), round, w, value, value >> 32, value & 0xffffffff);
                (void)!write(1, line, (size_t)n);
            }
            ((volatile uint64_t *)page)[w] = tag;
        }
        usleep(round & 1 ? 0 : 50);   // switch out often: other processes and other CPUs
    }
    return foreign ? 1 : 0;
}
extern char **environ;
extern int _NSGetExecutablePath(char *buf, uint32_t *bufsize);
static int asidCheck(unsigned procs, unsigned long seconds) {
    if (!procs || procs > 1000) procs = 200;
    char self[1024]; uint32_t size = sizeof self;
    if (_NSGetExecutablePath(self, &size)) { printf("asidcheck: no path\n"); return 1; }
    pid_t *children = calloc(procs, sizeof *children);
    unsigned started = 0;
    // The shared address: the middle of a gap free here (the children's layout is this process's).
    uint8_t *gap = mmap(NULL, 64 << 20, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (gap == MAP_FAILED) { printf("asidcheck: no gap\n"); return 1; }
    munmap(gap, 64 << 20);
    const uint64_t address = (uint64_t)(uintptr_t)gap + (32 << 20);
    char secondsArg[32], addressArg[32];
    snprintf(secondsArg, sizeof secondsArg, "%lu", seconds); snprintf(addressArg, sizeof addressArg, "%#llx", address);
    for (unsigned i = 0; i < procs; ++i) {
        char indexArg[16]; snprintf(indexArg, sizeof indexArg, "%u", i);
        char *argv[] = {self, "asidchild", secondsArg, indexArg, addressArg, NULL};
        if (posix_spawn(&children[i], self, NULL, NULL, argv, environ)) { children[i] = 0; continue; }
        ++started;
    }
    printf("asidcheck: %u of %u processes started, %lu s, the page at %#llx\n", started, procs, seconds, address);
    fflush(stdout);
    unsigned clean = 0, foreign = 0, misplaced = 0, other = 0;
    for (unsigned i = 0; i < procs; ++i) {
        if (!children[i]) continue;
        int status = 0;
        if (waitpid(children[i], &status, 0) < 0 || !WIFEXITED(status)) { ++other; continue; }
        switch (WEXITSTATUS(status)) { case 0: ++clean; break; case 1: ++foreign; break; case 3: ++misplaced; break; default: ++other; }
    }
    printf("asidcheck: %u clean, %u read another process's page, %u not at the address, %u other\n", clean, foreign, misplaced, other);
    return foreign ? 2 : 0;
}

static int zeroCheck(unsigned long mib, unsigned long seconds) {
    const size_t bytes = (size_t)(mib ? mib : 64) << 20, page = (size_t)getpagesize();
    uint64_t rounds = 0, pages = 0, dirty = 0;
    const time_t begin = time(NULL);
    printf("zerocheck: %zu MiB a round, %lu s\n", bytes >> 20, seconds);
    fflush(stdout);
    while ((unsigned long)(time(NULL) - begin) < seconds) {
        uint8_t *region = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (region == MAP_FAILED) { usleep(100000); continue; }
        ++rounds;
        for (size_t o = 0; o < bytes; o += page) {
            volatile uint64_t *w = (volatile uint64_t *)(region + o);
            if ((o / page) & 1) (void)w[1];   // read first: the zero page, then a copy of it
            w[0] = 0x5a5a5a5a5a5a5a5aull;
            ++pages;
            size_t nonzero = 0, first = 0;
            for (size_t i = 1; i < page / 8; ++i) if (w[i]) { if (!nonzero++) first = i; }
            if (!nonzero) continue;
            if (++dirty <= 40) {
                printf("DIRTY round %llu page %p (%s): %zu non-zero words, the first at +%#zx:", rounds, (void *)w,
                       (o / page) & 1 ? "read first" : "written first", nonzero, first * 8);
                for (size_t i = first, n = 0; i < page / 8 && n < 6; ++i) if (w[i]) { printf(" %016llx", w[i]); ++n; }
                printf("\n");
                fflush(stdout);
            }
        }
        munmap(region, bytes);
        if (!(rounds % 16)) {
            printf("zerocheck: %llu rounds, %llu pages, %llu not zero\n", rounds, pages, dirty);
            fflush(stdout);
        }
        usleep(2000);
    }
    printf("zerocheck: %llu rounds, %llu pages, %llu not zero\n", rounds, pages, dirty);
    return dirty ? 2 : 0;
}

// datacheck: hex without stdio (stdio's globals are in __DATA_DIRTY).
static char *hexTo(char *at, uint64_t value, int digits) {
    for (int i = digits - 1; i >= 0; --i) *at++ = "0123456789abcdef"[(value >> (i * 4)) & 0xf];
    return at;
}
static char *strTo(char *at, const char *text, size_t most) {
    for (size_t i = 0; text[i] && i < most; ++i) *at++ = text[i];
    return at;
}
struct DataSegment { const uint8_t *base; size_t bytes; uint8_t *copy; const char *image; char name[17]; };
static int dataCheck(unsigned long seconds, unsigned long periodMs) {
    struct DataSegment *segments = calloc(4096, sizeof *segments);
    unsigned count = 0;
    size_t total = 0;
    const uint32_t images = _dyld_image_count();
    for (uint32_t i = 0; i < images && count < 4096; ++i) {
        const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(i);
        const intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        if (!header || header->magic != MH_MAGIC_64) continue;
        const uint8_t *command = (const uint8_t *)(header + 1);
        for (uint32_t c = 0; c < header->ncmds && count < 4096; ++c, command += ((const struct load_command *)command)->cmdsize) {
            const struct segment_command_64 *segment = (const struct segment_command_64 *)command;
            if (segment->cmd != LC_SEGMENT_64 || !segment->vmsize) continue;
            if (strncmp(segment->segname, "__DATA", 16) && strncmp(segment->segname, "__DATA_DIRTY", 16) &&
                strncmp(segment->segname, "__AUTH", 16)) continue;
            struct DataSegment *d = &segments[count];
            d->base = (const uint8_t *)(uintptr_t)(segment->vmaddr + slide);
            d->bytes = (size_t)segment->vmsize & ~(size_t)7;
            d->image = _dyld_get_image_name(i);
            memcpy(d->name, segment->segname, 16);
            if (!(d->copy = malloc(d->bytes))) continue;
            memcpy(d->copy, d->base, d->bytes);
            total += d->bytes; ++count;
        }
    }
    char line[512];
    int n = snprintf(line, sizeof line, "datacheck: %u data segments of %u images, %zu KiB, %lu s every %lu ms\n", count, images,
                     total >> 10, seconds, periodMs);
    (void)!write(1, line, (size_t)n);
    uint64_t changed = 0, rounds = 0;
    struct timespec nap = {(time_t)(periodMs / 1000), (long)(periodMs % 1000) * 1000000};
    const time_t begin = time(NULL);
    while ((unsigned long)(time(NULL) - begin) < seconds) {
        nanosleep(&nap, NULL);
        ++rounds;
        for (unsigned s = 0; s < count; ++s) {
            const uint64_t *now = (const uint64_t *)segments[s].base;
            uint64_t *was = (uint64_t *)segments[s].copy;
            for (size_t w = 0; w < segments[s].bytes / 8; ++w) {
                if (now[w] == was[w]) continue;
                const uint64_t value = now[w];
                if (++changed <= 400) {
                    const char *image = strrchr(segments[s].image, '/');
                    char *at = strTo(line, "CHANGED round ", 14);
                    at = hexTo(at, rounds, 8);
                    *at++ = ' ';
                    at = strTo(at, image ? image + 1 : segments[s].image, 60);
                    *at++ = ' ';
                    at = strTo(at, segments[s].name, 16);
                    at = strTo(at, "+0x", 3);
                    at = hexTo(at, w * 8, 6);
                    at = strTo(at, " at 0x", 6);
                    at = hexTo(at, (uint64_t)(uintptr_t)&now[w], 12);
                    at = strTo(at, ": ", 2);
                    at = hexTo(at, was[w], 16);
                    at = strTo(at, " -> ", 4);
                    at = hexTo(at, value, 16);
                    *at++ = '\n';
                    (void)!write(1, line, (size_t)(at - line));
                }
                was[w] = value;
            }
        }
        if (!(rounds % (60000 / (periodMs ? periodMs : 1) + 1))) {
            char *at = strTo(line, "datacheck: round ", 17);
            at = hexTo(at, rounds, 8);
            at = strTo(at, ", words changed ", 16);
            at = hexTo(at, changed, 8);
            *at++ = '\n';
            (void)!write(1, line, (size_t)(at - line));
        }
    }
    return changed ? 2 : 0;
}

// cowcheck: one child; 0 clean, 1 when its guard or environ pointer changed.
extern uintptr_t __stack_chk_guard;
extern char ***_NSGetEnviron(void);
extern void *_os_alloc_once_table[];
static int cowChild(unsigned long lifeMs, unsigned index) {
    volatile uintptr_t *guardSlot = &__stack_chk_guard;
    char ***environSlot = _NSGetEnviron();
    const uintptr_t guard = *guardSlot;
    char **environment = *environSlot;
    const uint8_t *pages[3] = {(const uint8_t *)((uintptr_t)guardSlot & ~(uintptr_t)4095),
                               (const uint8_t *)((uintptr_t)environSlot & ~(uintptr_t)4095),
                               (const uint8_t *)((uintptr_t)_os_alloc_once_table & ~(uintptr_t)4095)};
    static uint8_t copies[3][4096];
    for (int i = 0; i < 3; ++i) memcpy(copies[i], pages[i], 4096);
    char line[400];
    int n = snprintf(line, sizeof line, "START cowchild %u pid %d guard %016lx environ %p\n", index, getpid(), (unsigned long)guard,
                     (void *)environment);
    (void)!write(1, line, (size_t)n);
    struct timespec begin, now;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (uint64_t checks = 0;; ++checks) {
        const uintptr_t g = *guardSlot;
        char **e = *(char **volatile *)environSlot;
        if (g != guard || e != environment) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            n = snprintf(line, sizeof line, "FOREIGN cowchild %u pid %d after %llu checks, %lld ms: guard %016lx -> %016lx, environ %p -> %p\n",
                         index, getpid(), checks, (long long)(now.tv_sec - begin.tv_sec) * 1000 + (now.tv_nsec - begin.tv_nsec) / 1000000,
                         (unsigned long)guard, (unsigned long)g, (void *)environment, (void *)e);
            (void)!write(1, line, (size_t)n);
            for (int i = 0; i < 3; ++i) {
                unsigned differ = 0, shown = 0;
                char *at = line + snprintf(line, sizeof line, "  page %p:", (const void *)pages[i]);
                for (unsigned w = 0; w < 512; ++w) {
                    const uint64_t was = ((const uint64_t *)copies[i])[w], is = ((const volatile uint64_t *)pages[i])[w];
                    if (was == is) continue;
                    ++differ;
                    if (shown++ < 4) at += snprintf(at, (size_t)(line + sizeof line - at), " +%03x %llx>%llx", w * 8, was, is);
                }
                at += snprintf(at, (size_t)(line + sizeof line - at), " (%u words differ)\n", differ);
                (void)!write(1, line, (size_t)(at - line));
            }
            _exit(1);   // no return through frames whose saved guard no longer matches
        }
        if (!(checks & 1023)) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if ((uint64_t)(now.tv_sec - begin.tv_sec) * 1000 + (uint64_t)(now.tv_nsec - begin.tv_nsec) / 1000000 >= lifeMs) {
                n = snprintf(line, sizeof line, "END cowchild %u pid %d\n", index, getpid());
                (void)!write(1, line, (size_t)n);
                return 0;
            }
            usleep(200);
        }
    }
}
static int cowCheck(unsigned procs, unsigned long lifeMs, unsigned long seconds) {
    if (!procs || procs > 400) procs = 40;
    char self[1024]; uint32_t size = sizeof self;
    if (_NSGetExecutablePath(self, &size)) { printf("cowcheck: no path\n"); return 1; }
    pid_t *alive = calloc(procs, sizeof *alive);
    char lifeArg[32]; snprintf(lifeArg, sizeof lifeArg, "%lu", lifeMs);
    uint64_t started = 0, clean = 0, foreign = 0, signalled = 0, other = 0;
    const time_t begin = time(NULL);
    printf("cowcheck: %u processes at a time, each %lu ms, %lu s\n", procs, lifeMs, seconds);
    fflush(stdout);
    for (;;) {
        const bool more = (unsigned long)(time(NULL) - begin) < seconds;
        unsigned running = 0;
        for (unsigned i = 0; i < procs; ++i) {
            if (!alive[i] && more) {
                char indexArg[24]; snprintf(indexArg, sizeof indexArg, "%llu", started);
                char *argv[] = {self, "cowchild", lifeArg, indexArg, NULL};
                if (!posix_spawn(&alive[i], self, NULL, NULL, argv, environ)) ++started; else alive[i] = 0;
            }
            running += alive[i] != 0;
        }
        if (!running) break;
        int status = 0;
        const pid_t done = waitpid(-1, &status, 0);
        if (done <= 0) continue;
        for (unsigned i = 0; i < procs; ++i) if (alive[i] == done) alive[i] = 0;
        if (WIFEXITED(status)) { if (!WEXITSTATUS(status)) ++clean; else if (WEXITSTATUS(status) == 1) ++foreign; else ++other; }
        else if (WIFSIGNALED(status)) {
            ++signalled;
            printf("cowcheck: a child died of signal %d\n", WTERMSIG(status));
            fflush(stdout);
        }
        if (!((clean + foreign + signalled + other) % 500)) {
            printf("cowcheck: %llu started, %llu clean, %llu saw another process's page, %llu killed by a signal, %llu other\n", started,
                   clean, foreign, signalled, other);
            fflush(stdout);
        }
    }
    printf("cowcheck: %llu started, %llu clean, %llu saw another process's page, %llu killed by a signal, %llu other\n", started, clean,
           foreign, signalled, other);
    return (foreign || signalled) ? 2 : 0;
}

static NSString *const kTessSource = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct ControlPoint { float4 position [[attribute(0)]]; };\n"
    "struct Patch { patch_control_point<ControlPoint> cp; };\n"
    "struct Out { float4 position [[position]]; float4 color; };\n"
    "[[patch(triangle, 3)]] vertex Out tri_vertex(Patch p [[stage_in]], float3 b [[position_in_patch]]) {\n"
    "  Out o; o.position = p.cp[0].position * b.x + p.cp[1].position * b.y + p.cp[2].position * b.z;\n"
    "  o.color = float4(b, 1); return o; }\n"
    "[[patch(quad, 4)]] vertex Out quad_vertex(Patch p [[stage_in]], float2 uv [[position_in_patch]]) {\n"
    "  float4 a = mix(p.cp[0].position, p.cp[1].position, uv.x), c = mix(p.cp[3].position, p.cp[2].position, uv.x);\n"
    "  Out o; o.position = mix(a, c, uv.y); o.color = float4(uv, 1, 1); return o; }\n"
    "fragment float4 color_fragment(Out in [[stage_in]]) { return in.color; }\n"
    "struct CP36 { float3 position [[attribute(0)]]; float3 normal [[attribute(1)]]; float3 tangent [[attribute(2)]];\n"
    "  float2 uv0 [[attribute(3)]]; float2 uv1 [[attribute(4)]]; };\n"
    "struct Patch36 { patch_control_point<CP36> cp; };\n"
    "[[patch(triangle, 3)]] vertex Out cc_vertex(Patch36 p [[stage_in]], float3 b [[position_in_patch]]) {\n"
    "  Out o; o.position = float4(p.cp[0].position * b.x + p.cp[1].position * b.y + p.cp[2].position * b.z, 1);\n"
    "  o.color = float4(p.cp[0].normal * b.x + p.cp[1].normal * b.y + p.cp[2].normal * b.z, 1); return o; }\n"
    "[[patch(quad, 0)]] vertex Out bezier_vertex(float2 uv [[position_in_patch]], uint pid [[patch_id]],\n"
    "  const device float4 *corners [[buffer(1)]]) {\n"
    "  float4 a = mix(corners[pid * 4], corners[pid * 4 + 1], uv.x), c = mix(corners[pid * 4 + 3], corners[pid * 4 + 2], uv.x);\n"
    "  Out o; o.position = mix(a, c, uv.y); o.color = float4(uv.x, uv.y, pid ? 1.0 : 0.0, 1); return o; }\n"
    "[[patch(triangle, 3)]] vertex Out cc4_vertex(Patch p [[stage_in]], float3 b [[position_in_patch]]) {\n"
    "  Out o; o.position = p.cp[0].position * b.x + p.cp[1].position * b.y + p.cp[2].position * b.z;\n"
    "  o.color = float4(1, 1, 1, 1); return o; }\n";
// tess: whether a texel's centre lies in the polygon (NDC corners, counter-clockwise or not), the texel at (x, y) of a size x size target.
static bool tessInside(const float (*corner)[2], int corners, int x, int y, int size) {
    const float px = (x + 0.5f) * 2 / size - 1, py = 1 - (y + 0.5f) * 2 / size;
    int sign = 0;
    for (int i = 0; i < corners; ++i) {
        const float *a = corner[i], *b = corner[(i + 1) % corners];
        const float e = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
        const int s = e > 0 ? 1 : e < 0 ? -1 : 0;
        if (s && sign && s != sign) return false;
        if (s) sign = s;
    }
    return true;
}
// pitchcheck W H PITCH: a BGRA8 texture over a no-copy buffer whose rows lie PITCH bytes apart -- the app switcher's 610x814
// snapshot thumbnails come to the render server at 2448-byte rows, which no Adreno linear image lays (its rows are 64-byte
// aligned), so the runtime gives the texture a shadow image. Read back through a blit and through a render pass that samples
// it, three times, the CPU rewriting the buffer's texels before the second and the third: every texel must come back.
static const char *kPitchSource =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; float2 uv; };\n"
    "vertex V pitch_vertex(uint i [[vertex_id]]) { float2 c = float2((i << 1) & 2, i & 2); V v; v.p = float4(c * 2 - 1, 0, 1);"
    " v.uv = float2(c.x, 1 - c.y); return v; }\n"
    "fragment float4 pitch_fragment(V v [[stage_in]], texture2d<float> t [[texture(0)]]) {"
    " constexpr sampler s(filter::nearest); return t.sample(s, v.uv); }\n";

static uint32_t pitchTexel(unsigned x, unsigned y, unsigned round) {
    // BGRA in memory: blue x, green y (varied by the round), red the high bits of both, alpha opaque.
    const uint32_t b = x & 0xff, g = (y & 0xff) ^ (round * 0x55), r = ((x >> 8) & 0xf) | (((y >> 8) & 0xf) << 4);
    return b | g << 8 | r << 16 | 0xffu << 24;
}

static int pitchCheck(unsigned w, unsigned h, unsigned pitch) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device || pitch < w * 4) { printf("pitch: no device or a pitch below the row\n"); return 1; }
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:@(kPitchSource) options:nil error:&error];
    if (!library) { printf("pitch: no library: %s\n", error.description.UTF8String); return 1; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [library newFunctionWithName:@"pitch_vertex"];
    pd.fragmentFunction = [library newFunctionWithName:@"pitch_fragment"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!state) { printf("pitch: no pipeline: %s\n", error.description.UTF8String); return 1; }
    const size_t page = (size_t)getpagesize(), length = ((size_t)pitch * h + page - 1) / page * page;
    uint8_t *memory = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (memory == MAP_FAILED) { printf("pitch: no memory\n"); return 1; }
    id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:memory length:length options:MTLResourceStorageModeShared deallocator:nil];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = buffer ? [buffer newTextureWithDescriptor:td offset:0 bytesPerRow:pitch] : nil;
    if (!texture) { printf("pitch: no texture over the buffer (%ux%u, rows of %u bytes)\n", w, h, pitch); return 1; }
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> copied = [device newTextureWithDescriptor:td], drawn = [device newTextureWithDescriptor:td];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    uint32_t *texels = malloc((size_t)w * h * 4);
    int failures = 0;
    for (unsigned round = 0; round < 3; ++round) {
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) ((uint32_t *)(memory + (size_t)y * pitch))[x] = pitchTexel(x, y, round);
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                    toTexture:copied destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawn; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 1, 1, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:state];
        [enc setFragmentTexture:texture atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.error) { printf("pitch round %u: command buffer: %s\n", round, cb.error.description.UTF8String); ++failures; continue; }
        const char *names[2] = {"blit", "render"};
        id<MTLTexture> results[2] = {copied, drawn};
        for (int k = 0; k < 2; ++k) {
            [results[k] getBytes:texels bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
            unsigned wrong = 0, white = 0, zero = 0, stale = 0, firstX = 0, firstY = 0;
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 0; x < w; ++x) {
                    const uint32_t got = texels[(size_t)y * w + x];
                    if (got == pitchTexel(x, y, round)) continue;
                    if (!wrong) { firstX = x; firstY = y; }
                    ++wrong; white += got == 0xffffffffu; zero += got == 0;
                    stale += round && got == pitchTexel(x, y, round - 1);
                }
            printf("pitch %ux%u rows %u round %u %s: %u wrong (white %u, zero %u, earlier round %u)", w, h, pitch, round, names[k], wrong,
                   white, zero, stale);
            if (wrong) printf("; first at %u,%u: %08x, want %08x", firstX, firstY, texels[(size_t)firstY * w + firstX],
                              pitchTexel(firstX, firstY, round));
            printf("\n");
            failures += wrong != 0;
        }
    }
    free(texels);
    return failures ? 1 : 0;
}

// msaa: what multisampling costs on this GPU path (Metal -> metal2vulkan -> Turnip): one pass a frame of DRAWS overlapping
// full-screen triangles, blended, at 1 sample into the target, at 4 samples into a memoryless multisample texture with a
// multisample resolve store (iOS apps' usual MSAA: on a tiler the samples never leave tile memory), and at 4 samples into a
// private multisample texture, resolved. Each set's mean GPU time per frame (the command buffers' GPU start and end).
static const char *kMsaaSource =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; float4 c; };\n"
    "vertex V msaa_vertex(uint i [[vertex_id]], uint n [[instance_id]]) {"
    " float2 c = float2((i << 1) & 2, i & 2); float a = n * 0.37; float2 r = float2(cos(a), sin(a));"
    " float2 p = c * 2 - 1; p = float2(p.x * r.x - p.y * r.y, p.x * r.y + p.y * r.x) * 1.6;"
    " V v; v.p = float4(p, 0, 1); v.c = float4(fract(n * 0.13), fract(n * 0.29), fract(n * 0.41), 0.08); return v; }\n"
    "fragment float4 msaa_fragment(V v [[stage_in]]) { return v.c; }\n";

static int msaaProbe(int argc, char **argv) {
    const NSUInteger w = argc > 0 ? strtoul(argv[0], NULL, 0) : 2048, h = argc > 1 ? strtoul(argv[1], NULL, 0) : 2732;
    const NSUInteger draws = argc > 2 ? strtoul(argv[2], NULL, 0) : 64, frames = argc > 3 ? strtoul(argv[3], NULL, 0) : 30;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { printf("msaa: no Metal device\n"); return 1; }
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:@(kMsaaSource) options:nil error:&error];
    if (!library) { printf("msaa: no library: %s\n", error.description.UTF8String); return 1; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    printf("msaa: %lux%lu, %lu blended full-screen triangles a pass, %lu frames; supportsTextureSampleCount 4: %d\n",
           (unsigned long)w, (unsigned long)h, (unsigned long)draws, (unsigned long)frames, [device supportsTextureSampleCount:4]);
    const char *names[3] = {"1 sample", "4 samples, memoryless, resolved", "4 samples, private, resolved"};
    double base = 0;
    for (int mode = 0; mode < 3; ++mode) {
        const NSUInteger samples = mode ? 4 : 1;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [library newFunctionWithName:@"msaa_vertex"];
        pd.fragmentFunction = [library newFunctionWithName:@"msaa_fragment"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pd.colorAttachments[0].blendingEnabled = YES;
        pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pd.rasterSampleCount = samples;
        id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!state) { printf("  %s: no pipeline: %s\n", names[mode], error.description.UTF8String); continue; }
        id<MTLTexture> multisample = nil;
        if (mode) {
            MTLTextureDescriptor *md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
            md.textureType = MTLTextureType2DMultisample; md.sampleCount = 4; md.usage = MTLTextureUsageRenderTarget;
            md.storageMode = mode == 1 ? MTLStorageModeMemoryless : MTLStorageModePrivate;
            multisample = [device newTextureWithDescriptor:md];
            if (!multisample) { printf("  %s: no multisample texture\n", names[mode]); continue; }
        }
        double total = 0; NSUInteger counted = 0;
        for (NSUInteger f = 0; f < frames; ++f) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = multisample ? multisample : target;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            if (multisample) {
                rp.colorAttachments[0].resolveTexture = target;
                rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
            } else rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:state];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:draws];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.error) { printf("  %s: command buffer: %s\n", names[mode], cb.error.description.UTF8String); break; }
            if (f < 3) continue;   // warm-up
            const double gpu = cb.GPUEndTime - cb.GPUStartTime;
            if (gpu > 0) { total += gpu; counted++; }
        }
        const double mean = counted ? total / counted * 1000 : 0;
        if (!mode) base = mean;
        printf("  %-34s %7.2f ms GPU a frame%s\n", names[mode], mean,
               mode && base > 0 ? [NSString stringWithFormat:@" (%.2fx the 1-sample pass)", mean / base].UTF8String : "");
    }
    return 0;
}

// dontcare: each case fills its attachment with the earlier value (a pass cleared to it and stored), then runs the pass under
// test (the case's load action, no draw or a quad over the left half or the whole target at depth 0.25 in green, stored
// DontCare, or MultisampleResolve into a resolve texture) and classifies every texel and sample on the GPU: the earlier value,
// the value Store keeps (green where drawn; elsewhere the clear value under Clear, the earlier one under Load, none under
// DontCare), or a third. Earlier red / 0.75, clear blue / 0.5.
static const char *kDontCareSource =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; };\n"
    "vertex V dc_vertex(uint i [[vertex_id]], constant float &right [[buffer(0)]]) {"
    " float2 c = float2(i & 1 ? right : -1.0, i & 2 ? 1.0 : -1.0); V v; v.p = float4(c, 0.25, 1); return v; }\n"
    "fragment float4 dc_fragment(V v [[stage_in]]) { return float4(0, 1, 0, 1); }\n"
    "struct P { float4 e; float4 d; float4 u; uint split; uint known; uint samples; uint pad; };\n"
    "static void dc_count(float4 v, uint x, constant P &p, device atomic_uint *c) {"
    " bool drawn = x < p.split; bool known = drawn || p.known != 0; float4 s = drawn ? p.d : p.u;"
    " uint k = all(abs(v - p.e) < 0.01) ? 0 : (known && all(abs(v - s) < 0.01)) ? 1 : known ? 2 : 3;"
    " atomic_fetch_add_explicit(&c[k], 1, memory_order_relaxed); }\n"
    "kernel void dc_color(texture2d<float> t [[texture(0)]], constant P &p [[buffer(0)]], device atomic_uint *c [[buffer(1)]],"
    " uint2 g [[thread_position_in_grid]]) { if (g.x >= t.get_width() || g.y >= t.get_height()) return; dc_count(t.read(g), g.x, p, c); }\n"
    "kernel void dc_color_ms(texture2d_ms<float> t [[texture(0)]], constant P &p [[buffer(0)]], device atomic_uint *c [[buffer(1)]],"
    " uint2 g [[thread_position_in_grid]]) { if (g.x >= t.get_width() || g.y >= t.get_height()) return;"
    " for (uint s = 0; s < p.samples; ++s) dc_count(t.read(g, s), g.x, p, c); }\n"
    "kernel void dc_depth(depth2d<float> t [[texture(0)]], constant P &p [[buffer(0)]], device atomic_uint *c [[buffer(1)]],"
    " uint2 g [[thread_position_in_grid]]) { if (g.x >= t.get_width() || g.y >= t.get_height()) return;"
    " dc_count(float4(t.read(g), 0, 0, 0), g.x, p, c); }\n"
    "kernel void dc_depth_ms(depth2d_ms<float> t [[texture(0)]], constant P &p [[buffer(0)]], device atomic_uint *c [[buffer(1)]],"
    " uint2 g [[thread_position_in_grid]]) { if (g.x >= t.get_width() || g.y >= t.get_height()) return;"
    " for (uint s = 0; s < p.samples; ++s) dc_count(float4(t.read(g, s), 0, 0, 0), g.x, p, c); }\n";

typedef struct { float e[4], d[4], u[4]; uint32_t split, known, samples, pad; } DontCareParams;

static int dontCareProbe(int argc, char **argv) {
    const NSUInteger w = argc > 0 ? strtoul(argv[0], NULL, 0) : 1024, h = argc > 1 ? strtoul(argv[1], NULL, 0) : 1024;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { printf("dontcare: no Metal device\n"); return 1; }
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:@(kDontCareSource) options:nil error:&error];
    if (!library) { printf("dontcare: no library: %s\n", error.description.UTF8String); return 1; }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> counts = [device newBufferWithLength:16 options:MTLResourceStorageModeShared];
    struct { const char *name; MTLPixelFormat format; NSUInteger samples; BOOL resolve; } configs[] = {
        {"BGRA8Unorm 1x DontCare", MTLPixelFormatBGRA8Unorm, 1, NO},
        {"BGRA8Unorm 4x MultisampleResolve", MTLPixelFormatBGRA8Unorm, 4, YES},
        {"BGRA8Unorm 4x DontCare", MTLPixelFormatBGRA8Unorm, 4, NO},
        {"RGBA16Float 1x DontCare", MTLPixelFormatRGBA16Float, 1, NO},
        {"Depth32Float 1x DontCare", MTLPixelFormatDepth32Float, 1, NO},
        {"Depth32Float 4x DontCare", MTLPixelFormatDepth32Float, 4, NO},
    };
    const MTLLoadAction loads[] = {MTLLoadActionClear, MTLLoadActionLoad, MTLLoadActionDontCare};
    const char *loadNames[] = {"Clear", "Load", "DontCare"};
    const float rights[] = {-2, 0, 1};   // no draw, the left half, everything
    const char *drawNames[] = {"none", "half", "full"};
    printf("dontcare: %lux%lu\n", (unsigned long)w, (unsigned long)h);
    unsigned long long allOther = 0;
    for (size_t ci = 0; ci < sizeof configs / sizeof *configs; ++ci) {
        const BOOL depth = configs[ci].format == MTLPixelFormatDepth32Float;
        const NSUInteger samples = configs[ci].samples;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:configs[ci].format width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        if (samples > 1) { td.textureType = MTLTextureType2DMultisample; td.sampleCount = samples; }
        id<MTLTexture> target = [device newTextureWithDescriptor:td];
        id<MTLTexture> resolved = nil;
        if (configs[ci].resolve) {
            MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:configs[ci].format width:w height:h mipmapped:NO];
            rd.usage = MTLTextureUsageRenderTarget; rd.storageMode = MTLStorageModePrivate;
            resolved = [device newTextureWithDescriptor:rd];
        }
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [library newFunctionWithName:@"dc_vertex"];
        pd.rasterSampleCount = samples;
        if (depth) pd.depthAttachmentPixelFormat = configs[ci].format;
        else { pd.fragmentFunction = [library newFunctionWithName:@"dc_fragment"]; pd.colorAttachments[0].pixelFormat = configs[ci].format; }
        id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:pd error:&error];
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = YES;
        id<MTLDepthStencilState> depthState = [device newDepthStencilStateWithDescriptor:dd];
        NSString *kernelName = depth ? (samples > 1 ? @"dc_depth_ms" : @"dc_depth") : (samples > 1 ? @"dc_color_ms" : @"dc_color");
        id<MTLComputePipelineState> classify = [device newComputePipelineStateWithFunction:[library newFunctionWithName:kernelName] error:&error];
        if (!state || !classify || !target) { printf("  %s: not made: %s\n", configs[ci].name, error.description.UTF8String); continue; }
        unsigned long long sum[4] = {};
        char lines[2048] = ""; size_t used = 0;
        for (int li = 0; li < 3; ++li) for (int di = 0; di < 3; ++di) {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            for (int pass = 0; pass < 2; ++pass) {   // the earlier value, then the pass under test
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                MTLRenderPassAttachmentDescriptor *a = depth ? (MTLRenderPassAttachmentDescriptor *)rp.depthAttachment
                                                             : (MTLRenderPassAttachmentDescriptor *)rp.colorAttachments[0];
                a.texture = target;
                a.loadAction = pass ? loads[li] : MTLLoadActionClear;
                if (depth) rp.depthAttachment.clearDepth = pass ? 0.5 : 0.75;
                else rp.colorAttachments[0].clearColor = pass ? MTLClearColorMake(0, 0, 1, 1) : MTLClearColorMake(1, 0, 0, 1);
                if (!pass) a.storeAction = MTLStoreActionStore;
                else if (resolved) { a.resolveTexture = resolved; a.storeAction = MTLStoreActionMultisampleResolve; }
                else a.storeAction = MTLStoreActionDontCare;
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                if (pass && rights[di] > -1) {
                    float right = rights[di];
                    [enc setRenderPipelineState:state];
                    [enc setDepthStencilState:depthState];
                    [enc setVertexBytes:&right length:sizeof right atIndex:0];
                    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                }
                [enc endEncoding];
            }
            memset(counts.contents, 0, 16);
            DontCareParams params = {};
            if (depth) { params.e[0] = 0.75f; params.d[0] = 0.25f; params.u[0] = loads[li] == MTLLoadActionClear ? 0.5f : 0.75f; }
            else {
                const float e[4] = {1, 0, 0, 1}, d[4] = {0, 1, 0, 1}, c[4] = {0, 0, 1, 1};
                memcpy(params.e, e, sizeof e); memcpy(params.d, d, sizeof d);
                memcpy(params.u, loads[li] == MTLLoadActionClear ? c : e, sizeof c);
            }
            params.split = rights[di] < -1 ? 0 : rights[di] < 0.5 ? (uint32_t)(w / 2) : (uint32_t)w;
            params.known = loads[li] != MTLLoadActionDontCare;
            params.samples = (uint32_t)samples;
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:classify];
            [ce setTexture:target atIndex:0];
            [ce setBytes:&params length:sizeof params atIndex:0];
            [ce setBuffer:counts offset:0 atIndex:1];
            [ce dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
            [ce endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.error) { printf("  %s: command buffer: %s\n", configs[ci].name, cb.error.description.UTF8String); break; }
            const uint32_t *n = counts.contents;
            for (int k = 0; k < 4; ++k) sum[k] += n[k];
            used += snprintf(lines + used, sizeof lines - used, "    %-8s %-4s earlier %9u stored %9u other %9u unknown %9u%s\n",
                             loadNames[li], drawNames[di], n[0], n[1], n[2], n[3], n[2] ? "  <-- a third answer" : "");
            if (used >= sizeof lines) used = sizeof lines - 1;
        }
        allOther += sum[2];
        printf("  %-34s earlier %10llu stored %10llu other %llu\n%s", configs[ci].name, sum[0], sum[1], sum[2], lines);
    }
    printf("dontcare: %s\n", allOther ? "a third answer seen" : "every texel and sample one of Metal's two answers");
    return allOther ? 2 : 0;
}

// vtdecode: one IDR frame of a 64x64 test pattern (ffmpeg's testsrc, x264 baseline), as SPS, PPS and the slice, decoded through
// VTDecompressionSession: what VideoToolbox chose and what came out. The way any app decodes, so it is also the trigger for
// VideoToolbox's paravirtualized decoder when the guest offers it.
static const uint8_t kTinySPS[21] = {
    0x67, 0x42, 0xc0, 0x1e, 0xdc, 0x42, 0x6c, 0x04, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x0f, 0x03, 0xc5, 0x8b, 0xe0,
};
static const uint8_t kTinyPPS[5] = {
    0x68, 0xce, 0x0f, 0x2c, 0x80,
};
static const uint8_t kTinyIDR[888] = {
    0x65, 0x88, 0x84, 0x37, 0x0c, 0x60, 0x00, 0x81, 0x1a, 0x98, 0x16, 0x4d, 0x52, 0x8d, 0x91, 0xda, 0x19, 0x1c, 0xbf, 0xff, 0x00, 0x1e, 0xbc, 0xf2,
    0x2c, 0x43, 0xbf, 0xf5, 0xd7, 0x00, 0x1e, 0x02, 0x55, 0x1f, 0x5c, 0xed, 0x20, 0x34, 0xaf, 0xeb, 0xa0, 0x01, 0xef, 0xcb, 0x22, 0xc6, 0x3b, 0xff,
    0x5d, 0x06, 0x1c, 0x40, 0x00, 0x2c, 0x0e, 0x44, 0x00, 0x01, 0x02, 0x90, 0x00, 0x10, 0x0c, 0x30, 0x02, 0x02, 0xde, 0x85, 0x95, 0x42, 0x11, 0xd6,
    0xf6, 0x3e, 0xaa, 0x10, 0x00, 0x26, 0xda, 0x04, 0x64, 0x38, 0x3a, 0xca, 0x22, 0x40, 0xf4, 0x92, 0x04, 0x64, 0x30, 0x32, 0x8a, 0x22, 0x6e, 0x57,
    0x1d, 0x80, 0x3f, 0x0d, 0x40, 0x24, 0x7e, 0x80, 0x00, 0x40, 0x1b, 0x66, 0xcf, 0xe3, 0x35, 0xdb, 0x1f, 0x66, 0x9c, 0x78, 0x02, 0xc7, 0xe2, 0x95,
    0x9f, 0x64, 0xc1, 0x02, 0x29, 0xb0, 0xc3, 0x84, 0x00, 0xa0, 0x01, 0x40, 0x07, 0x04, 0x00, 0x07, 0xc0, 0x00, 0x41, 0x6c, 0x04, 0x02, 0xd9, 0x50,
    0x54, 0xd2, 0xd5, 0x38, 0x01, 0xda, 0x6a, 0xb0, 0x1d, 0xf7, 0xf0, 0x28, 0xd9, 0x8c, 0x1f, 0xef, 0x88, 0x2a, 0x34, 0x4c, 0xfb, 0xc0, 0xf4, 0x5c,
    0x4a, 0xa5, 0x87, 0xe2, 0x1e, 0x1f, 0xf0, 0xd4, 0x07, 0x00, 0x04, 0x0a, 0x12, 0x25, 0x2d, 0xfc, 0x0f, 0x06, 0x05, 0x64, 0x04, 0x77, 0x38, 0x3a,
    0xf3, 0xf0, 0x80, 0x40, 0x02, 0x07, 0x80, 0x00, 0x81, 0x38, 0x40, 0x00, 0x6c, 0x30, 0x0c, 0xd9, 0x00, 0x01, 0x06, 0x11, 0x2c, 0x03, 0x20, 0x82,
    0xc4, 0xce, 0x10, 0x15, 0xb7, 0xf3, 0xf2, 0xab, 0x96, 0x00, 0xbe, 0x8c, 0x0b, 0x1a, 0x62, 0x82, 0x01, 0xdd, 0x3f, 0xdf, 0xf3, 0x0f, 0xac, 0x12,
    0x9e, 0x00, 0x00, 0x08, 0x03, 0x60, 0x02, 0xf5, 0x48, 0x29, 0xa0, 0xde, 0xf3, 0xd6, 0x78, 0xa0, 0x00, 0x20, 0x0d, 0xfe, 0x00, 0x0f, 0x35, 0x69,
    0xd4, 0xf9, 0xdf, 0xf4, 0x3f, 0x49, 0x89, 0xd3, 0x14, 0xdf, 0xb3, 0xe9, 0x83, 0xd0, 0x07, 0xbf, 0x3c, 0xc2, 0x5e, 0xff, 0x9c, 0x5f, 0xff, 0xaf,
    0x08, 0x00, 0x0b, 0x00, 0x79, 0x01, 0x01, 0x00, 0x00, 0x81, 0x48, 0x00, 0x08, 0x2b, 0xf0, 0x01, 0x55, 0xeb, 0xb0, 0x94, 0x8f, 0xf3, 0x83, 0x02,
    0x9f, 0x9a, 0xe7, 0x26, 0x7e, 0x88, 0x9f, 0xb0, 0x27, 0x8c, 0x02, 0xb8, 0x5b, 0x0e, 0x0c, 0x08, 0x69, 0x0d, 0x14, 0x85, 0xce, 0x27, 0xa5, 0x01,
    0xd9, 0x98, 0x83, 0xb4, 0x3f, 0x09, 0xc0, 0x03, 0xe3, 0x1e, 0x26, 0xa5, 0xf5, 0x11, 0x5f, 0xff, 0xf9, 0x79, 0xb6, 0xc6, 0xaa, 0xfe, 0xca, 0xa5,
    0x15, 0x5e, 0x1a, 0x14, 0x7a, 0xc9, 0x89, 0x52, 0x95, 0xfc, 0x92, 0x43, 0xf0, 0x91, 0x8c, 0x97, 0xf1, 0x35, 0x28, 0xdc, 0x25, 0xe1, 0x87, 0x4e,
    0xa9, 0xd3, 0x94, 0x20, 0x94, 0xfa, 0x41, 0x87, 0x3d, 0x80, 0x05, 0xf0, 0x05, 0x4c, 0x46, 0x89, 0xda, 0x66, 0xff, 0xb4, 0x01, 0x25, 0x63, 0x2d,
    0xdc, 0x5d, 0x42, 0x33, 0x5e, 0xef, 0x99, 0x3c, 0x8c, 0x87, 0xac, 0x54, 0xd6, 0x41, 0x5f, 0x82, 0xe6, 0xa2, 0xe6, 0x0c, 0x76, 0x3c, 0x21, 0xff,
    0xf8, 0x2d, 0x81, 0xc0, 0x00, 0xa0, 0x00, 0x08, 0x02, 0x8f, 0x41, 0x80, 0x3e, 0x30, 0xd9, 0x46, 0x4b, 0x8d, 0x71, 0xa0, 0x59, 0xea, 0xf7, 0xb7,
    0x8d, 0x4b, 0xe5, 0xd1, 0x4f, 0x2a, 0x2a, 0xd5, 0x4b, 0x76, 0xe3, 0x52, 0xf1, 0x4f, 0x32, 0x71, 0xd9, 0x6b, 0x65, 0xe6, 0xdf, 0xff, 0xfb, 0x30,
    0x5d, 0x80, 0xe0, 0x00, 0x54, 0x02, 0x02, 0xc8, 0x26, 0x40, 0x09, 0x35, 0x81, 0xaf, 0x3a, 0xe2, 0x17, 0xb1, 0x7d, 0xff, 0xe7, 0x7b, 0x77, 0xcb,
    0xb8, 0x86, 0x3c, 0x30, 0x56, 0xf1, 0x01, 0x00, 0x09, 0xc1, 0xc1, 0x1f, 0xf0, 0x70, 0x46, 0xe2, 0xde, 0x00, 0xa8, 0x6d, 0x30, 0xd2, 0x1a, 0xfc,
    0x86, 0x11, 0x54, 0x98, 0x1f, 0x9b, 0x89, 0xe0, 0x00, 0x20, 0x05, 0x80, 0x08, 0x2b, 0x76, 0xf9, 0x1f, 0xab, 0xbe, 0x8f, 0xb0, 0xa0, 0x00, 0x20,
    0x22, 0x00, 0x18, 0x00, 0x16, 0x18, 0x00, 0x08, 0x0c, 0x00, 0x00, 0x80, 0xb8, 0x00, 0x08, 0x10, 0x01, 0x40, 0x0d, 0x0a, 0x54, 0x68, 0x18, 0xd1,
    0x8e, 0x4f, 0x02, 0x14, 0xd6, 0x8c, 0x75, 0x7e, 0xc1, 0xd0, 0x00, 0xd6, 0x12, 0x80, 0x06, 0xa2, 0x86, 0x84, 0x49, 0x3c, 0x02, 0x71, 0x7d, 0x96,
    0x95, 0x0e, 0x08, 0x91, 0x6a, 0x3f, 0xa3, 0xfc, 0x82, 0xa0, 0x08, 0x3c, 0x1e, 0x1f, 0x14, 0x78, 0x9f, 0x37, 0xc5, 0x91, 0x12, 0x87, 0xa1, 0x38,
    0xcb, 0x00, 0xc1, 0x96, 0x3c, 0x11, 0xb3, 0x41, 0xcd, 0x19, 0x1c, 0x0e, 0x13, 0x08, 0x2e, 0x5a, 0x20, 0x13, 0xa3, 0x12, 0xf2, 0x17, 0x47, 0x4f,
    0xd4, 0x30, 0x80, 0x00, 0xc0, 0x00, 0x08, 0x17, 0xb0, 0x24, 0x20, 0x00, 0x26, 0x00, 0x02, 0x00, 0xb8, 0x00, 0x69, 0x91, 0x1e, 0x62, 0xd8, 0x07,
    0x59, 0x22, 0x86, 0xb6, 0x98, 0x02, 0xb9, 0xf3, 0x80, 0x51, 0xb3, 0x35, 0xf0, 0x0c, 0xbd, 0xf1, 0x81, 0x01, 0x66, 0x28, 0x93, 0x1f, 0x10, 0xfe,
    0xb4, 0x30, 0x7a, 0xdf, 0xef, 0x10, 0x21, 0x96, 0x5f, 0xfe, 0x10, 0x10, 0x07, 0x78, 0x38, 0x4b, 0xc1, 0xc2, 0x1f, 0xe0, 0xe1, 0x0b, 0xc1, 0xd0,
    0xbe, 0x00, 0xa0, 0xd0, 0xce, 0xa0, 0x67, 0xe3, 0x54, 0xce, 0x37, 0xe7, 0x7e, 0x20, 0x19, 0x05, 0x26, 0x22, 0x65, 0xcf, 0xc2, 0xe6, 0x2a, 0xb4,
    0x5e, 0x4e, 0x20, 0x40, 0x10, 0x00, 0x10, 0x32, 0x68, 0x00, 0x08, 0x10, 0x04, 0x00, 0x02, 0x02, 0x21, 0xc0, 0x06, 0xf1, 0x7a, 0x0e, 0xb0, 0x85,
    0xb8, 0xd5, 0x00, 0x0d, 0xa1, 0x8a, 0x9d, 0x80, 0x7d, 0x6d, 0x6d, 0x01, 0x00, 0x32, 0x57, 0xdf, 0x22, 0x00, 0x06, 0xb1, 0x38, 0x00, 0x13, 0xac,
    0x2a, 0x0d, 0xa5, 0x3d, 0x9e, 0x01, 0x3a, 0x5e, 0x6d, 0xb4, 0x04, 0x01, 0x05, 0xa2, 0xff, 0xb0, 0xc3, 0xb3, 0x94, 0x00, 0x07, 0x8a, 0xb1, 0x43,
    0xc4, 0x3c, 0x67, 0xe0, 0x07, 0xd2, 0xde, 0xf4, 0x16, 0x77, 0x4f, 0xd8, 0x6e, 0x93, 0xcb, 0xfc, 0x00, 0x00, 0x07, 0x3e, 0x3c, 0x04, 0xe2, 0x8c,
    0x96, 0x7d, 0xf0, 0x30, 0x10, 0x20, 0x40, 0x40, 0x08, 0x00, 0x30, 0x0b, 0x68, 0xd5, 0x16, 0x61, 0xc0, 0x00, 0x41, 0x8d, 0xc6, 0x22, 0x5e, 0x64,
    0xc6, 0xff, 0x8f, 0x18, 0x10, 0x6d, 0x01, 0xf7, 0x67, 0x93, 0x4c, 0x6b, 0xdc, 0x58, 0xfa, 0xe3, 0x1c, 0x0a, 0x61, 0xff, 0x10, 0x13, 0x1a, 0x5c,
};


static int vtFramesOut, vtFramesFailed;

static void vtDecoded(void *refcon, void *frameRefcon, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image,
                      CMTime pts, CMTime duration) {
    (void)refcon; (void)frameRefcon; (void)duration;
    if (status || !image) {
        if (vtFramesFailed++ < 4) printf("vtdecode: frame %lld status %d flags %#x\n", pts.value, (int)status, (unsigned)flags);
        return;
    }
    if (vtFramesOut++) return;
    CVPixelBufferRef buffer = (CVPixelBufferRef)image;
    const OSType format = CVPixelBufferGetPixelFormatType(buffer);
    printf("vtdecode: first frame %lld: %zux%zu '%.4s' planes %zu IOSurface %u\n", pts.value, CVPixelBufferGetWidth(buffer),
           CVPixelBufferGetHeight(buffer), (const char *)&(OSType){CFSwapInt32HostToBig(format)}, CVPixelBufferGetPlaneCount(buffer),
           CVPixelBufferGetIOSurface(buffer) ? IOSurfaceGetID(CVPixelBufferGetIOSurface(buffer)) : 0);
    if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly)) return;
    const uint8_t *luma = CVPixelBufferIsPlanar(buffer) ? CVPixelBufferGetBaseAddressOfPlane(buffer, 0) : CVPixelBufferGetBaseAddress(buffer);
    const size_t row = CVPixelBufferIsPlanar(buffer) ? CVPixelBufferGetBytesPerRowOfPlane(buffer, 0) : CVPixelBufferGetBytesPerRow(buffer);
    const size_t width = CVPixelBufferGetWidth(buffer), height = CVPixelBufferGetHeight(buffer);
    if (luma) {
        printf("vtdecode: luma row 8: %u %u %u %u %u %u %u %u\n", luma[8 * row + 0], luma[8 * row + 8], luma[8 * row + 16],
               luma[8 * row + 24], luma[8 * row + 32], luma[8 * row + 40], luma[8 * row + 48], luma[8 * row + 56]);
        uint64_t sum = 0;
        for (size_t y = 0; y < height; ++y) for (size_t x = 0; x < width; ++x) sum += luma[y * row + x];
        printf("vtdecode: mean luma %.2f\n", (double)sum / (double)(width * height));
    }
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
}

// An Annex B stream's NAL units: [start, end) of each, start codes skipped.
static size_t vtNalUnits(const uint8_t *d, size_t n, const uint8_t **starts, size_t *sizes, size_t max) {
    size_t count = 0, i = 0;
    while (i + 3 <= n && count < max) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            const size_t begin = i + 3;
            size_t j = begin;
            while (j + 3 <= n && !(d[j] == 0 && d[j + 1] == 0 && (d[j + 2] == 1 || (d[j + 2] == 0 && j + 3 < n && d[j + 3] == 1)))) ++j;
            if (j + 3 > n) j = n;
            starts[count] = d + begin;
            sizes[count++] = j - begin;
            i = j;
        } else ++i;
    }
    return count;
}

static int vtDecodeProbe(int argc, char **argv) {
    bool hardware = false;
    const char *path = NULL;
    long wanted = 30;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "hw")) hardware = true;
        else if (!strcmp(argv[i], "paravirt")) {
            int one = 1, stage = 0;
            size_t size = sizeof stage;
            const int r = sysctlbyname("kern.droidvm_vt_paravirt", NULL, NULL, &one, sizeof one);
            sysctlbyname("kern.droidvm_vt_paravirt", &stage, &size, NULL, 0);
            printf("vtdecode: kern.droidvm_vt_paravirt=1: %s, stage %d\n", r ? strerror(errno) : "ok", stage);
        } else if (!path) path = argv[i];
        else wanted = strtol(argv[i], NULL, 0);
    }
    // The stream: the embedded IDR, or a file's NAL units grouped into access units.
    const uint8_t *nals[4096];
    size_t sizes[4096], count = 0;
    NSData *file = nil;
    bool hevc = false;
    if (path) {
        file = [NSData dataWithContentsOfFile:@(path)];
        if (!file) { printf("vtdecode: cannot read %s\n", path); return 1; }
        count = vtNalUnits(file.bytes, file.length, nals, sizes, 4096);
        // HEVC: a VPS (type 32) leads the stream; H.264 has none (its type 7 SPS first).
        for (size_t i = 0; i < count && i < 4; ++i) if (((nals[i][0] >> 1) & 0x3f) == 32) hevc = true;
    } else {
        nals[0] = kTinySPS; sizes[0] = sizeof kTinySPS;
        nals[1] = kTinyPPS; sizes[1] = sizeof kTinyPPS;
        nals[2] = kTinyIDR; sizes[2] = sizeof kTinyIDR;
        count = 3;
    }
    const uint8_t *sets[3];
    size_t setSizes[3], setCount = 0;
    for (size_t i = 0; i < count && setCount < 3; ++i) {
        const unsigned type = hevc ? (nals[i][0] >> 1) & 0x3f : nals[i][0] & 0x1f;
        if (hevc ? (type >= 32 && type <= 34) : (type == 7 || type == 8)) { sets[setCount] = nals[i]; setSizes[setCount++] = sizes[i]; }
    }
    CMVideoFormatDescriptionRef format = NULL;
    OSStatus status = hevc ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, setCount, sets, setSizes, 4, NULL, &format)
                           : CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, setCount, sets, setSizes, 4, &format);
    if (status) { printf("vtdecode: format description %d (%zu parameter sets)\n", (int)status, setCount); return 1; }
    const CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format);
    printf("vtdecode: %s %dx%d, %zu NAL units; hardware decode supported: %d\n", hevc ? "HEVC" : "H.264", dimensions.width,
           dimensions.height, count, VTIsHardwareDecodeSupported(hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264));
    NSDictionary *specification = hardware ? @{(id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES} : @{};
    NSDictionary *attributes = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
                                 (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
    VTDecompressionOutputCallbackRecord callback = {vtDecoded, NULL};
    VTDecompressionSessionRef session = NULL;
    const uint64_t begin = mach_absolute_time();
    status = VTDecompressionSessionCreate(kCFAllocatorDefault, format, (__bridge CFDictionaryRef)specification,
                                          (__bridge CFDictionaryRef)attributes, &callback, &session);
    printf("vtdecode: session %d (%.1f ms)\n", (int)status, (mach_absolute_time() - begin) / 24000.0);
    if (status) { CFRelease(format); return 1; }
    CFBooleanRef usingHardware = NULL;
    if (!VTSessionCopyProperty(session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, kCFAllocatorDefault,
                               &usingHardware) && usingHardware) {
        printf("vtdecode: using a hardware decoder: %d\n", CFBooleanGetValue(usingHardware));
        CFRelease(usingHardware);
    }
    // Access units: a VCL NAL unit that starts a picture (H.264 first_mb_in_slice 0, HEVC first_slice_segment_in_pic_flag)
    // opens one; each is given as length-prefixed NAL units.
    const uint64_t start = mach_absolute_time();
    long frames = 0;
    size_t i = 0;
    while (i < count && frames < wanted) {
        size_t first = i, last = i;
        bool picture = false;
        for (; last < count; ++last) {
            const uint8_t *nal = nals[last];
            const unsigned type = hevc ? (nal[0] >> 1) & 0x3f : nal[0] & 0x1f;
            const bool vcl = hevc ? type < 32 : (type >= 1 && type <= 5);
            const bool starts = vcl && (hevc ? (nal[2] & 0x80) : (nal[1] & 0x80));
            if (starts && picture) break;
            if (vcl) picture = true;
        }
        i = last;
        if (!picture) continue;
        size_t bytes = 0;
        for (size_t k = first; k < last; ++k) bytes += 4 + sizes[k];
        uint8_t *sample = malloc(bytes), *at = sample;
        for (size_t k = first; k < last; ++k) {
            const uint32_t length = CFSwapInt32HostToBig((uint32_t)sizes[k]);
            memcpy(at, &length, 4);
            memcpy(at + 4, nals[k], sizes[k]);
            at += 4 + sizes[k];
        }
        CMBlockBufferRef block = NULL;
        CMSampleBufferRef buffer = NULL;
        status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, sample, bytes, kCFAllocatorMalloc, NULL, 0, bytes, 0, &block);
        CMSampleTimingInfo timing = {CMTimeMake(1, 30), CMTimeMake(frames, 30), kCMTimeInvalid};
        if (!status) status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, format, 1, 1, &timing, 1, &bytes, &buffer);
        VTDecodeInfoFlags info = 0;
        if (!status) status = VTDecompressionSessionDecodeFrame(session, buffer, kVTDecodeFrame_EnableAsynchronousDecompression, NULL, &info);
        if (status) printf("vtdecode: access unit %ld: %d\n", frames, (int)status);
        if (buffer) CFRelease(buffer);
        if (block) CFRelease(block);
        ++frames;
    }
    VTDecompressionSessionWaitForAsynchronousFrames(session);
    const double ms = (mach_absolute_time() - start) / 24000.0;
    printf("vtdecode: %ld access units in, %d frames out, %d failed, %.1f ms (%.1f fps)\n", frames, vtFramesOut, vtFramesFailed, ms,
           ms > 0 ? vtFramesOut * 1000.0 / ms : 0.0);
    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    CFRelease(format);
    return vtFramesOut ? 0 : 1;
}

// vtencode: N frames of a moving gradient through VTCompressionSession, the output kept as Annex B.
static FILE *vtEncodeOut;
static int vtEncoded, vtEncodeFailed, vtKeyFrames;
static size_t vtEncodedBytes;

static void vtEncodedFrame(void *refcon, void *frameRefcon, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
    (void)refcon; (void)frameRefcon;
    if (status || !sample) {
        if (vtEncodeFailed++ < 4) printf("vtencode: frame status %d flags %#x\n", (int)status, (unsigned)flags);
        return;
    }
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    const bool key = !attachments || !CFArrayGetCount(attachments) ||
                     !CFDictionaryContainsKey(CFArrayGetValueAtIndex(attachments, 0), kCMSampleAttachmentKey_NotSync);
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
    const bool hevc = CMFormatDescriptionGetMediaSubType(format) == kCMVideoCodecType_HEVC;
    if (key) {
        ++vtKeyFrames;
        size_t count = 0;
        int header = 0;
        if (hevc) CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, 0, NULL, NULL, &count, &header);
        else CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, NULL, NULL, &count, &header);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t *set = NULL;
            size_t size = 0;
            if (hevc) CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, i, &set, &size, NULL, NULL);
            else CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &set, &size, NULL, NULL);
            static const uint8_t start[4] = {0, 0, 0, 1};
            if (vtEncodeOut && set) { fwrite(start, 1, 4, vtEncodeOut); fwrite(set, 1, size, vtEncodeOut); }
        }
    }
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    const size_t length = CMBlockBufferGetDataLength(block);
    uint8_t *bytes = malloc(length);
    CMBlockBufferCopyDataBytes(block, 0, length, bytes);
    for (size_t at = 0; at + 4 <= length;) {
        const uint32_t n = (uint32_t)bytes[at] << 24 | (uint32_t)bytes[at + 1] << 16 | (uint32_t)bytes[at + 2] << 8 | bytes[at + 3];
        static const uint8_t start[4] = {0, 0, 0, 1};
        if (at + 4 + n > length) break;
        if (vtEncodeOut) { fwrite(start, 1, 4, vtEncodeOut); fwrite(bytes + at + 4, 1, n, vtEncodeOut); }
        at += 4 + n;
    }
    free(bytes);
    if (vtEncoded < 3 || key) printf("vtencode: frame %lld: %zu bytes%s\n", CMSampleBufferGetPresentationTimeStamp(sample).value, length,
                                     key ? " (key)" : "");
    vtEncodedBytes += length;
    ++vtEncoded;
}

static int vtEncodeProbe(int argc, char **argv) {
    bool paravirt = false, hevc = false;
    int numbers[3] = {1280, 720, 30}, numberCount = 0;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "paravirt")) paravirt = true;
        else if (!strcmp(argv[i], "hevc")) hevc = true;
        else if (numberCount < 3) numbers[numberCount++] = atoi(argv[i]);
    }
    const int width = numbers[0], height = numbers[1], frames = numbers[2];
    if (paravirt) {
        int one = 1;
        const int r = sysctlbyname("kern.droidvm_vt_paravirt", NULL, NULL, &one, sizeof one);
        printf("vtencode: kern.droidvm_vt_paravirt=1: %s\n", r ? strerror(errno) : "ok");
    }
    NSMutableDictionary *specification = [NSMutableDictionary dictionary];
    if (paravirt)
        specification[(id)kVTVideoEncoderSpecification_EncoderID] = hevc ? @"paravirtualized:com.droidvm.videoencoder.virtio-media.hevc"
                                                                         : @"paravirtualized:com.droidvm.videoencoder.virtio-media.avc";
    VTCompressionSessionRef session = NULL;
    const uint64_t begin = mach_absolute_time();
    OSStatus status = VTCompressionSessionCreate(kCFAllocatorDefault, width, height, hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264,
                                                 (__bridge CFDictionaryRef)specification, NULL, NULL, vtEncodedFrame, NULL, &session);
    printf("vtencode: %s %dx%d session %d (%.1f ms)\n", hevc ? "HEVC" : "H.264", width, height, (int)status,
           (mach_absolute_time() - begin) / 24000.0);
    if (status) return 1;
    CFStringRef encoderID = NULL;
    if (!VTSessionCopyProperty(session, kVTCompressionPropertyKey_EncoderID, kCFAllocatorDefault, &encoderID) && encoderID) {
        printf("vtencode: encoder %s\n", [(__bridge NSString *)encoderID UTF8String]);
        CFRelease(encoderID);
    }
    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate, (__bridge CFNumberRef)@(width * height * 4));
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, (__bridge CFNumberRef)@30);
    VTCompressionSessionPrepareToEncodeFrames(session);
    vtEncodeOut = fopen(hevc ? "/private/var/ngpu/enc.hevc" : "/private/var/ngpu/enc.h264", "wb");
    NSDictionary *attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
    const uint64_t start = mach_absolute_time();
    for (int f = 0; f < frames; ++f) {
        CVPixelBufferRef pixels = NULL;
        CVPixelBufferCreate(kCFAllocatorDefault, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                            (__bridge CFDictionaryRef)attributes, &pixels);
        CVPixelBufferLockBaseAddress(pixels, 0);
        uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pixels, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(pixels, 1);
        const size_t yRow = CVPixelBufferGetBytesPerRowOfPlane(pixels, 0), uvRow = CVPixelBufferGetBytesPerRowOfPlane(pixels, 1);
        for (int r = 0; r < height; ++r)
            for (int x = 0; x < width; ++x) y[r * yRow + x] = (uint8_t)(16 + ((x + r + f * 8) % 220));
        for (int r = 0; r < height / 2; ++r)
            for (int x = 0; x < width; x += 2) { uv[r * uvRow + x] = (uint8_t)(64 + (r % 128)); uv[r * uvRow + x + 1] = (uint8_t)(64 + (x / 2 % 128)); }
        CVPixelBufferUnlockBaseAddress(pixels, 0);
        status = VTCompressionSessionEncodeFrame(session, pixels, CMTimeMake(f, 30), CMTimeMake(1, 30), NULL, NULL, NULL);
        if (status) printf("vtencode: frame %d: %d\n", f, (int)status);
        CVPixelBufferRelease(pixels);
    }
    status = VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    const double ms = (mach_absolute_time() - start) / 24000.0;
    printf("vtencode: complete %d: %d frames out (%d key), %d failed, %zu bytes, %.1f ms (%.1f fps)\n", (int)status, vtEncoded,
           vtKeyFrames, vtEncodeFailed, vtEncodedBytes, ms, ms > 0 ? vtEncoded * 1000.0 / ms : 0.0);
    if (vtEncodeOut) fclose(vtEncodeOut);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    return vtEncoded ? 0 : 1;
}

// argbuf: MTLArgumentEncoder as WebKit's WebGPU uses it for every bind group: the argument buffer holds a buffer's address, a
// texture's and a sampler's resource IDs and a constant; a kernel and a fragment shader read all four through it (the buffer and the
// texture made resident by useResource:usage:). Each of the four runs must give in[i] * k + red(i) + green(i) << 8 for every i.
static const char *kArgSource =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct Args { device const uint *in [[id(0)]]; texture2d<float> tex [[id(1)]]; sampler smp [[id(2)]]; uint k [[id(3)]]; };\n"
    "static uint argValue(constant Args &a, uint2 p) { uint i = p.y * 4 + p.x; float4 c = a.tex.read(p);"
    " float4 s = a.tex.sample(a.smp, (float2(p) + 0.5) / 4.0, level(0));"
    " return a.in[i] * a.k + uint(rint(c.r * 255)) + (uint(rint(s.g * 255)) << 8); }\n"
    "kernel void arg_kernel(constant Args &a [[buffer(0)]], device uint *out [[buffer(1)]], uint i [[thread_position_in_grid]]) {"
    " out[i] = argValue(a, uint2(i % 4, i / 4)); }\n"
    "struct V { float4 p [[position]]; };\n"
    "vertex V arg_vertex(uint i [[vertex_id]]) { float2 c = float2((i << 1) & 2, i & 2); V v; v.p = float4(c * 4 - 1, 0, 1); return v; }\n"
    "fragment uint4 arg_fragment(V v [[stage_in]], constant Args &a [[buffer(0)]]) { return uint4(argValue(a, uint2(v.p.xy)), 0, 0, 1); }\n";

static int argBufferProbe(void) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { printf("argbuf: no Metal device\n"); return 1; }
    printf("argumentBuffersSupport %lu maxArgumentBufferSamplerCount %lu\n", (unsigned long)device.argumentBuffersSupport,
           (unsigned long)device.maxArgumentBufferSamplerCount);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:@(kArgSource) options:nil error:&error];
    if (!library) { printf("argbuf: no library: %s\n", error.description.UTF8String); return 1; }
    id<MTLFunction> kernel = [library newFunctionWithName:@"arg_kernel"], fragment = [library newFunctionWithName:@"arg_fragment"];
    id<MTLComputePipelineState> compute = [device newComputePipelineStateWithFunction:kernel error:&error];
    if (!compute) { printf("argbuf: no compute pipeline: %s\n", error.description.UTF8String); return 1; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [library newFunctionWithName:@"arg_vertex"];
    pd.fragmentFunction = fragment;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Uint;
    id<MTLRenderPipelineState> render = [device newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!render) { printf("argbuf: no render pipeline: %s\n", error.description.UTF8String); return 1; }

    enum { N = 16, K = 7 };
    id<MTLBuffer> in = [device newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
    for (uint32_t i = 0; i < N; ++i) ((uint32_t *)in.contents)[i] = 1000 + i * 3;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:td];
    uint8_t texels[N * 4];
    for (uint32_t i = 0; i < N; ++i) { texels[i * 4] = (uint8_t)(i * 10); texels[i * 4 + 1] = (uint8_t)(255 - i * 9); texels[i * 4 + 2] = 0; texels[i * 4 + 3] = 255; }
    [texture replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:texels bytesPerRow:16];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest; sd.supportArgumentBuffers = YES;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sd];
    printf("resource IDs: texture %#llx sampler %#llx, buffer address %#llx\n", (unsigned long long)texture.gpuResourceID._impl,
           (unsigned long long)sampler.gpuResourceID._impl, (unsigned long long)in.gpuAddress);
    uint32_t expected[N];
    for (uint32_t i = 0; i < N; ++i) expected[i] = (1000 + i * 3) * K + i * 10 + ((255 - i * 9) << 8);

    NSMutableArray<MTLArgumentDescriptor *> *arguments = [NSMutableArray array];
    const MTLDataType types[4] = {MTLDataTypePointer, MTLDataTypeTexture, MTLDataTypeSampler, MTLDataTypeUInt};
    for (NSUInteger k = 0; k < 4; ++k) {
        MTLArgumentDescriptor *a = [MTLArgumentDescriptor argumentDescriptor];
        a.dataType = types[k]; a.index = k; a.access = MTLBindingAccessReadOnly;
        if (types[k] == MTLDataTypeTexture) a.textureType = MTLTextureType2D;
        [arguments addObject:a];
    }
    id<MTLArgumentEncoder> encoders[2][2] = {
        {[device newArgumentEncoderWithArguments:arguments], [device newArgumentEncoderWithArguments:arguments]},
        {[kernel newArgumentEncoderWithBufferIndex:0], [fragment newArgumentEncoderWithBufferIndex:0]},
    };
    const char *made[2] = {"descriptors", "function"}, *runs[2] = {"kernel", "fragment"};
    id<MTLCommandQueue> queue = [device newCommandQueue];
    int failures = 0;
    for (int m = 0; m < 2; ++m)
        for (int r = 0; r < 2; ++r) {
            id<MTLArgumentEncoder> ae = encoders[m][r];
            if (!ae) { printf("%s encoder for the %s: none\n", made[m], runs[r]); ++failures; continue; }
            id<MTLBuffer> ab = [device newBufferWithLength:MAX(ae.encodedLength, (NSUInteger)16) options:MTLResourceStorageModeShared];
            [ae setArgumentBuffer:ab offset:0];
            [ae setBuffer:in offset:0 atIndex:0];
            [ae setTexture:texture atIndex:1];
            [ae setSamplerState:sampler atIndex:2];
            uint32_t *k = (uint32_t *)[ae constantDataAtIndex:3];
            if (k) *k = K;
            const uint64_t *words = (const uint64_t *)ab.contents;
            printf("%s encoder, %s: length %lu alignment %lu, words %#llx %#llx %#llx %#llx\n", made[m], runs[r],
                   (unsigned long)ae.encodedLength, (unsigned long)ae.alignment, (unsigned long long)words[0],
                   ae.encodedLength >= 16 ? (unsigned long long)words[1] : 0ull, ae.encodedLength >= 24 ? (unsigned long long)words[2] : 0ull,
                   ae.encodedLength >= 32 ? (unsigned long long)words[3] : 0ull);
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLBuffer> out = [device newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
            id<MTLTexture> target = nil;
            if (r == 0) {
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:compute];
                [enc setBuffer:ab offset:0 atIndex:0];
                [enc setBuffer:out offset:0 atIndex:1];
                [enc useResource:in usage:MTLResourceUsageRead];
                [enc useResource:texture usage:MTLResourceUsageRead];
                [enc dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(N, 1, 1)];
                [enc endEncoding];
            } else {
                MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:4 height:4 mipmapped:NO];
                rd.usage = MTLTextureUsageRenderTarget;
                target = [device newTextureWithDescriptor:rd];
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = target; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                [enc setRenderPipelineState:render];
                [enc setFragmentBuffer:ab offset:0 atIndex:0];
                [enc useResource:in usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
                [enc useResource:texture usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [enc endEncoding];
            }
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.error) { printf("  command buffer: %s\n", cb.error.description.UTF8String); ++failures; continue; }
            uint32_t got[N];
            if (target) [target getBytes:got bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
            else memcpy(got, out.contents, sizeof got);
            unsigned wrong = 0;
            for (uint32_t i = 0; i < N; ++i)
                if (got[i] != expected[i]) {
                    if (wrong++ < 4) printf("  [%u] %u, want %u\n", i, got[i], expected[i]);
                }
            printf("  %s\n", wrong ? "FAIL" : "ok");
            failures += wrong != 0;
        }
    return failures ? 1 : 0;
}

// families: what the Metal device answers for each GPU family (supportsFamily:) and the device queries a family check leans on --
// WebKit's WebGPU gives an adapter only to a device in MTLGPUFamilyApple4 or later.
static int familiesProbe(void) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        printf("no Metal device\n");
        return 1;
    }
    static const struct { const char *name; long family; } families[] = {
        {"Apple1", 1001}, {"Apple2", 1002}, {"Apple3", 1003}, {"Apple4", 1004}, {"Apple5", 1005}, {"Apple6", 1006},
        {"Apple7", 1007}, {"Apple8", 1008}, {"Apple9", 1009}, {"Apple10", 1010}, {"Mac1", 2001}, {"Mac2", 2002},
        {"Common1", 3001}, {"Common2", 3002}, {"Common3", 3003}, {"Metal3", 5001}, {"Metal4", 5002},
    };
    printf("%s:", device.name.UTF8String);
    for (size_t i = 0; i < sizeof families / sizeof *families; i++)
        printf(" %s %d", families[i].name, [device supportsFamily:(MTLGPUFamily)families[i].family]);
    printf("\nargumentBuffersSupport %lu readWriteTextureSupport %lu maxThreadgroupMemoryLength %lu maxBufferLength %llu\n",
           (unsigned long)device.argumentBuffersSupport, (unsigned long)device.readWriteTextureSupport,
           (unsigned long)device.maxThreadgroupMemoryLength, (unsigned long long)device.maxBufferLength);
    MTLSize threads = device.maxThreadsPerThreadgroup;
    printf("maxThreadsPerThreadgroup %lux%lux%lu BC %d\n", (unsigned long)threads.width, (unsigned long)threads.height,
           (unsigned long)threads.depth, device.supportsBCTextureCompression);
    // The Adreno plugin's device names the rows of Apple's tables that keep a family false (droidvmFamilyBlockers:, the runtime's
    // burn-down): Apple4 to Apple7 and Metal3, the families WebKit's WebGPU and newer apps look for.
    SEL blockers = sel_registerName("droidvmFamilyBlockers:");
    if ([device respondsToSelector:blockers]) {
        static const struct { const char *name; long family; } wanted[] = {
            {"Apple4", 1004}, {"Apple5", 1005}, {"Apple6", 1006}, {"Apple7", 1007}, {"Metal3", 5001}};
        for (size_t i = 0; i < sizeof wanted / sizeof *wanted; i++) {
            if ([device supportsFamily:(MTLGPUFamily)wanted[i].family])
                continue;
            NSString *rows = ((NSString * (*)(id, SEL, NSInteger))objc_msgSend)(device, blockers, wanted[i].family);
            printf("%s blocked by:\n", wanted[i].name);
            for (NSString *row in [rows componentsSeparatedByString:@"\n"])
                if (row.length)
                    printf("  %s\n", row.UTF8String);
        }
    }
    return 0;
}

// scnotify KEY...: configd tells every SCDynamicStore session watching each key that it changed (SCDynamicStoreNotifyValue), its value
// untouched -- what a watcher such as Preferences' Ethernet watcher does on a real change, without bouncing the interface.
static int scNotify(int count, char **keys) {
    void *sc = dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration", RTLD_NOW);
    CFTypeRef (*create)(CFAllocatorRef, CFStringRef, void *, void *) =
        sc ? (CFTypeRef (*)(CFAllocatorRef, CFStringRef, void *, void *))dlsym(sc, "SCDynamicStoreCreate") : NULL;
    Boolean (*notify)(CFTypeRef, CFStringRef) = sc ? (Boolean (*)(CFTypeRef, CFStringRef))dlsym(sc, "SCDynamicStoreNotifyValue") : NULL;
    int (*lastError)(void) = sc ? (int (*)(void))dlsym(sc, "SCError") : NULL;
    if (!create || !notify) {
        printf("no SCDynamicStoreCreate/SCDynamicStoreNotifyValue\n");
        return 1;
    }
    CFTypeRef store = create(NULL, CFSTR("gfxprobe"), NULL, NULL);
    if (!store) {
        printf("SCDynamicStoreCreate failed: %d\n", lastError ? lastError() : -1);
        return 1;
    }
    int failed = 0;
    for (int i = 0; i < count; i++) {
        CFStringRef key = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
        Boolean ok = notify(store, key);
        printf("%s: %s", keys[i], ok ? "notified\n" : "failed ");
        if (!ok) {
            printf("%d\n", lastError ? lastError() : -1);
            failed = 1;
        }
        CFRelease(key);
    }
    CFRelease(store);
    return failed;
}

// clocks: the clocks a timer can be on, side by side (mach absolute and continuous time, the POSIX clocks), and a 1-second dispatch timer
// on each of dispatch's clocks -- uptime (DISPATCH_TIME_NOW), wall, and monotonic (continuous time, which Swift's ContinuousClock and
// Task.sleep use) -- with when each fired, or that it had not after 5 seconds.
#ifndef DISPATCH_MONOTONICTIME_NOW
#define DISPATCH_MONOTONICTIME_NOW (1ull << 63)   // libdispatch's time_private.h: a time on continuous (monotonic) time
#endif
static int clocksProbe(void) {
    mach_timebase_info_data_t base;
    mach_timebase_info(&base);
    uint64_t abs = mach_absolute_time(), cont = mach_continuous_time();
    printf("timebase %u/%u; mach_absolute_time %llu, mach_continuous_time %llu (continuous - absolute = %.3f s)\n", base.numer, base.denom,
           (unsigned long long)abs, (unsigned long long)cont, (double)(cont - abs) * base.numer / base.denom / 1e9);
    static const struct { const char *name; clockid_t id; } clocks[] = {
        {"CLOCK_REALTIME", CLOCK_REALTIME}, {"CLOCK_MONOTONIC", CLOCK_MONOTONIC}, {"CLOCK_MONOTONIC_RAW", CLOCK_MONOTONIC_RAW},
        {"CLOCK_UPTIME_RAW", CLOCK_UPTIME_RAW},
    };
    for (size_t i = 0; i < sizeof clocks / sizeof *clocks; i++) {
        struct timespec ts;
        int r = clock_gettime(clocks[i].id, &ts);
        printf("  %-20s %s%ld.%09ld\n", clocks[i].name, r ? "failed " : "", (long)ts.tv_sec, ts.tv_nsec);
    }
    static const struct { const char *name; dispatch_time_t base; } timers[] = {
        {"uptime (DISPATCH_TIME_NOW)", DISPATCH_TIME_NOW},
        {"wall (DISPATCH_WALLTIME_NOW)", DISPATCH_WALLTIME_NOW},
        {"monotonic (DISPATCH_MONOTONICTIME_NOW)", DISPATCH_MONOTONICTIME_NOW},
    };
    enum { count = sizeof timers / sizeof *timers };
    double *fired = calloc(count, sizeof *fired);
    dispatch_queue_t queue = dispatch_queue_create("clocks", DISPATCH_QUEUE_SERIAL);
    const uint64_t start = mach_absolute_time();
    for (int i = 0; i < count; i++) {
        fired[i] = -1;
        dispatch_after(dispatch_time(timers[i].base, NSEC_PER_SEC), queue, ^{
            fired[i] = (double)(mach_absolute_time() - start) * base.numer / base.denom / 1e9;
        });
    }
    // The deadline as libswift_Concurrency computes it (swift_dispatchEnqueueWithDeadline): CLOCK_MONOTONIC_RAW nanoseconds as ticks
    // of Apple's 24 MHz (ns * 3 / 125) on the monotonic clock. It fires on time only where the timebase is 125/3.
    __block double swiftFired = -1;
    const uint64_t swiftDeadline = ((clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) + NSEC_PER_SEC) * 3 / 125) | DISPATCH_MONOTONICTIME_NOW;
    dispatch_after(swiftDeadline, queue, ^{
        swiftFired = (double)(mach_absolute_time() - start) * base.numer / base.denom / 1e9;
    });
    sleep(5);
    dispatch_sync(queue, ^{});
    if (swiftFired < 0)
        printf("  1 s timer as Swift computes it (ns * 3 / 125, monotonic): not fired after 5 s\n");
    else
        printf("  1 s timer as Swift computes it (ns * 3 / 125, monotonic): fired at %.3f s\n", swiftFired);
    int failed = swiftFired < 0;
    for (int i = 0; i < count; i++) {
        if (fired[i] < 0) {
            printf("  1 s timer on %s: not fired after 5 s\n", timers[i].name);
            failed = 1;
        } else
            printf("  1 s timer on %s: fired at %.3f s\n", timers[i].name, fired[i]);
    }
    free(fired);
    return failed;
}

// sysctl [NAME...]: what Mesa's heap estimate reads (sysconf _SC_PHYS_PAGES and _SC_PAGESIZE, the sysctls behind them) and each named
// sysctl, as numbers or the errno -- inside a sandbox (box) it says which the profile refuses.
static int sysctlProbe(int count, char **names) {
    errno = 0;
    long pages = sysconf(_SC_PHYS_PAGES);
    int pagesErrno = errno;
    errno = 0;
    long pageSize = sysconf(_SC_PAGESIZE);
    int pageSizeErrno = errno;
    printf("sysconf _SC_PHYS_PAGES %ld (%s), _SC_PAGESIZE %ld (%s)\n", pages, pagesErrno ? strerror(pagesErrno) : "ok", pageSize,
           pageSizeErrno ? strerror(pageSizeErrno) : "ok");
    static char *defaults[] = {"hw.memsize", "hw.physmem", "hw.pagesize", "hw.ncpu", "kern.osproductversion"};
    if (!count) {
        names = defaults;
        count = (int)(sizeof defaults / sizeof *defaults);
    }
    for (int i = 0; i < count; i++) {
        unsigned char value[256] = {};
        size_t size = sizeof value;
        if (sysctlbyname(names[i], value, &size, NULL, 0)) {
            printf("  %s: %s\n", names[i], strerror(errno));
            continue;
        }
        uint64_t number = 0;
        memcpy(&number, value, size < 8 ? size : 8);
        if (size == 4 || size == 8)
            printf("  %s: %llu\n", names[i], (unsigned long long)number);
        else
            printf("  %s: %.*s\n", names[i], (int)size, value);
    }
    return 0;
}

// ethernet: what Settings asks before it lists Ethernet (PSSystemConfigurationDynamicStoreEthernetWatcher, Preferences.framework):
// its answer, and for each network interface the conditions its ethernetNetworkInterfaces checks -- the BSD name's "en" prefix,
// isHiddenEthernetInterface: (a USB NCM or USB Ethernet provider), the IOKit interface type (6), the ifnet family (2) and subfamily
// (at most 1), and the extended flags' IFEF_DIRECTLINK (SIOCGIFEFLAGS).
static int ethernetProbe(void) {
    dlopen("/System/Library/PrivateFrameworks/Preferences.framework/Preferences", RTLD_NOW);
    dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration", RTLD_NOW);
    Class watcherClass = NSClassFromString(@"PSSystemConfigurationDynamicStoreEthernetWatcher");
    id watcher = watcherClass ? ((id (*)(id, SEL))objc_msgSend)(watcherClass, sel_registerName("sharedManager")) : nil;
    NSArray *listed = watcher ? ((id (*)(id, SEL))objc_msgSend)(watcher, sel_registerName("ethernetNetworkInterfaces")) : nil;
    BOOL has = watcher ? ((BOOL (*)(id, SEL))objc_msgSend)(watcher, sel_registerName("hasEthernetNetworkInterfaces")) : NO;
    printf("watcher %p: ethernetNetworkInterfaces %lu, hasEthernetNetworkInterfaces %d\n", (__bridge void *)watcher,
           (unsigned long)listed.count, has);
    CFArrayRef (*copyAll)(void) = (CFArrayRef (*)(void))dlsym(RTLD_DEFAULT, "SCNetworkInterfaceCopyAll");
    CFStringRef (*bsdName)(CFTypeRef) = (CFStringRef (*)(CFTypeRef))dlsym(RTLD_DEFAULT, "SCNetworkInterfaceGetBSDName");
    CFNumberRef (*ioType)(CFTypeRef) = (CFNumberRef (*)(CFTypeRef))dlsym(RTLD_DEFAULT, "_SCNetworkInterfaceGetIOInterfaceType");
    CFNumberRef (*family)(CFTypeRef) = (CFNumberRef (*)(CFTypeRef))dlsym(RTLD_DEFAULT, "_SCNetworkInterfaceGetFamilyType");
    CFNumberRef (*subfamily)(CFTypeRef) = (CFNumberRef (*)(CFTypeRef))dlsym(RTLD_DEFAULT, "_SCNetworkInterfaceGetFamilySubType");
    if (!copyAll || !bsdName) {
        printf("no SCNetworkInterfaceCopyAll\n");
        return 1;
    }
    CFArrayRef all = copyAll();
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    printf("SCNetworkInterfaceCopyAll: %ld interfaces, socket %d\n", all ? (long)CFArrayGetCount(all) : -1L, s);
    for (CFIndex i = 0; all && i < CFArrayGetCount(all); ++i) {
        CFTypeRef interface = CFArrayGetValueAtIndex(all, i);
        NSString *name = (__bridge NSString *)bsdName(interface);
        int type = -1, fam = -1, sub = -1;
        CFNumberRef n;
        if (ioType && (n = ioType(interface))) CFNumberGetValue(n, kCFNumberIntType, &type);
        if (family && (n = family(interface))) CFNumberGetValue(n, kCFNumberIntType, &fam);
        if (subfamily && (n = subfamily(interface))) CFNumberGetValue(n, kCFNumberIntType, &sub);
        BOOL hidden = watcher ? ((BOOL (*)(id, SEL, CFTypeRef))objc_msgSend)(watcher, sel_registerName("isHiddenEthernetInterface:"),
                                                                            interface) : NO;
        struct { char name[16]; uint64_t eflags; char pad[8]; } request = {};
        strncpy(request.name, name.UTF8String ?: "", sizeof request.name - 1);
        int r = s >= 0 ? ioctl(s, 0xc020698e /* SIOCGIFEFLAGS */, &request) : -1;
        printf("  %s: type %d family %d subfamily %d hidden %d eflags %s%#llx\n", name.UTF8String ?: "(none)", type, fam, sub, hidden,
               r ? "(ioctl failed) " : "", (unsigned long long)request.eflags);
    }
    if (s >= 0) close(s);
    return 0;
}

// spuopen: the motion devices (AppleSPUHIDDevice, DroidVMMotionHid.inc), and on the one that names the sensor processor's plug-in (the device orientation)
// each step CoreMotion's orientation notifier takes: IOServiceOpen with AppleSPULib's type 'ashd', its Open (selector 4), and
// IOCreatePlugInInterfaceForService with AppleSPULib's plug-in type. The kern_return_t of each.
extern kern_return_t IOConnectCallMethod(io_object_t connection, uint32_t selector, const uint64_t *input, uint32_t inputCount,
                                         const void *inputStruct, size_t inputStructCnt, uint64_t *output, uint32_t *outputCnt,
                                         void *outputStruct, size_t *outputStructCnt);
extern kern_return_t IOCreatePlugInInterfaceForService(io_object_t service, CFUUIDRef pluginType, CFUUIDRef interfaceType,
                                                       void ***theInterface, int32_t *theScore);
static int spuOpen(void) {
    io_object_t iterator = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("AppleSPUHIDDevice"), &iterator) != KERN_SUCCESS) {
        printf("no AppleSPUHIDDevice\n");
        return 1;
    }
    CFUUIDRef type = CFUUIDGetConstantUUIDWithBytes(NULL, 0x7a, 0xcf, 0x53, 0x32, 0x1a, 0x35, 0x48, 0x93, 0x87, 0xcb, 0xba, 0x64, 0xe1, 0x88,
                                                    0x7f, 0xae);
    CFUUIDRef plugin = CFUUIDGetConstantUUIDWithBytes(NULL, 0xc2, 0x44, 0xe8, 0x58, 0x10, 0x9c, 0x11, 0xd4, 0x91, 0xd4, 0x00, 0x50, 0xe4,
                                                      0xc6, 0x42, 0x6f);
    io_object_t service;
    while ((service = IOIteratorNext(iterator))) {
        CFTypeRef product = IORegistryEntryCreateCFProperty(service, CFSTR("Product"), NULL, 0);
        CFTypeRef types = IORegistryEntryCreateCFProperty(service, CFSTR("IOCFPlugInTypes"), NULL, 0);
        char name[128] = "?";
        if (product && CFGetTypeID(product) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)product, name, sizeof name, kCFStringEncodingUTF8);
        printf("%s: IOCFPlugInTypes %s\n", name, types ? "yes" : "no");
        if (types) {
            io_object_t connect = 0;
            kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0x61736864, &connect);
            printf("  IOServiceOpen 'ashd' %#x\n", kr);
            if (kr == KERN_SUCCESS) {
                printf("  selector 4 (Open) %#x\n", IOConnectCallMethod(connect, 4, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL));
                uint64_t command = 0x20;
                uint8_t config[8] = {0}, reply[64];
                size_t replySize = sizeof reply;
                printf("  selector 6 (SendCommand) %#x, reply %zu bytes\n",
                       IOConnectCallMethod(connect, 6, &command, 1, config, sizeof config, NULL, NULL, reply, &replySize), replySize);
                IOServiceClose(connect);
            }
            void **interface = NULL;
            int32_t score = 0;
            kr = IOCreatePlugInInterfaceForService(service, type, plugin, &interface, &score);
            printf("  IOCreatePlugInInterfaceForService %#x\n", kr);
            CFRelease(types);
        }
        if (product) CFRelease(product);
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    return 0;
}

// ktxdecode PATH: decode an image file (a SplashBoard snapshot's .ktx, ASTC 4x4) through ImageIO into a BGRA8 bitmap and count its
// colours -- the app switcher showed white cards for snapshots whose ASTC blocks held the apps' dark screens. A process without a
// Metal device (SpringBoard here) and one with it decode alike only if ImageIO's ASTC decoder needs no GPU.
static int ktxDecode(const char *path) {
    void *imageio = dlopen("/System/Library/Frameworks/ImageIO.framework/ImageIO", RTLD_NOW);
    typedef CFTypeRef (*CreateSource)(CFURLRef, CFDictionaryRef);
    typedef CGImageRef (*CreateImage)(CFTypeRef, size_t, CFDictionaryRef);
    typedef CFStringRef (*GetType)(CFTypeRef);
    typedef size_t (*GetCount)(CFTypeRef);
    typedef int (*GetStatus)(CFTypeRef);
    typedef CFArrayRef (*CopyTypes)(void);
    CreateSource createSource = imageio ? (CreateSource)dlsym(imageio, "CGImageSourceCreateWithURL") : NULL;
    CreateImage createImage = imageio ? (CreateImage)dlsym(imageio, "CGImageSourceCreateImageAtIndex") : NULL;
    GetType getType = imageio ? (GetType)dlsym(imageio, "CGImageSourceGetType") : NULL;
    GetCount getCount = imageio ? (GetCount)dlsym(imageio, "CGImageSourceGetCount") : NULL;
    GetStatus getStatus = imageio ? (GetStatus)dlsym(imageio, "CGImageSourceGetStatus") : NULL;
    CopyTypes copyTypes = imageio ? (CopyTypes)dlsym(imageio, "CGImageSourceCopyTypeIdentifiers") : NULL;
    if (!createSource || !createImage || !getType || !getCount || !getStatus || !copyTypes) { printf("ktx: no ImageIO\n"); return 1; }
    printf("ktx: Metal device %s\n", MTLCreateSystemDefaultDevice() ? "present" : "absent");
    CFArrayRef types = copyTypes();
    printf("ktx: ImageIO reads com.apple.atx: %s\n",
           types && CFArrayContainsValue(types, CFRangeMake(0, CFArrayGetCount(types)), CFSTR("com.apple.atx")) ? "yes" : "no");
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageRef image = NULL;
    for (int hinted = 0; hinted < 2 && !image; ++hinted) {
        const void *keys[1] = {CFSTR("kCGImageSourceTypeIdentifierHint")}, *values[1] = {CFSTR("com.apple.atx")};
        CFDictionaryRef options = hinted ? CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                              &kCFTypeDictionaryValueCallBacks) : NULL;
        CFTypeRef source = createSource(url, options);
        CFStringRef type = source ? getType(source) : NULL;
        printf("ktx: %s source: type %s, %zu images, status %d\n", hinted ? "hinted" : "plain",
               type ? CFStringGetCStringPtr(type, kCFStringEncodingUTF8) : "(none)", source ? getCount(source) : 0,
               source ? getStatus(source) : -99);
        image = source ? createImage(source, 0, options) : NULL;
    }
    if (!image) { printf("ktx: %s: no image\n", path); return 1; }
    const size_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);
    printf("ktx: %zux%zu, %zu bits a pixel, %zu a component, bitmap info %#x\n", w, h, CGImageGetBitsPerPixel(image),
           CGImageGetBitsPerComponent(image), (unsigned)CGImageGetBitmapInfo(image));
    uint32_t *texels = calloc(w * h, 4);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(texels, w, h, 8, w * 4, space,
                                                 (CGBitmapInfo)(kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little));
    CGContextDrawImage(context, CGRectMake(0, 0, w, h), image);
    unsigned white = 0, black = 0, clear = 0, other = 0;
    for (size_t i = 0; i < w * h; ++i) {
        const uint32_t v = texels[i];
        if (v == 0xffffffffu) ++white; else if (v == 0xff000000u) ++black; else if (!(v >> 24)) ++clear; else ++other;
    }
    printf("ktx: white %u, black %u, transparent %u, other %u; centre %08x\n", white, black, clear, other, texels[(h / 2) * w + w / 2]);
    return 0;
}

static int tessCheck(void) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:kTessSource options:nil error:&error];
    if (!library) { printf("tess: no library: %s\n", error.description.UTF8String); return 1; }
    enum { Size = 128 };
    static const float triangle[3][2] = {{-0.8f, -0.8f}, {0.8f, -0.8f}, {0.0f, 0.8f}};
    static const float quad[4][2] = {{-0.7f, -0.6f}, {0.6f, -0.7f}, {0.7f, 0.6f}, {-0.6f, 0.7f}};
    id<MTLCommandQueue> queue = [device newCommandQueue];
    int failures = 0;
    for (int shape = 0; shape < 2; ++shape)
        for (int partition = 0; partition < 2; ++partition)
            for (int f = 0; f < 3; ++f) {
                const float factor = (float[]){1, 4, 16}[f];
                const bool quadPatch = shape == 1;
                MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
                pd.vertexFunction = [library newFunctionWithName:quadPatch ? @"quad_vertex" : @"tri_vertex"];
                pd.fragmentFunction = [library newFunctionWithName:@"color_fragment"];
                pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
                pd.maxTessellationFactor = 16;
                pd.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
                pd.tessellationPartitionMode = partition ? MTLTessellationPartitionModeFractionalEven : MTLTessellationPartitionModeInteger;
                pd.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionConstant;
                pd.tessellationOutputWindingOrder = MTLWindingCounterClockwise;
                MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
                vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
                vd.layouts[0].stride = 16; vd.layouts[0].stepFunction = MTLVertexStepFunctionPerPatchControlPoint;
                pd.vertexDescriptor = vd;
                id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:pd error:&error];
                const char *name = quadPatch ? "quad" : "triangle", *mode = partition ? "fractional-even" : "integer";
                if (!state) { printf("tess %s %s %g: no pipeline: %s\n", name, mode, factor, error.description.UTF8String); ++failures; continue; }
                const int corners = quadPatch ? 4 : 3;
                float points[4][4];
                for (int i = 0; i < corners; ++i) {
                    points[i][0] = quadPatch ? quad[i][0] : triangle[i][0]; points[i][1] = quadPatch ? quad[i][1] : triangle[i][1];
                    points[i][2] = 0.5f; points[i][3] = 1;
                }
                id<MTLBuffer> controlPoints = [device newBufferWithBytes:points length:sizeof points options:MTLResourceStorageModeShared];
                __fp16 factors[6];
                for (int i = 0; i < 6; ++i) factors[i] = (__fp16)factor;
                id<MTLBuffer> factorBuffer = [device newBufferWithBytes:factors length:sizeof factors options:MTLResourceStorageModeShared];
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:Size height:Size
                                                                                         mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
                id<MTLTexture> target = [device newTextureWithDescriptor:td];
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = target; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                [enc setRenderPipelineState:state];
                [enc setVertexBuffer:controlPoints offset:0 atIndex:0];
                [enc setTessellationFactorBuffer:factorBuffer offset:0 instanceStride:0];
                [enc drawPatches:(NSUInteger)corners patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1
                    baseInstance:0];
                [enc endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                if (cb.error) { printf("tess %s %s %g: command buffer: %s\n", name, mode, factor, cb.error.description.UTF8String); ++failures; continue; }
                static uint8_t texels[Size * Size * 4];
                [target getBytes:texels bytesPerRow:Size * 4 fromRegion:MTLRegionMake2D(0, 0, Size, Size) mipmapLevel:0];
                int stray = 0, holes = 0, lit = 0;
                for (int y = 0; y < Size; ++y)
                    for (int x = 0; x < Size; ++x) {
                        const uint8_t *t = texels + (y * Size + x) * 4;
                        const bool on = t[0] | t[1] | t[2];
                        lit += on;
                        // Texels within a texel of an edge may go either way.
                        bool sure = true, inside = tessInside(quadPatch ? quad : triangle, corners, x, y, Size);
                        for (int dy = -1; dy <= 1 && sure; ++dy)
                            for (int dx = -1; dx <= 1 && sure; ++dx)
                                if (tessInside(quadPatch ? quad : triangle, corners, x + dx, y + dy, Size) != inside) sure = false;
                        if (!sure) continue;
                        if (on && !inside) ++stray;
                        if (!on && inside) ++holes;
                    }
                const bool bad = stray || holes;
                failures += bad;
                printf("tess %-8s %-15s factor %2g: %5d texels lit, %4d stray, %4d holes%s\n", name, mode, factor, lit, stray, holes,
                       bad ? "  BAD" : "");
                char path[96];
                snprintf(path, sizeof path, "/private/var/tmp/tess-%s-%s-%g.ppm", name, mode, factor);
                FILE *ppm = fopen(path, "wb");
                if (ppm) {
                    fprintf(ppm, "P6 %d %d 255\n", Size, Size);
                    for (int i = 0; i < Size * Size; ++i) fwrite(texels + i * 4, 1, 3, ppm);
                    fclose(ppm);
                }
            }
    printf("tess: %d of 12 cases wrong\n", failures);
    // Car Chase's features, one at a time. A quad of two triangle patches over corners 0-3; each corner's colour vector.
    static const float corner[4][2] = {{-0.8f, -0.8f}, {0.8f, -0.8f}, {0.8f, 0.8f}, {-0.8f, 0.8f}};
    static const uint8_t colour[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};
    static const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    struct { const char *name; bool odd, layout36, indexed, perPatch; } cases[] = {
        {"fractional-odd", true, false, false, false},
        {"car-chase-vertex-layout", false, true, false, false},
        {"layout+index-buffer", false, true, true, false},
        {"layout+per-patch-factors", false, true, false, true},
        {"car-chase-all", true, true, true, true},
    };
    int carFailures = 0;
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [library newFunctionWithName:cases[c].layout36 ? @"cc_vertex" : @"cc4_vertex"];
        pd.fragmentFunction = [library newFunctionWithName:@"color_fragment"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        pd.maxTessellationFactor = 16;
        pd.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
        pd.tessellationPartitionMode = cases[c].odd ? MTLTessellationPartitionModeFractionalOdd : MTLTessellationPartitionModeInteger;
        pd.tessellationFactorStepFunction = cases[c].perPatch ? MTLTessellationFactorStepFunctionPerPatchAndPerInstance
                                                              : MTLTessellationFactorStepFunctionConstant;
        pd.tessellationOutputWindingOrder = MTLWindingCounterClockwise;
        pd.tessellationControlPointIndexType = cases[c].indexed ? MTLTessellationControlPointIndexTypeUInt16 : MTLTessellationControlPointIndexTypeNone;
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        NSUInteger slot = 0;
        if (cases[c].layout36) {
            slot = 7;
            const MTLVertexFormat formats[5] = {MTLVertexFormatFloat3, MTLVertexFormatUChar3Normalized, MTLVertexFormatUChar3Normalized,
                                                MTLVertexFormatFloat2, MTLVertexFormatFloat2};
            const NSUInteger offsets[5] = {0, 12, 16, 20, 28};
            for (int a = 0; a < 5; ++a) { vd.attributes[a].format = formats[a]; vd.attributes[a].offset = offsets[a]; vd.attributes[a].bufferIndex = slot; }
            vd.layouts[slot].stride = 36;
        } else {
            vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
            vd.layouts[0].stride = 16;
        }
        vd.layouts[slot].stepFunction = MTLVertexStepFunctionPerPatchControlPoint;
        pd.vertexDescriptor = vd;
        id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!state) { printf("tess %s: no pipeline: %s\n", cases[c].name, error.description.UTF8String); ++carFailures; continue; }
        // Control points: without an index buffer, each patch's three in order (0 1 2, 0 2 3).
        uint8_t points[6][36];
        const int count = cases[c].indexed ? 4 : 6;
        for (int i = 0; i < count; ++i) {
            const int k = cases[c].indexed ? i : indices[i];
            memset(points[i], 0, 36);
            if (cases[c].layout36) {
                const float position[3] = {corner[k][0], corner[k][1], 0.5f}, uv[4] = {0.25f, 0.75f, 0.5f, 0.5f};
                memcpy(points[i], position, 12); memcpy(points[i] + 12, colour[k], 3); points[i][16] = 128; points[i][17] = 128; points[i][18] = 255;
                memcpy(points[i] + 20, uv, 16);
            } else {
                const float position[4] = {corner[k][0], corner[k][1], 0.5f, 1};
                memcpy(points[i], position, 16);
            }
        }
        const NSUInteger stride = cases[c].layout36 ? 36 : 16;
        uint8_t packed[6 * 36];
        for (int i = 0; i < count; ++i) memcpy(packed + i * stride, points[i], stride);
        id<MTLBuffer> controlPoints = [device newBufferWithBytes:packed length:count * stride options:MTLResourceStorageModeShared];
        id<MTLBuffer> indexBuffer = [device newBufferWithBytes:indices length:sizeof indices options:MTLResourceStorageModeShared];
        __fp16 factors[2][4];
        for (int patch = 0; patch < 2; ++patch) for (int i = 0; i < 4; ++i) factors[patch][i] = (__fp16)(patch ? 5.0f : 3.0f);
        id<MTLBuffer> factorBuffer = [device newBufferWithBytes:factors length:sizeof factors options:MTLResourceStorageModeShared];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:Size height:Size mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = [device newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:state];
        [enc setVertexBuffer:controlPoints offset:0 atIndex:slot];
        [enc setTessellationFactorBuffer:factorBuffer offset:0 instanceStride:cases[c].perPatch ? sizeof factors : 0];
        if (cases[c].indexed)
            [enc drawIndexedPatches:3 patchStart:0 patchCount:2 patchIndexBuffer:nil patchIndexBufferOffset:0 controlPointIndexBuffer:indexBuffer
                controlPointIndexBufferOffset:0 instanceCount:1 baseInstance:0];
        else
            [enc drawPatches:3 patchStart:0 patchCount:2 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.error) { printf("tess %s: command buffer: %s\n", cases[c].name, cb.error.description.UTF8String); ++carFailures; continue; }
        static uint8_t texels[Size * Size * 4];
        [target getBytes:texels bytesPerRow:Size * 4 fromRegion:MTLRegionMake2D(0, 0, Size, Size) mipmapLevel:0];
        int stray = 0, holes = 0, wrong = 0, lit = 0;
        for (int y = 0; y < Size; ++y)
            for (int x = 0; x < Size; ++x) {
                const uint8_t *t = texels + (y * Size + x) * 4;
                const bool on = t[0] | t[1] | t[2];
                lit += on;
                bool sure = true;
                const bool inside = tessInside(corner, 4, x, y, Size);
                for (int dy = -1; dy <= 1 && sure; ++dy)
                    for (int dx = -1; dx <= 1 && sure; ++dx)
                        if (tessInside(corner, 4, x + dx, y + dy, Size) != inside) sure = false;
                if (!sure) continue;
                if (on && !inside) { ++stray; continue; }
                if (!on && inside) { ++holes; continue; }
                if (!inside || !cases[c].layout36) continue;
                // The texel's expected colour: the barycentric blend of its triangle's corner colours.
                const float px = (x + 0.5f) * 2 / Size - 1, py = 1 - (y + 0.5f) * 2 / Size;
                for (int tri = 0; tri < 2; ++tri) {
                    const float *a = corner[indices[tri * 3]], *b = corner[indices[tri * 3 + 1]], *d = corner[indices[tri * 3 + 2]];
                    const float area = (b[0] - a[0]) * (d[1] - a[1]) - (b[1] - a[1]) * (d[0] - a[0]);
                    const float wb = ((px - a[0]) * (d[1] - a[1]) - (py - a[1]) * (d[0] - a[0])) / area;
                    const float wd = ((b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])) / area;
                    const float wa = 1 - wb - wd;
                    if (wa < -0.01f || wb < -0.01f || wd < -0.01f) continue;
                    bool off = false;
                    for (int ch = 0; ch < 3; ++ch) {
                        const float want = (colour[indices[tri * 3]][ch] * wa + colour[indices[tri * 3 + 1]][ch] * wb +
                                            colour[indices[tri * 3 + 2]][ch] * wd);
                        if (fabsf(want - t[ch]) > 24) off = true;
                    }
                    wrong += off;
                    break;
                }
            }
        const bool bad = stray || holes || wrong;
        carFailures += bad;
        printf("tess %-25s: %5d texels lit, %4d stray, %4d holes, %4d wrong colours%s\n", cases[c].name, lit, stray, holes, wrong, bad ? "  BAD" : "");
        char path[96];
        snprintf(path, sizeof path, "/private/var/tmp/tess-%s.ppm", cases[c].name);
        FILE *ppm = fopen(path, "wb");
        if (ppm) {
            fprintf(ppm, "P6 %d %d 255\n", Size, Size);
            for (int i = 0; i < Size * Size; ++i) fwrite(texels + i * 4, 1, 3, ppm);
            fclose(ppm);
        }
    }
    printf("tess: %d of %zu Car Chase cases wrong\n", carFailures, sizeof cases / sizeof cases[0]);
    // The Bezier body's form: patch(quad, 0), corners by patch_id. Two patches, the left and right halves of the target.
    {
        id<MTLFunction> bezier = [library newFunctionWithName:@"bezier_vertex"];
        printf("tess bezier: function patchType %ld, patchControlPointCount %ld (Metal: quad = 2, 0)\n", (long)bezier.patchType,
               (long)bezier.patchControlPointCount);
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = bezier;
        pd.fragmentFunction = [library newFunctionWithName:@"color_fragment"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        pd.maxTessellationFactor = 16;
        pd.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
        pd.tessellationPartitionMode = MTLTessellationPartitionModeFractionalOdd;
        pd.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
        pd.tessellationOutputWindingOrder = MTLWindingCounterClockwise;
        id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!state) { printf("tess bezier: no pipeline: %s\n", error.description.UTF8String); return 2; }
        static const float quads[2][4][2] = {{{-0.8f, -0.8f}, {0.0f, -0.8f}, {0.0f, 0.8f}, {-0.8f, 0.8f}},
                                             {{0.0f, -0.8f}, {0.8f, -0.8f}, {0.8f, 0.8f}, {0.0f, 0.8f}}};
        float corners4[8][4];
        for (int q = 0; q < 2; ++q) for (int i = 0; i < 4; ++i) {
            corners4[q * 4 + i][0] = quads[q][i][0]; corners4[q * 4 + i][1] = quads[q][i][1]; corners4[q * 4 + i][2] = 0.5f; corners4[q * 4 + i][3] = 1;
        }
        id<MTLBuffer> cornerBuffer = [device newBufferWithBytes:corners4 length:sizeof corners4 options:MTLResourceStorageModeShared];
        __fp16 quadFactors[2][6];
        for (int q = 0; q < 2; ++q) for (int i = 0; i < 6; ++i) quadFactors[q][i] = (__fp16)5.0f;
        id<MTLBuffer> factorBuffer = [device newBufferWithBytes:quadFactors length:sizeof quadFactors options:MTLResourceStorageModeShared];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:Size height:Size mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = [device newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:state];
        [enc setVertexBuffer:cornerBuffer offset:0 atIndex:1];
        [enc setTessellationFactorBuffer:factorBuffer offset:0 instanceStride:0];
        [enc drawPatches:0 patchStart:0 patchCount:2 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.error) { printf("tess bezier: command buffer: %s\n", cb.error.description.UTF8String); return 2; }
        static uint8_t texels[Size * Size * 4];
        [target getBytes:texels bytesPerRow:Size * 4 fromRegion:MTLRegionMake2D(0, 0, Size, Size) mipmapLevel:0];
        static const float outline[4][2] = {{-0.8f, -0.8f}, {0.8f, -0.8f}, {0.8f, 0.8f}, {-0.8f, 0.8f}};
        int stray = 0, holes = 0, wrong = 0, lit = 0;
        for (int y = 0; y < Size; ++y)
            for (int x = 0; x < Size; ++x) {
                const uint8_t *t = texels + (y * Size + x) * 4;
                const bool on = t[0] | t[1] | t[2];
                lit += on;
                bool sure = true;
                const bool inside = tessInside(outline, 4, x, y, Size);
                for (int dy = -1; dy <= 1 && sure; ++dy)
                    for (int dx = -1; dx <= 1 && sure; ++dx)
                        if (tessInside(outline, 4, x + dx, y + dy, Size) != inside) sure = false;
                if (!sure) continue;
                if (on && !inside) { ++stray; continue; }
                if (!on && inside) { ++holes; continue; }
                if (!inside) continue;
                // Expected: u across the patch (0 at its left edge), v up it, blue for the right patch.
                const float px = (x + 0.5f) * 2 / Size - 1, py = 1 - (y + 0.5f) * 2 / Size;
                if (fabsf(px) < 0.03f) continue;
                const int q = px > 0;
                const float u = (px - quads[q][0][0]) / 0.8f, v = (py + 0.8f) / 1.6f;
                if (fabsf(u * 255 - t[0]) > 24 || fabsf(v * 255 - t[1]) > 24 || abs((q ? 255 : 0) - (int)t[2]) > 24) ++wrong;
            }
        printf("tess bezier quad(0)            : %5d texels lit, %4d stray, %4d holes, %4d wrong colours%s\n", lit, stray, holes, wrong,
               (stray || holes || wrong) ? "  BAD" : "");
        FILE *ppm = fopen("/private/var/tmp/tess-bezier.ppm", "wb");
        if (ppm) {
            fprintf(ppm, "P6 %d %d 255\n", Size, Size);
            for (int i = 0; i < Size * Size; ++i) fwrite(texels + i * 4, 1, 3, ppm);
            fclose(ppm);
        }
        if (stray || holes || wrong) ++carFailures;
    }
    return (failures || carFailures) ? 2 : 0;
}

static volatile int regStop;
static volatile uint64_t regRounds, regBad, regCpuMax;
static void *registerWorker(void *argument) {
    const uint64_t id = (uint64_t)(uintptr_t)argument;
    const pthread_t self = pthread_self();
    uint64_t tsd, generation = 0;
    __asm__ volatile("mrs %0, TPIDRRO_EL0" : "=r"(tsd));
    uint8_t in[512] __attribute__((aligned(16))), out[512] __attribute__((aligned(16)));
    while (!regStop) {
        ++generation;
        for (unsigned i = 0; i < 64; ++i) ((uint64_t *)in)[i] = (id << 56) ^ (generation << 16) ^ (i * 0x0101010101010101ull) ^ 0xa5;
        vectorRound(in, out, 200000);
        for (unsigned r = 0; r < 32; ++r) {
            if (!memcmp(in + r * 16, out + r * 16, 16)) continue;
            if (__atomic_add_fetch(&regBad, 1, __ATOMIC_RELAXED) <= 100)
                printf("BAD thread %llu round %llu v%u: got %016llx%016llx want %016llx%016llx\n", id, generation, r,
                       ((uint64_t *)out)[r * 2 + 1], ((uint64_t *)out)[r * 2], ((uint64_t *)in)[r * 2 + 1], ((uint64_t *)in)[r * 2]);
        }
        uint64_t nowTsd, cpu;
        __asm__ volatile("mrs %0, TPIDRRO_EL0" : "=r"(nowTsd));
        __asm__ volatile("mrs %0, TPIDR_EL0" : "=r"(cpu));
        if (nowTsd != tsd || pthread_self() != self) {
            if (__atomic_add_fetch(&regBad, 1, __ATOMIC_RELAXED) <= 100)
                printf("BAD thread %llu round %llu: TSD base %#llx (was %#llx), pthread_self %p (was %p)\n", id, generation, nowTsd, tsd,
                       (void *)pthread_self(), (void *)self);
        }
        uint64_t number = cpu & 0xff, seen = regCpuMax;
        while (number > seen && !__atomic_compare_exchange_n(&regCpuMax, &seen, number, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        __atomic_add_fetch(&regRounds, 1, __ATOMIC_RELAXED);
        usleep(1000);   // the vCPUs idle now and then: vmguard stops crosvm when every secondary spins for a minute
    }
    return NULL;
}
static int registerCheck(unsigned threads, unsigned long seconds) {
    if (!threads) threads = 12;
    pthread_t workers[64];
    if (threads > 64) threads = 64;
    for (unsigned i = 0; i < threads; ++i) pthread_create(&workers[i], NULL, registerWorker, (void *)(uintptr_t)(i + 1));
    printf("regcheck: %u threads for %lu s\n", threads, seconds);
    fflush(stdout);
    for (unsigned long t = 0; t < seconds; t += 30) {
        sleep(seconds - t < 30 ? (unsigned)(seconds - t) : 30);
        printf("regcheck: %llu rounds, %llu bad, largest CPU number in TPIDR_EL0 %llu\n", regRounds, regBad, regCpuMax);
        fflush(stdout);
    }
    regStop = 1;
    for (unsigned i = 0; i < threads; ++i) pthread_join(workers[i], NULL);
    return regBad ? 2 : 0;
}

// atomcheck: one round's shared counters, on a page of their own.
struct AtomPage { _Atomic uint64_t added, swapped; uint64_t locked; os_unfair_lock lock; };
static struct AtomPage *volatile atomPage;
static volatile int atomStop;
static volatile uint64_t atomGeneration, atomDone;
enum { AtomAdds = 20000 };
static void *atomWorker(void *argument) {
    (void)argument;
    uint64_t seen = 0;
    while (!atomStop) {
        const uint64_t generation = atomGeneration;
        if (generation == seen) { usleep(200); continue; }
        seen = generation;
        struct AtomPage *page = atomPage;
        for (unsigned i = 0; i < AtomAdds; ++i) {
            atomic_fetch_add_explicit(&page->added, 1, memory_order_relaxed);
            uint64_t old = atomic_load_explicit(&page->swapped, memory_order_relaxed);
            while (!atomic_compare_exchange_weak_explicit(&page->swapped, &old, old + 1, memory_order_acq_rel, memory_order_relaxed)) {}
            os_unfair_lock_lock(&page->lock);
            page->locked = page->locked + 1;
            os_unfair_lock_unlock(&page->lock);
        }
        __atomic_add_fetch(&atomDone, 1, __ATOMIC_ACQ_REL);
    }
    return NULL;
}
static int atomicCheck(unsigned threads, unsigned long seconds) {
    if (!threads) threads = 8;
    if (threads > 64) threads = 64;
    pthread_t workers[64];
    for (unsigned i = 0; i < threads; ++i) pthread_create(&workers[i], NULL, atomWorker, NULL);
    const time_t begin = time(NULL);
    time_t reported = begin;
    uint64_t rounds = 0, bad = 0;
    printf("atomcheck: %u threads for %lu s\n", threads, seconds);
    fflush(stdout);
    while ((unsigned long)(time(NULL) - begin) < seconds) {
        // A fresh page each round (mapped anew: its physical page changes), the counters at a varying offset in it.
        uint8_t *memory = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (memory == MAP_FAILED) break;
        struct AtomPage *page = (struct AtomPage *)(memory + (rounds % 64) * 64);
        *page = (struct AtomPage){0, 0, 0, OS_UNFAIR_LOCK_INIT};
        atomDone = 0;
        atomPage = page;
        __atomic_add_fetch(&atomGeneration, 1, __ATOMIC_RELEASE);
        while (__atomic_load_n(&atomDone, __ATOMIC_ACQUIRE) < threads) usleep(500);
        const uint64_t want = (uint64_t)threads * AtomAdds;
        const uint64_t added = atomic_load(&page->added), swapped = atomic_load(&page->swapped), locked = page->locked;
        if (added != want || swapped != want || locked != want) {
            if (++bad <= 100)
                printf("BAD round %llu page %p: atomic add %llu, compare-and-swap %llu, locked add %llu, want %llu\n",
                       rounds, (void *)page, added, swapped, locked, want);
        }
        munmap(memory, 16384);
        ++rounds;
        usleep(2000);
        if (time(NULL) - reported >= 30) {
            reported = time(NULL);
            printf("atomcheck: %llu rounds, %llu bad\n", rounds, bad);
            fflush(stdout);
        }
    }
    atomStop = 1;
    for (unsigned i = 0; i < threads; ++i) pthread_join(workers[i], NULL);
    printf("atomcheck: done, %llu rounds, %llu bad\n", rounds, bad);
    return bad ? 2 : 0;
}

// filecheck: FNV-1a over 64-bit words, enough to tell two reads of the same bytes apart from different ones.
static uint64_t chunkHash(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h = (h ^ w) * 0x100000001b3ull; }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
struct FileChunks { char *path; uint64_t *hashes; size_t count; };
static int fileCheck(const char *dir, unsigned long seconds) {
    enum { Chunk = 1 << 20 };
    uint8_t *buffer = malloc(Chunk);
    if (!buffer) return 1;
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    NSDirectoryEnumerator *walk = [[NSFileManager defaultManager] enumeratorAtPath:@(dir)];
    for (NSString *relative in walk) {
        if (![walk.fileAttributes[NSFileType] isEqual:NSFileTypeRegular]) continue;
        if ([walk.fileAttributes[NSFileSize] unsignedLongLongValue] < 4096) continue;
        [paths addObject:[@(dir) stringByAppendingPathComponent:relative]];
    }
    const size_t files = paths.count;
    struct FileChunks *table = calloc(files, sizeof *table);
    uint64_t totalBytes = 0, passes = 0, bad = 0;
    const time_t begin = time(NULL);
    printf("filecheck: %zu files under %s for %lu s\n", files, dir, seconds);
    fflush(stdout);
    do {
        for (size_t f = 0; f < files; ++f) {
            const char *path = paths[f].fileSystemRepresentation;
            int fd = open(path, O_RDONLY);
            if (fd < 0) continue;
            fcntl(fd, F_NOCACHE, 1);
            size_t chunk = 0;
            for (;;) {
                ssize_t got = 0, n;
                while (got < Chunk && (n = read(fd, buffer + got, Chunk - got)) > 0) got += n;
                if (got <= 0) break;
                const uint64_t h = chunkHash(buffer, (size_t)got);
                if (!passes) {
                    table[f].hashes = realloc(table[f].hashes, (chunk + 1) * sizeof(uint64_t));
                    table[f].hashes[chunk] = h;
                    table[f].count = chunk + 1;
                    totalBytes += (uint64_t)got;
                } else if (chunk < table[f].count && table[f].hashes[chunk] != h) {
                    if (++bad <= 200)
                        printf("BAD pass %llu %s offset %#zx (%zd bytes): %016llx, first pass %016llx\n", passes, path,
                               chunk * (size_t)Chunk, got, h, table[f].hashes[chunk]);
                }
                ++chunk;
                if (got < Chunk) break;
            }
            close(fd);
        }
        ++passes;
        printf("filecheck: pass %llu done after %ld s, %llu MiB, %llu bad chunks\n", passes, (long)(time(NULL) - begin),
               totalBytes >> 20, bad);
        fflush(stdout);
    } while ((unsigned long)(time(NULL) - begin) < seconds);
    return bad ? 2 : 0;
}

static int virtgpuProbe(void) {
    io_object_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAcceleratorES"));
    kern_return_t kr = service ? IOServiceOpen(service, mach_task_self(), DROIDVM_VIRTGPU_OPEN_TYPE, &vgConnection) : KERN_FAILURE;
    if (service) IOObjectRelease(service);
    printf("open IOAcceleratorES type 0x%x: 0x%x\n", DROIDVM_VIRTGPU_OPEN_TYPE, kr);
    if (kr) return 1;
    const uint32_t probe[2] = {0x4e544e4c, 0xffffffffu};
    for (vgSelector = 0; vgSelector < 256; vgSelector++)
        if (IOConnectCallStructMethod(vgConnection, vgSelector, probe, sizeof probe, NULL, NULL) == KERN_SUCCESS) break;
    printf("tunnel selector %u\n", vgSelector);
    const uint64_t params[] = {DROIDVM_VIRTGPU_PARAM_ABI_VERSION, DROIDVM_VIRTGPU_PARAM_SUPPORTED_CAPSET_IDS, DROIDVM_VIRTGPU_PARAM_CONTEXT_INIT,
                               DROIDVM_VIRTGPU_PARAM_HOST_VISIBLE, DROIDVM_VIRTGPU_PARAM_CREATE_GUEST_HANDLE};
    for (unsigned i = 0; i < sizeof params / sizeof *params; i++) {
        struct droidvm_virtgpu_getparam in = {params[i]}; struct droidvm_virtgpu_getparam_reply out = {0};
        kr = vgCall(DROIDVM_VIRTGPU_GETPARAM, &in, sizeof in, &out, sizeof out);
        printf("GETPARAM %#llx: 0x%x value %#llx\n", params[i], kr, out.value);
    }
    uint8_t caps[160] = {0};
    struct droidvm_virtgpu_get_caps getCaps = {6, 0};
    kr = vgCall(DROIDVM_VIRTGPU_GET_CAPS, &getCaps, sizeof getCaps, caps, sizeof caps);
    uint32_t gpuId = 0, contextType = 0; uint64_t vaStart = 0, vaSize = 0;
    memcpy(&contextType, caps + 16, 4); memcpy(&vaStart, caps + 32, 8); memcpy(&vaSize, caps + 40, 8); memcpy(&gpuId, caps + 48, 4);
    printf("GET_CAPS(DRM): 0x%x context type %u gpu %u va %#llx+%#llx\n", kr, contextType, gpuId, vaStart, vaSize);
    struct droidvm_virtgpu_context_init init = {6, 64}; struct droidvm_virtgpu_context_init_reply context = {0};
    kr = vgCall(DROIDVM_VIRTGPU_CONTEXT_INIT, &init, sizeof init, &context, sizeof context);
    printf("CONTEXT_INIT: 0x%x context %u\n", kr, context.context_id);
    if (kr) return 1;
    struct droidvm_virtgpu_create_blob shmemBlob = {DROIDVM_VIRTGPU_BLOB_MEM_HOST3D, DROIDVM_VIRTGPU_BLOB_FLAG_MAPPABLE, 16384, 0, 0, 0};
    struct droidvm_virtgpu_create_blob_reply shmem = {0};
    kr = vgCall(DROIDVM_VIRTGPU_CREATE_BLOB, &shmemBlob, sizeof shmemBlob, &shmem, sizeof shmem);
    struct droidvm_virtgpu_handle shmemHandle = {shmem.handle, 0}; struct droidvm_virtgpu_map_reply shmemMap = {0};
    if (!kr) kr = vgCall(DROIDVM_VIRTGPU_MAP, &shmemHandle, sizeof shmemHandle, &shmemMap, sizeof shmemMap);
    printf("shmem: 0x%x handle %u resource %u mapped at %#llx (%llu bytes)\n", kr, shmem.handle, shmem.resource_id, shmemMap.address, shmemMap.size);
    if (kr) return 1;
    volatile uint8_t *shmemBytes = (volatile uint8_t *)shmemMap.address;
    uint32_t seqno = 0;
    const CFAbsoluteTime began = CFAbsoluteTimeGetCurrent();
    const uint64_t ccmdGpuId = vgGetParam(shmemBytes, &seqno, 0x01, &kr);
    printf("GET_PARAM(GPU_ID) ccmd: 0x%x value %llu (%.2f ms)\n", kr, ccmdGpuId, (CFAbsoluteTimeGetCurrent() - began) * 1000);
    const uint64_t contextVa = vgGetParam(shmemBytes, &seqno, 0x0e /* VA_START */, &kr);
    printf("GET_PARAM(VA_START) ccmd: 0x%x value %#llx\n", kr, contextVa);
    // A guest-alloc BO: GEM_NEW names its iova and blob id, the blob brings the pages.
    struct { struct droidvm_virtgpu_create_blob b; uint32_t header[4]; uint64_t iova, size; uint32_t flags, blobId; } bo = {
        {DROIDVM_VIRTGPU_BLOB_MEM_HOST3D_GUEST, DROIDVM_VIRTGPU_BLOB_FLAG_MAPPABLE | DROIDVM_VIRTGPU_BLOB_FLAG_CREATE_GUEST_HANDLE, 65536, 1, 40, 0},
        {3 /* GEM_NEW */, 40, ++seqno, 0}, contextVa + 0x100000, 65536, 0x80000000u /* GUEST_ALLOC */ | 0x00020000u /* WC */, 1};
    struct droidvm_virtgpu_create_blob_reply boReply = {0};
    kr = vgCall(DROIDVM_VIRTGPU_CREATE_BLOB, &bo, sizeof bo, &boReply, sizeof boReply);
    struct droidvm_virtgpu_handle boHandle = {boReply.handle, 0}; struct droidvm_virtgpu_map_reply boMap = {0};
    if (!kr) kr = vgCall(DROIDVM_VIRTGPU_MAP, &boHandle, sizeof boHandle, &boMap, sizeof boMap);
    unsigned matched = 0;
    if (!kr) {
        volatile uint32_t *words = (volatile uint32_t *)boMap.address;
        for (unsigned i = 0; i < 16384; i++) words[i] = i * 2654435761u;
        for (unsigned i = 0; i < 16384; i++) matched += words[i] == i * 2654435761u;
    }
    printf("guest BO: 0x%x handle %u resource %u at %#llx, %u/16384 words written and read back\n", kr, boReply.handle,
           boReply.resource_id, boMap.address, matched);
    // A fenced submission: a NOP ccmd on ring 1, then its fence waited for.
    struct { struct droidvm_virtgpu_execbuffer e; uint32_t nop[4]; } exec = {{DROIDVM_VIRTGPU_EXEC_FENCE, 1, 16, 0}, {1 /* NOP */, 16, ++seqno, 0}};
    struct droidvm_virtgpu_execbuffer_reply fenced = {0};
    const CFAbsoluteTime submitted = CFAbsoluteTimeGetCurrent();
    kr = vgCall(DROIDVM_VIRTGPU_EXECBUFFER, &exec, sizeof exec, &fenced, sizeof fenced);
    struct droidvm_virtgpu_wait_fence waitFence = {1, 0, fenced.fence, 1000000000ull};
    if (!kr) kr = vgCall(DROIDVM_VIRTGPU_WAIT_FENCE, &waitFence, sizeof waitFence, NULL, 0);
    printf("fenced NOP: 0x%x fence %llu signalled after %.2f ms\n", kr, fenced.fence, (CFAbsoluteTimeGetCurrent() - submitted) * 1000);
    kern_return_t closed = vgCall(DROIDVM_VIRTGPU_GEM_CLOSE, &boHandle, sizeof boHandle, NULL, 0);
    closed |= vgCall(DROIDVM_VIRTGPU_GEM_CLOSE, &shmemHandle, sizeof shmemHandle, NULL, 0);
    printf("GEM_CLOSE: 0x%x; close: 0x%x\n", closed, IOServiceClose(vgConnection));
    return ccmdGpuId == gpuId && matched == 16384 && !kr ? 0 : 1;
}

// MTLCompiler.framework (private): the service behind Metal's newLibraryWithSource:. Request 13 compiles Metal source into a metallib
// (AIR, no GPU code): two little-endian u64 lengths, then the source (NUL-padded to a multiple of 4 bytes) and the compiler options
// (NUL-terminated). The reply, to a block called before the request returns, is a header whose u32s at 8 and 12 are its own size and
// that of the warnings after it, then the metallib (as tinygrad's ops_metal.py drives it on macOS).
typedef void (^CodeGenReply)(int32_t status, const void *reply, size_t length, const char *message);
// The largest channel difference of two RGBA8 renders, the rows where they differ (by more than 8), and the pixels at the left,
// middle and right of the middle row of each.
static unsigned compareRenders(const char *what, const uint8_t *gpu, const uint8_t *cpu, unsigned size) {
    unsigned worst = 0, lit = 0, rows = 0, first = size, last = 0;
    for (unsigned y = 0; y < size; y++) {
        unsigned row = 0;
        for (size_t i = (size_t)y * size * 4; i < (size_t)(y + 1) * size * 4; i++) {
            unsigned d = gpu[i] > cpu[i] ? gpu[i] - cpu[i] : cpu[i] - gpu[i];
            if (d > row) row = d;
            lit |= gpu[i];
        }
        if (row > worst) worst = row;
        if (row > 8) {
            rows++;
            if (y < first) first = y;
            last = y;
        }
    }
    printf("%s: largest difference %u%s", what, worst, lit ? "" : " (the Metal render is all zero)");
    if (rows) printf("; %u rows differ, %u to %u (row 0 is the bottom)", rows, first, last);
    printf("\n");
    for (unsigned x = 0; x < size; x += size / 2 ? size / 2 - 1 : 1) {
        const uint8_t *g = gpu + ((size_t)(size / 2) * size + x) * 4, *c = cpu + ((size_t)(size / 2) * size + x) * 4;
        printf("  x=%-3u metal %3u %3u %3u %3u   software %3u %3u %3u %3u\n", x, g[0], g[1], g[2], g[3], c[0], c[1], c[2], c[3]);
    }
    return worst;
}

static void writeRender(const char *dir, int index, const char *which, const uint8_t *pixels, unsigned size) {
    if (!dir) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/ci-%d-%s.rgba", dir, index, which);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(pixels, 4, (size_t)size * size, f);
    fclose(f);
}

// An icon-like image of side s: an orange rounded rectangle over white, a white checkerboard "glyph" over that.
static CIImage *iconImage(CGFloat s) {
    CGRect r = CGRectMake(0, 0, s, s);
    CIImage *white = [[CIImage imageWithColor:[CIColor colorWithRed:1 green:1 blue:1]] imageByCroppingToRect:r];
    CIImage *rounded = [CIFilter filterWithName:@"CIRoundedRectangleGenerator" withInputParameters:@{
        @"inputExtent": [CIVector vectorWithCGRect:CGRectInset(r, s / 8, s / 8)], @"inputRadius": @(s / 5),
        @"inputColor": [CIColor colorWithRed:1 green:0.5 blue:0]}].outputImage;
    CIImage *checker = [[CIFilter filterWithName:@"CICheckerboardGenerator" withInputParameters:@{
        @"inputCenter": [CIVector vectorWithX:0 Y:0], @"inputColor0": [CIColor colorWithRed:1 green:1 blue:1],
        @"inputColor1": [CIColor colorWithRed:0 green:0 blue:0 alpha:0], @"inputWidth": @(s / 8), @"inputSharpness": @1}].outputImage
        imageByCroppingToRect:CGRectInset(r, s / 4, s / 4)];
    return [checker imageByCompositingOverImage:[rounded imageByCompositingOverImage:white]];
}

static int ciProbe(unsigned size, const char *dir) {
    const char *stitching = getenv("CI_ENABLE_FUNCTION_STITCHING");
    printf("CI_ENABLE_FUNCTION_STITCHING %s\n", stitching ? stitching : "(unset: on)");
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    printf("device %s\n", device ? device.name.UTF8String : "(none)");
    if (!device) return 1;
    NSDictionary *plain = @{kCIContextWorkingColorSpace: [NSNull null], kCIContextOutputColorSpace: [NSNull null]};
    CIContext *gpu = [CIContext contextWithMTLDevice:device options:plain];
    NSMutableDictionary *software = [[plain mutableCopy] autorelease];
    software[kCIContextUseSoftwareRenderer] = @YES;
    CIContext *cpu = [CIContext contextWithOptions:software];
    CGRect r = CGRectMake(0, 0, size, size);
    CIImage *red = [[CIImage imageWithColor:[CIColor colorWithRed:1 green:0 blue:0]] imageByCroppingToRect:r];
    CIImage *blue = [[CIImage imageWithColor:[CIColor colorWithRed:0 green:0 blue:1]] imageByCroppingToRect:r];
    CIImage *mask = [[CIFilter filterWithName:@"CILinearGradient" withInputParameters:@{
        @"inputPoint0": [CIVector vectorWithX:0 Y:0], @"inputPoint1": [CIVector vectorWithX:size Y:0],
        @"inputColor0": [CIColor colorWithRed:0 green:0 blue:0 alpha:0], @"inputColor1": [CIColor colorWithRed:1 green:1 blue:1 alpha:1]}]
        .outputImage imageByCroppingToRect:r];
    CIImage *blend = [CIFilter filterWithName:@"CIBlendWithAlphaMask" withInputParameters:@{
        kCIInputImageKey: blue, kCIInputBackgroundImageKey: red, kCIInputMaskImageKey: mask}].outputImage;
    CIImage *blur = [[CIFilter filterWithName:@"CIGaussianBlur" withInputParameters:@{kCIInputImageKey: blend, kCIInputRadiusKey: @4}]
        .outputImage imageByCroppingToRect:r];
    CIImage *icon = iconImage(size), *icon2x = iconImage(size * 2);
    CIImage *lanczos = [[CIFilter filterWithName:@"CILanczosScaleTransform" withInputParameters:@{
        kCIInputImageKey: icon2x, kCIInputScaleKey: @0.5, kCIInputAspectRatioKey: @1}].outputImage imageByCroppingToRect:r];
    CIImage *affine = [[icon2x imageByApplyingTransform:CGAffineTransformMakeScale(0.5, 0.5)] imageByCroppingToRect:r];
    CIImage *images[] = {blend, blur, icon, lanczos, affine};
    const char *names[] = {"blend with alpha mask", "gaussian blur", "icon (rounded rectangle, glyph)", "icon 2x, lanczos to 1x",
                           "icon 2x, affine to 1x"};
    uint8_t *g = calloc((size_t)size * size, 4), *c = calloc((size_t)size * size, 4);
    unsigned worst = 0;
    for (int i = 0; i < (int)(sizeof images / sizeof images[0]); i++) {
        memset(g, 0, (size_t)size * size * 4);
        memset(c, 0, (size_t)size * size * 4);
        [gpu render:images[i] toBitmap:g rowBytes:size * 4 bounds:r format:kCIFormatRGBA8 colorSpace:nil];
        [cpu render:images[i] toBitmap:c rowBytes:size * 4 bounds:r format:kCIFormatRGBA8 colorSpace:nil];
        writeRender(dir, i, "metal", g, size);
        writeRender(dir, i, "software", c, size);
        unsigned d = compareRenders(names[i], g, c, size);
        if (d > worst) worst = d;
    }
    free(g);
    free(c);
    return worst > 3;
}

// `gfxprobe motion [SECONDS]`: the motion sensors the stock way. First the HID event services on the Apple vendor page, which is
// how locationd finds the accelerometer (usage 3) and the gyroscope (usage 9) (DroidVMMotionHid.inc in the kernel); then what
// CoreMotion -- an app's CMMotionManager, which asks locationd -- reads from them twice a second: acceleration in g, rotation rate
// in radians per second.
static NSString *motionProperty(io_object_t service, CFStringRef key) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
    return value ? [[(id)value autorelease] description] : @"-";
}

static int motionProbe(unsigned seconds) {
    io_object_t iterator = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOHIDEventDriver"), &iterator) == KERN_SUCCESS) {
        for (io_object_t service; (service = IOIteratorNext(iterator)); IOObjectRelease(service)) {
            if (![motionProperty(service, CFSTR("PrimaryUsagePage")) isEqualToString:@"65280"]) continue;
            printf("service: usage %s, product %s, built-in %s, transport %s, accel events %s, gyro events %s\n",
                   motionProperty(service, CFSTR("PrimaryUsage")).UTF8String, motionProperty(service, CFSTR("Product")).UTF8String,
                   motionProperty(service, CFSTR("Built-In")).UTF8String, motionProperty(service, CFSTR("Transport")).UTF8String,
                   motionProperty(service, CFSTR("SupportsAccelEvents")).UTF8String,
                   motionProperty(service, CFSTR("SupportsGyroEvents")).UTF8String);
        }
        IOObjectRelease(iterator);
    }
    CMMotionManager *manager = [[CMMotionManager alloc] init];
    printf("CoreMotion: accelerometer %s, gyroscope %s, device motion %s\n", manager.accelerometerAvailable ? "available" : "absent",
           manager.gyroAvailable ? "available" : "absent", manager.deviceMotionAvailable ? "available" : "absent");
    manager.accelerometerUpdateInterval = 0.02;
    manager.gyroUpdateInterval = 0.02;
    [manager startAccelerometerUpdates];
    [manager startGyroUpdates];
    for (unsigned i = 0; i < seconds * 2; ++i) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
        CMAccelerometerData *accel = manager.accelerometerData;
        CMGyroData *gyro = manager.gyroData;
        printf("%4.1f s  accel %s  gyro %s\n", (i + 1) / 2.0,
               accel ? [NSString stringWithFormat:@"%+.3f %+.3f %+.3f g", accel.acceleration.x, accel.acceleration.y,
                        accel.acceleration.z].UTF8String : "none",
               gyro ? [NSString stringWithFormat:@"%+.3f %+.3f %+.3f rad/s", gyro.rotationRate.x, gyro.rotationRate.y,
                       gyro.rotationRate.z].UTF8String : "none");
    }
    [manager stopAccelerometerUpdates];
    [manager stopGyroUpdates];
    [manager release];
    return 0;
}

// `gfxprobe ioreg CLASS`: every service of CLASS, its properties and its children's classes (the guest has no ioreg).
// mach_memory_info's site flags (xnu osfmk/mach/vm_statistics.h, kernel private): the low byte is the site's type.
enum { SiteType = 0xff, SiteTag = 0, SiteKmod = 1, SiteKernel = 2, SiteCounter = 3, SiteWired = 0x100, SiteZone = 0x800,
       SiteZoneView = 0x1000 };
static int wiredMemory(unsigned top) {
    mach_zone_name_array_t names = NULL; mach_zone_info_array_t zones = NULL; mach_memory_info_array_t sites = NULL;
    mach_msg_type_number_t nameCount = 0, zoneCount = 0, siteCount = 0;
    kern_return_t kr = mach_memory_info(mach_host_self(), &names, &nameCount, &zones, &zoneCount, &sites, &siteCount);
    if (kr != KERN_SUCCESS) { printf("mach_memory_info: %s (%d)\n", mach_error_string(kr), kr); return 1; }
    uint64_t zoneBytes = 0;
    for (mach_msg_type_number_t i = 0; i < zoneCount; ++i) zoneBytes += zones[i].mzi_cur_size;
    // Each site's size; a zone's own rows (SiteZone, SiteZoneView) are in the zones' total. The wired total adds the rows the
    // kernel counts as wired (SiteWired), as zprint does.
    unsigned *order = calloc(siteCount, sizeof *order);
    unsigned rows = 0; uint64_t total = 0;
    for (mach_msg_type_number_t i = 0; i < siteCount; ++i) {
        if (!sites[i].size || (sites[i].flags & (SiteZone | SiteZoneView))) continue;
        if (sites[i].flags & SiteWired) total += sites[i].size;
        order[rows++] = i;
    }
    for (unsigned a = 0; a < rows; ++a)
        for (unsigned b = a + 1; b < rows; ++b)
            if (sites[order[b]].size > sites[order[a]].size) { unsigned t = order[a]; order[a] = order[b]; order[b] = t; }
    printf("sites %u (wired %.1f MiB), zones %u (%.1f MiB)\n", siteCount, total / 1048576.0, zoneCount, zoneBytes / 1048576.0);
    static const char *const kinds[] = {"tag", "kmod", "kernel", "counter"};
    for (unsigned r = 0; r < rows && r < top; ++r) {
        const mach_memory_info_t *m = &sites[order[r]];
        const unsigned type = m->flags & SiteType;
        printf("%9.1f MiB  peak %9.1f  %-7s%s tag %3u  %s%s%#llx\n", m->size / 1048576.0, m->peak / 1048576.0,
               type < 4 ? kinds[type] : "?", (m->flags & SiteWired) ? " wired" : "      ", m->tag,
               m->name[0] ? m->name : "", m->name[0] ? " " : "", (unsigned long long)m->site);
    }
    vm_statistics64_data_t vm = {};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) == KERN_SUCCESS) {
        const double page = vm_kernel_page_size / 1048576.0;
        printf("pages of %lu bytes: wired %.1f MiB, free %.1f, active %.1f, inactive %.1f, compressor %.1f, purgeable %.1f\n",
               (unsigned long)vm_kernel_page_size, vm.wire_count * page, vm.free_count * page, vm.active_count * page,
               vm.inactive_count * page, vm.compressor_page_count * page, vm.purgeable_count * page);
    }
    free(order);
    return 0;
}

static int mobileGestalt(int count, char **keys) {
    static const char *const screen[] = {"main-screen-scale", "main-screen-width", "main-screen-height", "main-screen-class",
        "main-screen-pitch", "DisplayExtendedProperties", "ArtworkTraits", "ArtworkDeviceSubType", "ArtworkDeviceScaleFactor",
        "DeviceClassNumber", "ProductType", "HWModelStr", "IsVirtualDevice"};
    void *library = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW);
    CFTypeRef (*copyAnswer)(CFStringRef, CFDictionaryRef) = library ? dlsym(library, "MGCopyAnswer") : NULL;
    if (!copyAnswer) { printf("no MGCopyAnswer\n"); return 1; }
    if (!count) { count = sizeof screen / sizeof *screen; keys = (char **)screen; }
    for (int i = 0; i < count; ++i) {
        CFStringRef key = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
        CFTypeRef answer = copyAnswer(key, NULL);
        printf("%s = %s\n", keys[i], answer ? [(id)answer description].UTF8String : "(none)");
        if (answer) CFRelease(answer);
        CFRelease(key);
    }
    return 0;
}

static int springBoardScreen(void) {
    static const char *const frameworks[] = {"Frameworks/UIKit.framework/UIKit", "PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices",
        "PrivateFrameworks/SpringBoardFoundation.framework/SpringBoardFoundation",
        "PrivateFrameworks/SpringBoardHome.framework/SpringBoardHome"};
    for (unsigned i = 0; i < sizeof frameworks / sizeof *frameworks; ++i) {
        char path[160];
        snprintf(path, sizeof path, "/System/Library/%s", frameworks[i]);
        if (!dlopen(path, RTLD_NOW)) printf("dlopen %s: %s\n", path, dlerror());
    }
    long (*deviceClass)(void) = dlsym(RTLD_DEFAULT, "SBFEffectiveDeviceClass");
    long (*homeButton)(void) = dlsym(RTLD_DEFAULT, "SBFEffectiveHomeButtonType");
    long (*subtype)(void) = dlsym(RTLD_DEFAULT, "SBFEffectiveArtworkSubtype");
    double (*cornerRadius)(void) = dlsym(RTLD_DEFAULT, "SBFEffectiveDisplayCornerRadius");
    long (*screenType)(void) = dlsym(RTLD_DEFAULT, "SBHScreenTypeForCurrentDevice");
    if (deviceClass) printf("SBFEffectiveDeviceClass = %ld\n", deviceClass());
    if (homeButton) printf("SBFEffectiveHomeButtonType = %ld\n", homeButton());
    if (subtype) printf("SBFEffectiveArtworkSubtype = %ld\n", subtype());
    if (cornerRadius) printf("SBFEffectiveDisplayCornerRadius = %g\n", cornerRadius());
    id screen = [NSClassFromString(@"UIScreen") valueForKey:@"mainScreen"];
    for (NSString *key in @[@"bounds", @"scale", @"nativeBounds", @"nativeScale", @"_referenceBounds", @"displayConfiguration"]) {
        @try {
            printf("UIScreen %s = %s\n", key.UTF8String, [[screen valueForKey:key] description].UTF8String);
        } @catch (NSException *e) {
            printf("UIScreen %s: %s\n", key.UTF8String, e.reason.UTF8String);
        }
    }
    fflush(stdout);
    if (screenType) printf("SBHScreenTypeForCurrentDevice = %ld\n", screenType());
    return 0;
}

static int ioregDump(const char *className) {
    io_object_t iterator = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(className), &iterator) != KERN_SUCCESS) return 1;
    unsigned count = 0;
    for (io_object_t service; (service = IOIteratorNext(iterator)); IOObjectRelease(service), ++count) {
        CFMutableDictionaryRef properties = NULL;
        IORegistryEntryCreateCFProperties(service, &properties, kCFAllocatorDefault, 0);
        printf("== %s %u\n%s\n", className, count, properties ? [(id)properties description].UTF8String : "(no properties)");
        if (properties) CFRelease(properties);
        io_object_t children = 0;
        if (IORegistryEntryGetChildIterator(service, "IOService", &children) == KERN_SUCCESS) {
            for (io_object_t child; (child = IOIteratorNext(children)); IOObjectRelease(child)) {
                char name[128] = "";
                IOObjectGetClass(child, name);
                printf("   child %s\n", name);
            }
            IOObjectRelease(children);
        }
    }
    IOObjectRelease(iterator);
    printf("%u %s\n", count, className);
    return 0;
}

static int setOrientation(long orientation) {
    void *bbs = dlopen("/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices", RTLD_NOW);
    void (*set)(long) = bbs ? (void (*)(long))dlsym(bbs, "BKSHIDServicesSetDeviceOrientationForAutomation") : NULL;
    if (!set) {
        fprintf(stderr, "no BKSHIDServicesSetDeviceOrientationForAutomation\n");
        return 1;
    }
    set(orientation);
    printf("device orientation %ld sent\n", orientation);
    // backboardd reads the caller's entitlements after the message arrives: a process gone by then is refused.
    sleep(2);
    return 0;
}

// kern.droidvm_motion (DroidVMMotion.c): the motion sensors' latest frame and the UIDeviceOrientation it says, 0 for none yet.
struct DroidVMMotionSample { uint32_t seq; int32_t orientation, accel[3], gyro[3]; };

// Whether SpringBoard has blanked the screen (its com.apple.springboard.hasBlankedScreen state is 1 while the display is off),
// updated when the state changes. Without the notification the screen counts as on.
static bool screenBlanked(int token, bool blanked) {
    int changed = 0;
    if (token < 0 || notify_check(token, &changed) != NOTIFY_STATUS_OK || !changed) return blanked;
    uint64_t state = 0;
    if (notify_get_state(token, &state) != NOTIFY_STATUS_OK) return blanked;
    if ((state == 1) != blanked) {
        printf("screen %s\n", state == 1 ? "blanked" : "on");
        fflush(stdout);
    }
    return state == 1;
}

static int followOrientation(void) {
    void *bbs = dlopen("/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices", RTLD_NOW);
    void (*set)(long) = bbs ? (void (*)(long))dlsym(bbs, "BKSHIDServicesSetDeviceOrientationForAutomation") : NULL;
    if (!set) {
        fprintf(stderr, "no BKSHIDServicesSetDeviceOrientationForAutomation\n");
        return 1;
    }
    // An orientation sent while the screen is off reaches backboardd, but the lock screen it wakes to keeps the one before
    // (seen 2026-10-27: sent 4 then 1 asleep, woke at 3). So nothing is sent then: the screen wakes in the last orientation
    // sent, and the one the sensors say now is a real change, sent once it is on.
    int blankToken = -1;
    if (notify_register_check("com.apple.springboard.hasBlankedScreen", &blankToken) != NOTIFY_STATUS_OK) blankToken = -1;
    bool blanked = false;
    int32_t sent = 0;
    bool missing = false;
    for (;;) {
        blanked = screenBlanked(blankToken, blanked);
        struct DroidVMMotionSample sample;
        size_t size = sizeof(sample);
        if (sysctlbyname("kern.droidvm_motion", &sample, &size, NULL, 0) != 0 || size != sizeof(sample)) {
            // No motion-sensor device (yet): the VM has none, or the driver has not found it. Keep looking, quietly.
            if (!missing) fprintf(stderr, "kern.droidvm_motion: %s; waiting\n", strerror(errno));
            missing = true;
            sleep(5);
            continue;
        }
        missing = false;
        // backboardd follows; it would also take the same value again, but there is no reason to send it.
        if (!blanked && sample.orientation >= 1 && sample.orientation <= 4 && sample.orientation != sent) {
            set(sample.orientation);
            printf("device orientation %d (gravity %d %d %d)\n", sample.orientation, sample.accel[0], sample.accel[1],
                   sample.accel[2]);
            fflush(stdout);
            sent = sample.orientation;
        }
        usleep(100000);
    }
}

// libproc, in libSystem on iOS though its header is not in the SDK (sys/proc_info.h: PROC_PIDTHREADINFO 5, PROC_PIDLISTTHREADS 6).
extern int proc_listallpids(void *buffer, int size);
extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int size);
extern int proc_name(int pid, void *buffer, uint32_t size);
extern int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer);
enum { PROC_PIDTHREADINFO = 5, PROC_PIDLISTTHREADS = 6 };
struct proc_threadinfo {
    uint64_t pth_user_time, pth_system_time;
    int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority;
    char pth_name[64];
};

// One process's CPU time in nanoseconds (user and system), or -1 where it is gone.
static int64_t processTime(pid_t pid) {
    struct rusage_info_v0 info;
    if (proc_pid_rusage(pid, RUSAGE_INFO_V0, (rusage_info_t *)&info) != 0) return -1;
    static mach_timebase_info_data_t base;
    if (!base.denom) mach_timebase_info(&base);
    return (int64_t)((info.ri_user_time + info.ri_system_time) * base.numer / base.denom);
}

// Each thread of `pid` with its CPU time in nanoseconds, keyed by thread id.
static NSDictionary<NSNumber *, NSArray *> *threadTimes(pid_t pid) {
    NSMutableDictionary *threads = [NSMutableDictionary dictionary];
    uint64_t ids[1024];
    int bytes = proc_pidinfo(pid, PROC_PIDLISTTHREADS, 0, ids, sizeof(ids));
    for (int i = 0; bytes > 0 && i < bytes / (int)sizeof(uint64_t); i++) {
        struct proc_threadinfo info;
        if (proc_pidinfo(pid, PROC_PIDTHREADINFO, ids[i], &info, sizeof(info)) != sizeof(info)) continue;
        threads[@(ids[i])] = @[@(info.pth_user_time + info.pth_system_time), @(info.pth_name)];
    }
    return threads;
}

static int cpuUse(double seconds, int threadProcesses) {
    int pids[4096];
    int count = proc_listallpids(pids, sizeof(pids)) ;
    NSMutableDictionary *before = [NSMutableDictionary dictionary];
    for (int i = 0; i < count; i++) {
        int64_t t = processTime(pids[i]);
        if (t >= 0) before[@(pids[i])] = @(t);
    }
    NSMutableDictionary *threadsBefore = [NSMutableDictionary dictionary];
    for (NSNumber *pid in before) threadsBefore[pid] = threadTimes(pid.intValue);
    usleep((useconds_t)(seconds * 1e6));
    NSMutableArray *rows = [NSMutableArray array];
    for (NSNumber *pid in before) {
        int64_t t = processTime(pid.intValue);
        if (t < 0) continue;
        int64_t used = t - [before[pid] longLongValue];
        if (used > 0) [rows addObject:@[@(used), pid]];
    }
    [rows sortUsingComparator:^NSComparisonResult(NSArray *a, NSArray *b) { return [b[0] compare:a[0]]; }];
    double total = 0;
    for (NSArray *row in rows) total += [row[0] doubleValue];
    printf("cpu over %.1f s: %.0f%% of one CPU in all\n", seconds, 100 * total / (seconds * 1e9));
    for (NSUInteger i = 0; i < rows.count && i < 15; i++) {
        pid_t pid = [rows[i][1] intValue];
        char name[256] = "";
        proc_name(pid, name, sizeof(name));
        printf("  %5.1f%%  %5d %s\n", 100 * [rows[i][0] doubleValue] / (seconds * 1e9), pid, name);
    }
    for (NSUInteger i = 0; i < rows.count && (int)i < threadProcesses; i++) {
        NSNumber *pid = rows[i][1];
        NSDictionary *was = threadsBefore[pid], *now = threadTimes(pid.intValue);
        NSMutableArray *threads = [NSMutableArray array];
        for (NSNumber *tid in now) {
            int64_t used = [now[tid][0] longLongValue] - [was[tid][0] longLongValue];
            if (used > 0) [threads addObject:@[@(used), tid, now[tid][1]]];
        }
        [threads sortUsingComparator:^NSComparisonResult(NSArray *a, NSArray *b) { return [b[0] compare:a[0]]; }];
        char name[256] = "";
        proc_name(pid.intValue, name, sizeof(name));
        printf("threads of %d %s:\n", pid.intValue, name);
        for (NSUInteger k = 0; k < threads.count && k < 8; k++)
            printf("  %5.1f%%  %llu %s\n", 100 * [threads[k][0] doubleValue] / (seconds * 1e9),
                   [threads[k][1] unsignedLongLongValue], [threads[k][2] UTF8String]);
    }
    return 0;
}

static void printKeys(NSString *what, id object, NSArray<NSString *> *keys) {
    printf("%s %s\n", what.UTF8String, [[object description] UTF8String]);
    for (NSString *key in keys) {
        @try {
            id value = [object valueForKey:key];
            printf("  %s = %s\n", key.UTF8String, [[value description] UTF8String]);
        } @catch (NSException *e) {
        }
    }
}

static int displayInfo(void) {
    Class display = objc_getClass("CADisplay");
    id main = display ? ((id (*)(id, SEL))objc_msgSend)(display, sel_registerName("mainDisplay")) : nil;
    if (!main) { printf("no CADisplay mainDisplay\n"); return 1; }
    NSArray *keys = @[@"name", @"displayId", @"refreshRate", @"heartbeatRate", @"minimumFrameDuration", @"maximumRefreshRate",
                      @"minimumRefreshRate", @"idealRefreshRate", @"hardwareRefreshRate", @"displayRefreshRate", @"supportsVRR",
                      @"isVRR", @"preferredFramesPerSecond", @"currentMode", @"preferredMode", @"availableModes", @"bounds",
                      @"frame", @"nativeOrientation", @"currentOrientation", @"compatQuantaMode", @"serverCompatQuantaMode"];
    printKeys(@"CADisplay mainDisplay", main, keys);
    id mode = [main valueForKey:@"currentMode"];
    if (mode) printKeys(@"currentMode", mode, @[@"width", @"height", @"refreshRate", @"isVRR", @"pixelFormat", @"hdrMode"]);
    Class link = objc_getClass("CADisplayLink");
    if (link) {
        id dl = ((id (*)(id, SEL, id, id, SEL))objc_msgSend)(link, sel_registerName("displayLinkWithDisplay:target:selector:"), main,
                                                          main, sel_registerName("description"));
        if (dl) printKeys(@"CADisplayLink", dl, @[@"preferredFramesPerSecond", @"maximumRefreshRate", @"duration",
                                                  @"preferredFrameRateRange"]);
    }
    return 0;
}

static int compileMetal(const char *sourcePath, const char *outPath, const char *options) {
    void *library = dlopen("/System/Library/PrivateFrameworks/MTLCompiler.framework/MTLCompiler", RTLD_NOW);
    void *(*create)(const char *) = library ? dlsym(library, "MTLCodeGenServiceCreate") : NULL;
    void (*build)(void *, void *, uint32_t, const void *, size_t, CodeGenReply) =
        library ? dlsym(library, "MTLCodeGenServiceBuildRequest") : NULL;
    if (!create || !build) {
        printf("MTLCompiler: %s\n", dlerror());
        return 1;
    }
    NSData *source = [NSData dataWithContentsOfFile:@(sourcePath)];
    if (!source) {
        printf("%s: cannot read\n", sourcePath);
        return 1;
    }
    const uint64_t sourceBytes = (source.length + 1 + 3) & ~(uint64_t)3, optionBytes = strlen(options) + 1;
    NSMutableData *request = [NSMutableData dataWithLength:16 + sourceBytes + optionBytes];
    uint8_t *bytes = request.mutableBytes;
    memcpy(bytes, &sourceBytes, 8);
    memcpy(bytes + 8, &optionBytes, 8);
    memcpy(bytes + 16, source.bytes, source.length);
    memcpy(bytes + 16 + sourceBytes, options, optionBytes);
    void *service = create("gfxprobe");
    printf("service %p, options: %s\n", service, options);
    __block int result = 1;
    __block BOOL called = NO;
    const CFTimeInterval began = CFAbsoluteTimeGetCurrent();
    build(service, NULL, 13, bytes, request.length, ^(int32_t status, const void *reply, size_t length, const char *message) {
        called = YES;
        printf("status %d, reply %zu bytes, %.1f ms%s%s\n", status, length, (CFAbsoluteTimeGetCurrent() - began) * 1000,
               message && *message ? ", message: " : "", message ? message : "");
        if (status || !reply || length < 16) return;
        uint32_t header, warnings;
        memcpy(&header, (const uint8_t *)reply + 8, 4);
        memcpy(&warnings, (const uint8_t *)reply + 12, 4);
        if ((uint64_t)header + warnings > length) return;
        if (warnings) printf("warnings: %.*s\n", (int)warnings, (const char *)reply + header);
        NSData *metallib = [NSData dataWithBytes:(const uint8_t *)reply + header + warnings length:length - header - warnings];
        printf("metallib %zu bytes, magic %.4s\n", metallib.length, (const char *)metallib.bytes);
        result = [metallib writeToFile:@(outPath) atomically:NO] ? 0 : 1;
    });
    if (!called) printf("MTLCodeGenServiceBuildRequest returned without calling back\n");
    return result;
}

// tsd: the slots as dyld and libSystem's initializers left them, read before main's autorelease pool exists.
static void dumpTsd(void) {
    uint64_t base;
    __asm__ volatile("mrs %0, TPIDRRO_EL0" : "=r"(base));
    const uint64_t *slots = (const uint64_t *)(uintptr_t)(base & ~7ull);
    printf("tsd base %#llx, pthread_self %p\n", base, (void *)pthread_self());
    for (unsigned i = 0; i < 256; ++i) if (slots[i]) printf("  slot %3u = %#018llx\n", i, slots[i]);
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "tsd")) { dumpTsd(); return 0; }
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        if (argc >= 4 && !strcmp(argv[1], "box")) {
            // PROFILE[:UID]: as that user (501, mobile, as WebKit's processes run), then in the profile.
            char profile[256];
            strlcpy(profile, argv[2], sizeof profile);
            char *uid = strchr(profile, ':');
            if (uid) {
                *uid++ = 0;
                if (setgid((gid_t)atoi(uid)) || setuid((uid_t)atoi(uid))) {
                    perror("setuid");
                    return 1;
                }
            }
            // Directories a shader cache might live in, their parents made before the sandbox (as the system would have them).
            static const char *candidates[] = {"/var/mobile/Library/Caches/com.apple.WebKit.GPU", "/var/mobile/Library/Caches/com.apple.metal",
                                               "/var/mobile/Library/Caches/com.apple.WebKit.GPU/com.apple.metal",
                                               "/var/mobile/Library/Caches/com.apple.MobileSafari", "/var/mobile/tmp", "/private/var/tmp",
                                               "/var/mobile/Library/Caches", "/private/var/ngpu/cache"};
            for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
                mkdir(candidates[i], 0777);
                chmod(candidates[i], 0777);
            }
            char *error = NULL;
            int r = sandbox_init(profile, 1 /* SANDBOX_NAMED */, &error);
            printf("sandbox %s as uid %d: %d %s\n", profile, getuid(), r, error ? error : "");
            if (r)
                return 1;
            static const char *files[] = {"/System/Library/Extensions/NativeAdrenoMetal.bundle/NativeAdrenoMetal",
                                          "/System/Library/Extensions/NativeAdrenoMetal.clients",
                                          "/System/Library/Extensions/NativeAdrenoMetal.bundle/libvulkan_freedreno.dylib"};
            for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
                int fd = open(files[i], O_RDONLY);
                printf("  open %s: %s\n", files[i], fd >= 0 ? "ok" : strerror(errno));
                if (fd >= 0) close(fd);
            }
            // The plugin's shader-cache candidates (NativeAdrenoMetalPlugin.mm clientDevice): the user cache and temp directories + m2v.
            int names[2] = {_CS_DARWIN_USER_CACHE_DIR, _CS_DARWIN_USER_TEMP_DIR};
            for (int i = 0; i < 2; i++) {
                char dir[512] = "";
                size_t n = confstr(names[i], dir, sizeof dir - 16);
                if (!n) {
                    printf("  confstr %d: %s\n", names[i], strerror(errno));
                    continue;
                }
                strlcat(dir, "m2v", sizeof dir);
                int made = mkdir(dir, 0700);
                int madeErrno = errno;
                printf("  %s: mkdir %s, writable %s\n", dir, made ? strerror(madeErrno) : "made", access(dir, W_OK) ? strerror(errno) : "yes");
            }
            for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
                char dir[512], probe[600];
                snprintf(dir, sizeof dir, "%s/m2v", candidates[i]);
                int made = mkdir(dir, 0700), madeErrno = errno;
                snprintf(probe, sizeof probe, "%s/.probe", dir);
                int fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0600), fdErrno = errno;
                if (fd >= 0) {
                    close(fd);
                    unlink(probe);
                }
                printf("  %s: mkdir %s, write %s\n", dir, made ? strerror(madeErrno) : "made", fd >= 0 ? "ok" : strerror(fdErrno));
            }
            argv += 2;
            argc -= 2;
            argv[0] = argv[-2];
        }
        if (argc >= 2 && !strcmp(argv[1], "sync")) {
            int failed = 0;
            for (int i = 2; i < argc; i++) {
                int fd = open(argv[i], O_RDONLY);
                int r = fd >= 0 ? fcntl(fd, F_FULLFSYNC) : -1;
                printf("%s: %s\n", argv[i], r ? strerror(errno) : "synced");
                failed |= r != 0;
                if (fd >= 0) close(fd);
            }
            sync();
            return failed;
        }
        if (argc >= 3 && !strcmp(argv[1], "orient"))
            return setOrientation(strtol(argv[2], NULL, 0));
        if (argc >= 2 && !strcmp(argv[1], "orient-follow"))
            return followOrientation();
        if (argc >= 2 && !strcmp(argv[1], "wired"))
            return wiredMemory(argc >= 3 ? (unsigned)strtoul(argv[2], NULL, 0) : 40);
        if (argc >= 2 && !strcmp(argv[1], "sbscreen"))
            return springBoardScreen();
        if (argc >= 2 && !strcmp(argv[1], "mg"))
            return mobileGestalt(argc - 2, argv + 2);
        if (argc >= 3 && !strcmp(argv[1], "ioreg"))
            return ioregDump(argv[2]);
        if (argc >= 2 && !strcmp(argv[1], "motion"))
            return motionProbe(argc >= 3 ? (unsigned)strtoul(argv[2], NULL, 0) : 5);
        if (argc >= 2 && !strcmp(argv[1], "display")) {
            dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW);
            return displayInfo();
        }
        if (argc >= 3 && !strcmp(argv[1], "cpu"))
            return cpuUse(atof(argv[2]), argc >= 4 ? atoi(argv[3]) : 3);
        if (argc >= 2 && !strcmp(argv[1], "tess"))
            return tessCheck();
        if (argc >= 2 && !strcmp(argv[1], "ci"))
            return ciProbe(argc >= 3 ? (unsigned)strtoul(argv[2], NULL, 0) : 64, argc >= 4 ? argv[3] : NULL);
        if (argc >= 2 && !strcmp(argv[1], "virtgpu"))
            return virtgpuProbe();
        if (argc >= 4 && !strcmp(argv[1], "filecheck"))
            return fileCheck(argv[2], strtoul(argv[3], NULL, 0));
        if (argc >= 4 && !strcmp(argv[1], "atomcheck"))
            return atomicCheck((unsigned)strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
        if (argc >= 5 && !strcmp(argv[1], "cowcheck"))
            return cowCheck((unsigned)strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), strtoul(argv[4], NULL, 0));
        if (argc >= 4 && !strcmp(argv[1], "cowchild"))
            return cowChild(strtoul(argv[2], NULL, 0), (unsigned)strtoul(argv[3], NULL, 0));
        if (argc >= 4 && !strcmp(argv[1], "datacheck"))
            return dataCheck(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
        if (argc >= 4 && !strcmp(argv[1], "zerocheck"))
            return zeroCheck(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
        if (argc >= 4 && !strcmp(argv[1], "asidcheck"))
            return asidCheck((unsigned)strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
        if (argc >= 5 && !strcmp(argv[1], "asidchild"))
            return asidChild(strtoul(argv[2], NULL, 0), (unsigned)strtoul(argv[3], NULL, 0), strtoull(argv[4], NULL, 0));
        if (argc >= 4 && !strcmp(argv[1], "textcheck"))
            return textCheck(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
        if (argc >= 6 && !strcmp(argv[1], "reusethreads"))
            return reuseThreaded(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), strtoul(argv[4], NULL, 0),
                                 (unsigned)strtoul(argv[5], NULL, 0));
        if (argc >= 5 && !strcmp(argv[1], "reusecheck"))
            return reuseCheck(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), strtoul(argv[4], NULL, 0));
        if (argc >= 3 && !strcmp(argv[1], "machcheck"))
            return machCheck(strtoul(argv[2], NULL, 0));
        if (argc >= 4 && !strcmp(argv[1], "regfault"))
            return faultCheck((unsigned)strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
        if (argc >= 4 && !strcmp(argv[1], "regcheck"))
            return registerCheck((unsigned)strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
        if (argc >= 5 && !strcmp(argv[1], "memidle"))
            return memoryIdle(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), strtoul(argv[4], NULL, 0));
        if (argc >= 4 && !strcmp(argv[1], "memcheck"))
            return memoryCheck(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
        // user UID VERB...: the verb as that user (501, mobile, as Settings and the apps run), without a sandbox.
        if (argc >= 4 && !strcmp(argv[1], "user")) {
            uid_t uid = (uid_t)atoi(argv[2]);
            if (setgid((gid_t)uid) || setuid(uid)) {
                perror("setuid");
                return 1;
            }
            argv[2] = argv[0];
            return main(argc - 2, argv + 2);
        }
        if (argc >= 2 && !strcmp(argv[1], "clocks"))
            return clocksProbe();
        if (argc >= 3 && !strcmp(argv[1], "scnotify"))
            return scNotify(argc - 2, argv + 2);
        if (argc >= 2 && !strcmp(argv[1], "families"))
            return familiesProbe();
        if (argc >= 2 && !strcmp(argv[1], "argbuf"))
            return argBufferProbe();
        if (argc >= 2 && !strcmp(argv[1], "msaa"))
            return msaaProbe(argc - 2, argv + 2);
        if (argc >= 2 && !strcmp(argv[1], "dontcare"))
            return dontCareProbe(argc - 2, argv + 2);
        if (argc >= 2 && !strcmp(argv[1], "vtdecode"))
            return vtDecodeProbe(argc - 2, argv + 2);
        if (argc >= 2 && !strcmp(argv[1], "vtencode"))
            return vtEncodeProbe(argc - 2, argv + 2);
        if (argc >= 2 && !strcmp(argv[1], "sysctl"))
            return sysctlProbe(argc - 2, argv + 2);
        if (argc >= 2 && !strcmp(argv[1], "ethernet"))
            return ethernetProbe();
        if (argc >= 2 && !strcmp(argv[1], "spuopen"))
            return spuOpen();
        if (argc >= 3 && !strcmp(argv[1], "ktxdecode"))
            return ktxDecode(argv[2]);
        if (argc >= 5 && !strcmp(argv[1], "pitchcheck"))
            return pitchCheck((unsigned)strtoul(argv[2], NULL, 0), (unsigned)strtoul(argv[3], NULL, 0), (unsigned)strtoul(argv[4], NULL, 0));
        if (argc >= 4 && !strcmp(argv[1], "msl"))
            return compileMetal(argv[2], argv[3], argc >= 5 ? argv[4] : "-std=metal3.1 --driver-mode=metal -x metal");
        if (argc < 5) {
            fprintf(stderr, "usage: gfxprobe rb|ca|cg|plain|open W H ARG... | sync PATH...\n");
            return 2;
        }
        dlopen("/System/Library/PrivateFrameworks/RenderBox.framework/RenderBox", RTLD_NOW);
        dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW);
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        printf("device %s\n", device ? device.name.UTF8String : "(none)");
        unsigned w = (unsigned)strtoul(argv[2], NULL, 0), h = (unsigned)strtoul(argv[3], NULL, 0);
        if (!strcmp(argv[1], "rb")) {
            Class drawable = objc_getClass("RBDrawable");
            IMP size = drawable ? class_getMethodImplementation(drawable, sel_registerName("size")) : NULL;
            if (!size) {
                fprintf(stderr, "no RBDrawable\n");
                return 1;
            }
            CreateIOSurface create = (CreateIOSurface)((uintptr_t)size - kRBDrawableSize + kRBCreateIOSurface);
            for (int i = 4; i < argc; i++) {
                char *rest = NULL;
                unsigned long format = strtoul(argv[i], &rest, 0);
                unsigned flags = rest && *rest == ':' ? (unsigned)strtoul(rest + 1, NULL, 16) : 0;
                describe([NSString stringWithFormat:@"rb format %lu%s flags %#x", format & 0x7fffffff,
                          format >> 31 ? "+rb" : "", flags], create(format, w, h, flags), device,
                         (MTLPixelFormat)(format & 0x7fffffff), w, h);
            }
        } else if (!strcmp(argv[1], "ca")) {
            for (int i = 4; i < argc; i++) {
                OSType fcc = parseFourcc(argv[i]);
                describe([NSString stringWithFormat:@"ca %@", fourcc(fcc)], CAIOSurfaceCreateDefault(w, h, fcc), device,
                         MTLPixelFormatBGRA8Unorm, w, h);
            }
        } else if (!strcmp(argv[1], "cg")) {
            for (int i = 4; i < argc; i++) {
                OSType fcc = parseFourcc(argv[i]);
                IOSurfaceRef surface = CAIOSurfaceCreateDefault(w, h, fcc);
                if (!surface) {
                    printf("cg %s: no surface\n", fourcc(fcc).UTF8String);
                    continue;
                }
                // WebKit's IOSurface formats: BGRA 8 bits premultiplied first; RGB10A8 and RGB10 10 bits a channel in 32-bit words.
                bool deep = fcc == 'b3a8' || fcc == 'w30r';
                CGColorSpaceRef space = CGColorSpaceCreateWithName(deep ? kCGColorSpaceExtendedSRGB : kCGColorSpaceSRGB);
                CGBitmapInfo info = deep ? (CGBitmapInfo)((fcc == 'b3a8' ? kCGImageAlphaPremultipliedLast : kCGImageAlphaNoneSkipLast) |
                                                          kCGImagePixelFormatRGB101010 | kCGBitmapByteOrder32Little)
                                         : (CGBitmapInfo)(kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
                size_t before[2], after[2];
                litPlanes(surface, before);
                CGContextRef context = CGIOSurfaceContextCreate(surface, w, h, deep ? 10 : 8, 32, space, info);
                if (context) {
                    CGContextSetRGBFillColor(context, 1, 0, 0, 1);
                    CGContextFillRect(context, CGRectMake(0, 0, w, h));
                    CGContextFlush(context);
                }
                // A context that draws through the GPU (it has a Metal device) lands its bytes when the GPU is done: sampled at once,
                // after 0.1 s and after 1 s.
                size_t later[2], last[2];
                litPlanes(surface, after);
                usleep(100000);
                litPlanes(surface, later);
                usleep(900000);
                litPlanes(surface, last);
                printf("cg %s: planes=%zu context %s; lit before %zu/%zu, after %zu/%zu, 0.1 s %zu/%zu, 1 s %zu/%zu\n",
                       fourcc(fcc).UTF8String, IOSurfaceGetPlaneCount(surface), context ? "made" : "REFUSED", before[0], before[1],
                       after[0], after[1], later[0], later[1], last[0], last[1]);
                if (context) CGContextRelease(context);
                CGColorSpaceRelease(space);
                CFRelease(surface);
            }
        } else if (!strcmp(argv[1], "coherence")) {
            unsigned rounds = (unsigned)strtoul(argv[4], NULL, 0);
            // An optional fifth argument: the surface's cache mode (kIOSurfaceCacheMode: 0x400 write-combine, 0x100 inhibit).
            unsigned mode = argc > 5 ? (unsigned)strtoul(argv[5], NULL, 0) : 0;
            NSMutableDictionary *properties = [@{(id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h), (id)kIOSurfaceBytesPerElement: @4,
                                                 (id)kIOSurfacePixelFormat: @((unsigned)'BGRA')} mutableCopy];
            if (mode)
                properties[(id)kIOSurfaceCacheMode] = @(mode);
            IOSurfaceRef surface = IOSurfaceCreate((CFDictionaryRef)properties);
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h
                                                                                    mipmapped:NO];
            d.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> texture = surface && device ? [device newTextureWithDescriptor:d iosurface:surface plane:0] : nil;
            id<MTLBuffer> buffer = [device newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
            id<MTLCommandQueue> queue = [device newCommandQueue];
            if (!texture || !buffer || !queue) {
                printf("coherence: surface %p texture %p buffer %p queue %p\n", surface, texture, buffer, queue);
                return 1;
            }
            size_t row = IOSurfaceGetBytesPerRow(surface);
            printf("coherence: surface cache mode %#x\n", IOSurfaceGetCacheMode(surface));
            for (unsigned round = 0; round < rounds; round++) {
                IOSurfaceLock(surface, 0, NULL);
                uint8_t *base = IOSurfaceGetBaseAddress(surface);
                for (unsigned y = 0; y < h; y++)
                    for (unsigned x = 0; x < w; x++)
                        ((uint32_t *)(base + y * row))[x] = 0xff000000u | (round * 0x10101u + y * 3 + x) % 0xffffff;
                IOSurfaceUnlock(surface, 0, NULL);
                id<MTLCommandBuffer> commands = [queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
                [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                             toBuffer:buffer destinationOffset:0 destinationBytesPerRow:(NSUInteger)w * 4 destinationBytesPerImage:(NSUInteger)w * h * 4];
                [blit endEncoding];
                [commands commit];
                [commands waitUntilCompleted];
                const uint32_t *seen = buffer.contents;
                size_t wrong = 0, zero = 0;
                for (unsigned y = 0; y < h; y++)
                    for (unsigned x = 0; x < w; x++) {
                        uint32_t want = 0xff000000u | (round * 0x10101u + y * 3 + x) % 0xffffff, got = seen[(size_t)y * w + x];
                        if (got != want) {
                            wrong++;
                            zero += got == 0;
                        }
                    }
                printf("coherence round %u: %zu of %u words differ (%zu zero), status %ld\n", round, wrong, w * h, zero, (long)commands.status);
            }
        } else if (!strcmp(argv[1], "rotbench")) {
            unsigned passes = (unsigned)strtoul(argv[4], NULL, 0), rotation = argc > 5 ? (unsigned)strtoul(argv[5], NULL, 0) : 0;
            bool clear = argc > 6 && atoi(argv[6]);
            unsigned sw = rotation % 2 ? h : w, sh = rotation % 2 ? w : h;
            IOSurfaceRef surface = IOSurfaceCreate((CFDictionaryRef)@{(id)kIOSurfaceWidth: @(sw), (id)kIOSurfaceHeight: @(sh),
                (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfacePixelFormat: @((unsigned)'BGRA')});
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h
                                                                                    mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget;
            // The SPI CoreAnimation sets on its framebuffer's texture (QuartzCore: setRotation:, framebufferOnly).
            ((void (*)(id, SEL, NSUInteger))objc_msgSend)(d, sel_registerName("setRotation:"), rotation);
            ((void (*)(id, SEL, BOOL))objc_msgSend)(d, sel_registerName("setFramebufferOnly:"), YES);
            id<MTLTexture> target = surface ? [device newTextureWithDescriptor:d iosurface:surface plane:0] : nil;
            static const char *source =
                "#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n"
                "vertex V vs(uint i [[vertex_id]]) { float2 q = float2((i << 1) & 2, i & 2); V v; v.p = float4(q * 2 - 1, 0, 1); return v; }\n"
                "fragment float4 fs(V v [[stage_in]]) { uint2 p = uint2(v.p.xy);\n"
                "  return float4(float(p.x & 255), float(p.y & 255), float((p.x >> 8) | ((p.y >> 8) << 4)), 255) / 255; }\n";
            NSError *error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:@(source) options:nil error:&error];
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [library newFunctionWithName:@"vs"];
            pd.fragmentFunction = [library newFunctionWithName:@"fs"];
            pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            id<MTLRenderPipelineState> pipeline = library ? [device newRenderPipelineStateWithDescriptor:pd error:&error] : nil;
            id<MTLCommandQueue> queue = [device newCommandQueue];
            if (!target || !pipeline || !queue) {
                printf("rotbench: surface %p target %p pipeline %p: %s\n", surface, target, pipeline, error.description.UTF8String);
                return 1;
            }
            // The first command buffer warms up (the twin image, pipelines); the second is timed.
            for (int round = 0; round < 2; round++) {
                id<MTLCommandBuffer> commands = [queue commandBuffer];
                for (unsigned p = 0; p < (round ? passes : 1); p++) {
                    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                    pass.colorAttachments[0].texture = target;
                    pass.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
                    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                    id<MTLRenderCommandEncoder> draw = [commands renderCommandEncoderWithDescriptor:pass];
                    [draw setRenderPipelineState:pipeline];
                    [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    [draw endEncoding];
                }
                struct timespec begin, end;
                clock_gettime(CLOCK_MONOTONIC, &begin);
                [commands commit];
                [commands waitUntilCompleted];
                clock_gettime(CLOCK_MONOTONIC, &end);
                const double wall = (end.tv_sec - begin.tv_sec) * 1e3 + (end.tv_nsec - begin.tv_nsec) / 1e6,
                             gpu = (commands.GPUEndTime - commands.GPUStartTime) * 1e3;
                if (round)
                    printf("rotbench %ux%u rotation %u %s: %u passes, gpu %.2f ms (%.3f ms a pass), wall %.2f ms, status %ld\n", w, h,
                           rotation, clear ? "clear" : "load", passes, gpu, gpu / passes, wall, (long)commands.status);
            }
            // Where each frame texel (x, y) lands in the surface: Metal's store for rotations 0 to 3 (rotated.rs), and the four
            // mirrors (transpose, anti-transpose, horizontal, vertical) a wrong turn would make.
            IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
            const uint8_t *base = IOSurfaceGetBaseAddress(surface);
            const size_t row = IOSurfaceGetBytesPerRow(surface);
            static const char *names[8] = {"rot0", "rot1", "rot2", "rot3", "transpose", "anti-transpose", "hflip", "vflip"};
            size_t hits[8] = {}, texels = (size_t)sw * sh;
            for (unsigned py = 0; py < sh; py++)
                for (unsigned px = 0; px < sw; px++) {
                    const uint8_t *t = base + py * row + px * 4; // BGRA: B G R A
                    unsigned x = t[2] | (t[0] & 15) << 8, y = t[1] | (t[0] >> 4) << 8;
                    const unsigned map[8][2] = {{x, y}, {h - 1 - y, x}, {w - 1 - x, h - 1 - y}, {y, w - 1 - x},
                                                {y, x}, {h - 1 - y, w - 1 - x}, {w - 1 - x, y}, {x, h - 1 - y}};
                    for (int k = 0; k < 8; k++)
                        hits[k] += map[k][0] == px && map[k][1] == py;
                }
            IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
            printf("rotbench check: %zu of %zu texels where Metal stores rotation %u;", hits[rotation & 3], texels, rotation);
            for (int k = 0; k < 8; k++)
                if (hits[k] && k != (int)(rotation & 3))
                    printf(" %s %zu", names[k], hits[k]);
            printf("\n");
        } else if (!strcmp(argv[1], "gpubw")) {
            unsigned rounds = (unsigned)strtoul(argv[4], NULL, 0);
            NSArray<NSString *> *kinds = @[@"private", @"shared", @"iosurface", @"iosurface-wc", @"ca", @"buffer"];
            if (argc > 5) {
                NSMutableArray *chosen = [NSMutableArray array];
                for (int i = 5; i < argc; i++) [chosen addObject:@(argv[i])];
                kinds = chosen;
            }
            static const char *source =
                "#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n"
                "vertex V vs(uint i [[vertex_id]]) { float2 q = float2((i << 1) & 2, i & 2); V v; v.p = float4(q * 2 - 1, 0, 1); return v; }\n"
                "fragment float4 copy(V v [[stage_in]], texture2d<float> t [[texture(0)]]) { return t.read(uint2(v.p.xy)); }\n"
                "fragment float4 blur(V v [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
                "  uint2 c = uint2(v.p.xy); uint w = t.get_width() - 1; float4 sum = 0;\n"
                "  for (int d = -4; d <= 4; d++) sum += t.read(uint2(clamp(int(c.x) + d, 0, int(w)), c.y));\n"
                "  return sum / 9; }\n";
            NSError *error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:@(source) options:nil error:&error];
            id<MTLCommandQueue> queue = [device newCommandQueue];
            id<MTLRenderPipelineState> pipelines[2] = {nil, nil};
            for (int i = 0; i < 2 && library; i++) {
                MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
                d.vertexFunction = [library newFunctionWithName:@"vs"];
                d.fragmentFunction = [library newFunctionWithName:i ? @"blur" : @"copy"];
                d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                pipelines[i] = [device newRenderPipelineStateWithDescriptor:d error:&error];
            }
            if (!queue || !pipelines[0] || !pipelines[1]) {
                printf("gpubw: library %p pipelines %p %p: %s\n", library, pipelines[0], pipelines[1], error.description.UTF8String);
                return 1;
            }
            const double bytes = 2.0 * w * h * 4 * rounds; // each copy reads one image and writes the other
            for (NSString *kind in kinds) {
                id images[2] = {nil, nil};
                for (int i = 0; i < 2; i++) {
                    if ([kind isEqualToString:@"buffer"]) {
                        images[i] = [device newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
                        continue;
                    }
                    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w
                                                                                                height:h mipmapped:NO];
                    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
                    IOSurfaceRef surface = NULL;
                    if ([kind isEqualToString:@"private"] || [kind isEqualToString:@"shared"]) {
                        d.storageMode = [kind isEqualToString:@"private"] ? MTLStorageModePrivate : MTLStorageModeShared;
                        images[i] = [device newTextureWithDescriptor:d];
                        continue;
                    } else if ([kind isEqualToString:@"ca"]) {
                        surface = CAIOSurfaceCreateDefault(w, h, 'BGRA');
                    } else {
                        NSMutableDictionary *properties = [@{(id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h),
                            (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfacePixelFormat: @((unsigned)'BGRA')} mutableCopy];
                        if ([kind isEqualToString:@"iosurface-wc"])
                            properties[(id)kIOSurfaceCacheMode] = @0x400;
                        surface = IOSurfaceCreate((CFDictionaryRef)properties);
                    }
                    // The surface stays alive for the process: a texture over it may not keep it.
                    images[i] = surface ? [device newTextureWithDescriptor:d iosurface:surface plane:0] : nil;
                }
                if (!images[0] || !images[1]) {
                    printf("gpubw %s: no images (%p %p)\n", kind.UTF8String, images[0], images[1]);
                    continue;
                }
                const bool buffer = [kind isEqualToString:@"buffer"];
                for (int op = 0; op < (buffer ? 1 : 3); op++) {
                    // The first command buffer warms up (page mapping, pipeline upload); the second is timed.
                    for (int pass = 0; pass < 2; pass++) {
                        id<MTLCommandBuffer> commands = [queue commandBuffer];
                        for (unsigned r = 0; r < (pass ? rounds : 1); r++) {
                            id from = images[r & 1], to = images[!(r & 1)];
                            if (op == 0) {
                                id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
                                if (buffer)
                                    [blit copyFromBuffer:from sourceOffset:0 toBuffer:to destinationOffset:0 size:(NSUInteger)w * h * 4];
                                else
                                    [blit copyFromTexture:from toTexture:to];
                                [blit endEncoding];
                                continue;
                            }
                            MTLRenderPassDescriptor *target = [MTLRenderPassDescriptor renderPassDescriptor];
                            target.colorAttachments[0].texture = to;
                            target.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                            target.colorAttachments[0].storeAction = MTLStoreActionStore;
                            id<MTLRenderCommandEncoder> draw = [commands renderCommandEncoderWithDescriptor:target];
                            [draw setRenderPipelineState:pipelines[op - 1]];
                            [draw setFragmentTexture:from atIndex:0];
                            [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                            [draw endEncoding];
                        }
                        struct timespec begin, end;
                        clock_gettime(CLOCK_MONOTONIC, &begin);
                        [commands commit];
                        [commands waitUntilCompleted];
                        clock_gettime(CLOCK_MONOTONIC, &end);
                        const double wall = (end.tv_sec - begin.tv_sec) * 1e3 + (end.tv_nsec - begin.tv_nsec) / 1e6, gpu = (commands.GPUEndTime - commands.GPUStartTime) * 1e3;
                        if (pass)
                            printf("gpubw %-12s %-5s %u x %ux%u: gpu %.2f ms, wall %.2f ms, %.2f GB/s, status %ld\n", kind.UTF8String,
                                   op == 0 ? "blit" : op == 1 ? "draw" : "blur", rounds, w, h, gpu, wall,
                                   bytes / ((gpu > 0 ? gpu : wall) * 1e6), (long)commands.status);
                    }
                }
            }
        } else if (!strcmp(argv[1], "plain")) {
            for (int i = 4; i < argc; i++) {
                OSType fcc = parseFourcc(argv[i]);
                NSDictionary *properties = @{(id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h),
                                             (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfacePixelFormat: @(fcc)};
                describe([NSString stringWithFormat:@"plain %@", fourcc(fcc)], IOSurfaceCreate((CFDictionaryRef)properties),
                         device, MTLPixelFormatBGRA8Unorm, w, h);
            }
        } else if (!strcmp(argv[1], "open")) {
            for (int i = 4; i < argc; i++) {
                io_object_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(argv[i]));
                io_object_t connect = 0;
                kern_return_t kr = service ? IOServiceOpen(service, mach_task_self(), 0, &connect) : KERN_FAILURE;
                printf("open %s: service %#x, IOServiceOpen %#x\n", argv[i], service, kr);
                if (connect) IOServiceClose(connect);
                if (service) IOObjectRelease(service);
            }
        } else {
            fprintf(stderr, "unknown verb %s\n", argv[1]);
            return 2;
        }
    }
    return 0;
}

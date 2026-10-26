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
//   gfxprobe box PROFILE[:UID] VERB ARG...  the verb inside the named sandbox profile (sandbox_init), e.g. com.apple.WebKit.GPU:501:
//                                       what a sandboxed process can do with the GPU; whether the plugin's files and its
//                                       shader-cache directories can be read and written
//   gfxprobe coherence W H ROUNDS [MODE] the CPU fills a BGRA surface with a new pattern each round and the GPU copies it to a buffer
//                                       (a blit of a texture over the surface): the words the GPU did not see as the CPU left them
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
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>
#import <dlfcn.h>
#import <mach-o/dyld.h>
#import <mach-o/loader.h>
#import <fcntl.h>
#import <objc/runtime.h>
#import <pthread.h>
#import <spawn.h>
#import <sys/wait.h>
#import <mach/mach.h>
#import <stdatomic.h>
#import <os/lock.h>
#import <sys/mman.h>
#import <sys/stat.h>
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

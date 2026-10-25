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
//                                       3 landscape left, 4 landscape right), as XCTest rotates a device. Tried 2026-10-25 with
//                                       com.apple.backboard.client: backboardd did not act on it. What keeps the home screen
//                                       landscape after an app that turns the interface (GFXBench) is iPadOS's rotation lock
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
#import <fcntl.h>
#import <objc/runtime.h>
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

int main(int argc, char **argv) {
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

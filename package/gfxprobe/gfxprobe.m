// gfxprobe: make the IOSurfaces the system's renderers make, inside the guest, and ask the Metal device for a texture on each.
//
//   gfxprobe rb W H FORMAT[:FLAGS]...   RenderBox's own RB::create_iosurface(MTLPixelFormat, w, h, flags) (what an RBLayer --
//                                       SwiftUI in a widget -- draws into), then newTextureWithDescriptor:iosurface:plane:0
//   gfxprobe ca W H FOURCC...           QuartzCore's CAIOSurfaceCreateDefault(w, h, fourcc), the same texture request
//   gfxprobe plain W H FOURCC...        a plain IOSurfaceCreate (width, height, 4 bytes per element, the pixel format)
//   gfxprobe open W H SERVICE...        IOServiceOpen of each IOKit service class (type 0): the kern_return_t
//   gfxprobe sync PATH...               F_FULLFSYNC each file, then sync(): a file staged into the Data overlay (export.py put)
//                                       reaches the store before the VM stops (vm.sh stop is no clean shutdown)
//
// FORMAT is an MTLPixelFormat number (80 BGRA8Unorm, 81 _sRGB, 115 RGBA16Float, 552-555 the XR formats; RenderBox sets bit 31
// for a variant of its own), FLAGS RenderBox's create flags (hex). Each line prints the surface's IOSurface pixel format, plane
// count, bytes per row, allocation and base address, and whether the device made a texture. RB::create_iosurface is not
// exported: its address is RenderBox's slide (from -[RBDrawable size]) plus its address in the iPadOS 17.7.11 (21H461) cache.
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>
#import <dlfcn.h>
#import <fcntl.h>
#import <objc/runtime.h>

static const uintptr_t kRBDrawableSize = 0x1e2687330;      // -[RBDrawable size], unslid
static const uintptr_t kRBCreateIOSurface = 0x1e2638c84;   // RB::create_iosurface(MTLPixelFormat, unsigned, unsigned, unsigned)

typedef IOSurfaceRef (*CreateIOSurface)(unsigned long format, unsigned width, unsigned height, unsigned flags);
extern IOSurfaceRef CAIOSurfaceCreateDefault(unsigned width, unsigned height, unsigned fourcc);   // QuartzCore, private

// IOKit's C interface; the iphoneos SDK ships the framework without its headers.
typedef mach_port_t io_object_t;
extern const mach_port_t kIOMainPortDefault;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_object_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_object_t service, task_port_t owningTask, uint32_t type, io_object_t *connect);
extern kern_return_t IOServiceClose(io_object_t connect);
extern kern_return_t IOObjectRelease(io_object_t object);

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

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
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
        if (argc < 5) {
            fprintf(stderr, "usage: gfxprobe rb|ca|plain|open W H ARG... | sync PATH...\n");
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

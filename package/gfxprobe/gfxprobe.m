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
//
// FORMAT is an MTLPixelFormat number (80 BGRA8Unorm, 81 _sRGB, 115 RGBA16Float, 552-555 the XR formats; RenderBox sets bit 31
// for a variant of its own), FLAGS RenderBox's create flags (hex). Each line prints the surface's IOSurface pixel format, plane
// count, bytes per row, allocation and base address, and whether the device made a texture. RB::create_iosurface is not
// exported: its address is RenderBox's slide (from -[RBDrawable size]) plus its address in the iPadOS 17.7.11 (21H461) cache.
#import <CoreGraphics/CoreGraphics.h>
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

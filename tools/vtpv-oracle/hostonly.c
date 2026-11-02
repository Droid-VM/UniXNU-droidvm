// hostonly.c: VideoToolbox's host session alone (no guest in this process): each FILE is delivered as a message from a guest,
// and every message the host sends back is written to OUTDIR/reply-NN-TYPE.bin.   ./hostonly OUTDIR FILE...
// A file whose message is an efrm gets an IOSurface of this process (HOSTONLY_SIZE=WxH, default 1280x720, 420v, a gradient);
// the host's own requests to the guest (flags 2) are answered at once with ,err 0 (flags 8).
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dispatch/dispatch.h>

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *out = argv[1];
    void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW);
    OSStatus (*create)(void (^)(CMBlockBufferRef), CFTypeRef *) = dlsym(vt, "VTParavirtualizationHostSessionCreate");
    OSStatus (*deliver)(CFTypeRef, CMBlockBufferRef, void *, void *, uint64_t *, size_t) =
        dlsym(vt, "VTParavirtualizationHostSessionDeliverMessageFromGuest");
    __block int n = 0;
    // Not __block: create copies the handler, which would move a __block variable to the heap after &session was taken.
    static CFTypeRef session;
    CFTypeRef created = NULL;
    OSStatus status = create(^(CMBlockBufferRef message) {
        const size_t length = CMBlockBufferGetDataLength(message);
        uint8_t *bytes = malloc(length);
        CMBlockBufferCopyDataBytes(message, 0, length, bytes);
        char path[1024], type[5] = "????";
        memcpy(type, bytes + 8, 4);
        snprintf(path, sizeof path, "%s/reply-%02d-%s.bin", out, n++, type);
        FILE *f = fopen(path, "wb");
        fwrite(bytes, 1, length, f);
        fclose(f);
        printf("hostonly: %s (%zu bytes)\n", path, length);
        // A request of the host's (flags 2): the guest's reply, ,err 0 (flags 8), delivered back on a queue of its own. An Efds
        // (make a format description) is answered as the guest does: the description, made here, as a "=fds" ImageDescription.
        if (length >= 48 && bytes[15] == 2) {
            size_t replyLength = 68;
            uint8_t *reply = calloc(1, 68 + 8192);
            memcpy(reply, bytes, 48);
            reply[15] = 8;
            const uint8_t param[20] = {0, 0, 0, 20, 'p', 'a', 'r', 'a', ',', 'e', 'r', 'r', '=', 's', '3', '2', 0, 0, 0, 0};
            memcpy(reply + 48, param, 20);
            if (!memcmp(bytes + 8, "Efds", 4)) {
                int32_t codec = 0, w = 0, h = 0;
                CFDictionaryRef extensions = NULL;
                for (size_t at = 48; at + 16 <= length;) {
                    const uint32_t size = (uint32_t)bytes[at] << 24 | bytes[at + 1] << 16 | bytes[at + 2] << 8 | bytes[at + 3];
                    if (size < 16 || at + size > length) break;
                    const uint8_t *key = bytes + at + 8, *value = bytes + at + 16;
                    const int32_t v32 = (int32_t)((uint32_t)value[0] << 24 | value[1] << 16 | value[2] << 8 | value[3]);
                    if (!memcmp(key, ",cod", 4)) codec = v32;
                    else if (!memcmp(key, ",wdt", 4)) w = v32;
                    else if (!memcmp(key, ",hgt", 4)) h = v32;
                    else if (!memcmp(key, ",ext", 4)) {
                        CFDataRef data = CFDataCreate(NULL, value, size - 16);
                        extensions = CFPropertyListCreateWithData(NULL, data, 0, NULL, NULL);
                        CFRelease(data);
                    }
                    at += size;
                }
                CMVideoFormatDescriptionRef format = NULL;
                OSStatus s = CMVideoFormatDescriptionCreate(NULL, codec, w, h, extensions, &format);
                CMBlockBufferRef description = NULL;
                if (!s) s = CMVideoFormatDescriptionCopyAsBigEndianImageDescriptionBlockBuffer(NULL, format, CFStringGetSystemEncoding(), NULL, &description);
                if (!s) {
                    const size_t n = CMBlockBufferGetDataLength(description);
                    const uint32_t size = 16 + (uint32_t)n;
                    uint8_t *p = reply + 68;
                    p[0] = size >> 24; p[1] = size >> 16; p[2] = size >> 8; p[3] = size;
                    memcpy(p + 4, "para=fds=fds", 12);
                    CMBlockBufferCopyDataBytes(description, 0, n, p + 16);
                    replyLength += size;
                }
                printf("hostonly: Efds %.4s %dx%d -> %d\n", (const char *)&(uint32_t){__builtin_bswap32((uint32_t)codec)}, w, h, (int)s);
            }
            dispatch_async(dispatch_get_global_queue(0, 0), ^{
                CMBlockBufferRef m = NULL;
                CMBlockBufferCreateWithMemoryBlock(NULL, reply, replyLength, kCFAllocatorMalloc, NULL, 0, replyLength, 0, &m);
                OSStatus s = deliver(session, m, NULL, NULL, NULL, 0);
                printf("hostonly: answered the host's %.4s: %d\n", (const char *)reply + 8, (int)s);
                CFRelease(m);
            });
        }
        free(bytes);
    }, &created);
    session = created;
    printf("hostonly: session %d\n", (int)status);
    for (int i = 2; i < argc; ++i) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        const long length = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *bytes = malloc(length);
        fread(bytes, 1, length, f);
        fclose(f);
        CMBlockBufferRef message = NULL;
        const bool frame = length >= 12 && !memcmp(bytes + 8, "efrm", 4);
        CMBlockBufferCreateWithMemoryBlock(NULL, bytes, length, kCFAllocatorMalloc, NULL, 0, length, 0, &message);
        IOSurfaceRef surface = NULL;
        uint64_t id = 0;
        if (frame) {
            int w = 1280, h = 720;
            if (getenv("HOSTONLY_SIZE")) sscanf(getenv("HOSTONLY_SIZE"), "%dx%d", &w, &h);
            CVPixelBufferRef pixels = NULL;
            CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            const void *k[] = {kCVPixelBufferIOSurfacePropertiesKey};
            const void *v[] = {empty};
            CFDictionaryRef attributes = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CVPixelBufferCreate(NULL, w, h, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes, &pixels);
            CVPixelBufferLockBaseAddress(pixels, 0);
            uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pixels, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(pixels, 1);
            for (int r = 0; r < h; ++r) for (int x = 0; x < w; ++x) y[r * CVPixelBufferGetBytesPerRowOfPlane(pixels, 0) + x] = 16 + (x + r) % 220;
            for (int r = 0; r < h / 2; ++r) memset(uv + r * CVPixelBufferGetBytesPerRowOfPlane(pixels, 1), 128, w);
            CVPixelBufferUnlockBaseAddress(pixels, 0);
            surface = (IOSurfaceRef)CFRetain(CVPixelBufferGetIOSurface(pixels));
            id = IOSurfaceGetID(surface);
        }
        status = deliver(session, message, NULL, surface ? &surface : NULL, surface ? &id : NULL, surface ? 1 : 0);
        printf("hostonly: delivered %s: %d\n", argv[i], (int)status);
        CFRelease(message);
        sleep(1);
    }
    return 0;
}

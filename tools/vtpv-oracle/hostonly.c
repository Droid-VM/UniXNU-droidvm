// hostonly.c: VideoToolbox's host session alone (no guest in this process): each FILE is delivered as a message from a guest,
// and every message the host sends back is written to OUTDIR/reply-NN-TYPE.bin.   ./hostonly OUTDIR FILE...
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    const char *out = argv[1];
    void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW);
    OSStatus (*create)(void (^)(CMBlockBufferRef), CFTypeRef *) = dlsym(vt, "VTParavirtualizationHostSessionCreate");
    OSStatus (*deliver)(CFTypeRef, CMBlockBufferRef, void *, void *, uint64_t *, size_t) =
        dlsym(vt, "VTParavirtualizationHostSessionDeliverMessageFromGuest");
    __block int n = 0;
    CFTypeRef session = NULL;
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
        free(bytes);
    }, &session);
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
        CMBlockBufferCreateWithMemoryBlock(NULL, bytes, length, kCFAllocatorMalloc, NULL, 0, length, 0, &message);
        status = deliver(session, message, NULL, NULL, NULL, 0);
        printf("hostonly: delivered %s: %d\n", argv[i], (int)status);
        CFRelease(message);
        sleep(1);
    }
    return 0;
}

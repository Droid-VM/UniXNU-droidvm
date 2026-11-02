// oracle.c: decode one H.264 frame and encode a few frames through VideoToolbox's paravirtualized codecs, with interpose.dylib
// turning this process into its own guest and host (loopback), so that every message of both directions is recorded.
//   VTPV_LOG=dir DYLD_INSERT_LIBRARIES=./interpose.dylib ./oracle [decode] [encode h264|hevc] [frames N]
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tiny-h264.inc"

static CFStringRef key(const char *name) {
    void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW);
    CFStringRef *p = dlsym(vt, name);
    if (!p) { fprintf(stderr, "no %s\n", name); exit(1); }
    return *p;
}

static void decoded(void *a, void *b, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image, CMTime pts, CMTime d) {
    (void)a; (void)b; (void)pts; (void)d;
    printf("decode: frame status %d flags %#x image %p\n", (int)status, (unsigned)flags, image);
    if (status || !image) return;
    CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(image, 0);
    const size_t row = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
    OSType f = CVPixelBufferGetPixelFormatType(image);
    printf("decode: %zux%zu %c%c%c%c row %zu luma row 8: %u %u %u %u %u %u %u %u\n", CVPixelBufferGetWidth(image),
           CVPixelBufferGetHeight(image), f >> 24, f >> 16, f >> 8, f, row, y[8 * row], y[8 * row + 8], y[8 * row + 16],
           y[8 * row + 24], y[8 * row + 32], y[8 * row + 40], y[8 * row + 48], y[8 * row + 56]);
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
}

static int decode(void) {
    const uint8_t *sets[2] = {kTinySPS, kTinyPPS};
    const size_t sizes[2] = {sizeof kTinySPS, sizeof kTinyPPS};
    CMVideoFormatDescriptionRef format = NULL;
    OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, sets, sizes, 4, &format);
    if (status) return printf("decode: format %d\n", (int)status), 1;
    const void *sk[] = {key("kVTVideoDecoderSpecification_RequireParavirtualizedDecoder")};
    const void *sv[] = {kCFBooleanTrue};
    CFDictionaryRef spec = CFDictionaryCreate(NULL, sk, sv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int pf = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef pfn = CFNumberCreate(NULL, kCFNumberIntType, &pf);
    CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *ak[] = {kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferIOSurfacePropertiesKey};
    const void *av[] = {pfn, empty};
    CFDictionaryRef attributes = CFDictionaryCreate(NULL, ak, av, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    VTDecompressionOutputCallbackRecord callback = {decoded, NULL};
    VTDecompressionSessionRef session = NULL;
    status = VTDecompressionSessionCreate(NULL, format, spec, attributes, &callback, &session);
    printf("decode: session %d\n", (int)status);
    if (status) return 1;
    uint8_t *sample = malloc(4 + sizeof kTinyIDR);
    const uint32_t be = CFSwapInt32HostToBig((uint32_t)sizeof kTinyIDR);
    memcpy(sample, &be, 4);
    memcpy(sample + 4, kTinyIDR, sizeof kTinyIDR);
    const size_t sampleSize = 4 + sizeof kTinyIDR;
    CMBlockBufferRef block = NULL;
    CMSampleBufferRef buffer = NULL;
    CMBlockBufferCreateWithMemoryBlock(NULL, sample, sampleSize, kCFAllocatorMalloc, NULL, 0, sampleSize, 0, &block);
    CMSampleTimingInfo timing = {CMTimeMake(1, 30), CMTimeMake(0, 30), kCMTimeInvalid};
    CMSampleBufferCreateReady(NULL, block, format, 1, 1, &timing, 1, &sampleSize, &buffer);
    VTDecodeInfoFlags info = 0;
    status = VTDecompressionSessionDecodeFrame(session, buffer, kVTDecodeFrame_EnableAsynchronousDecompression, NULL, &info);
    VTDecompressionSessionWaitForAsynchronousFrames(session);
    printf("decode: decode %d\n", (int)status);
    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    return 0;
}

static void encoded(void *a, void *b, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
    (void)a; (void)b;
    printf("encode: frame status %d flags %#x %zu bytes\n", (int)status, (unsigned)flags, sample ? CMSampleBufferGetTotalSampleSize(sample) : 0);
}

static int encode(CMVideoCodecType codec, int frames) {
    {
        void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW);
        Boolean (*inProcess)(void) = dlsym(vt, "VTShouldRunVideoEncodersInProcess");
        Boolean (*inGuest)(void) = dlsym(vt, "VTParavirtualizationIsRunningInGuest");
        printf("encode: encoders in process %d, in guest %d\n", inProcess ? inProcess() : -1, inGuest ? inGuest() : -1);
    }
    const char *wanted = getenv("VTPV_ENCODER");
    CFStringRef wantedID = wanted ? CFStringCreateWithCString(NULL, wanted, kCFStringEncodingUTF8) : NULL;
    const void *sk[] = {key("kVTVideoEncoderSpecification_RequireParavirtualizedEncoder"), kVTVideoEncoderSpecification_EncoderID};
    const void *sv[] = {kCFBooleanTrue, wantedID};
    CFDictionaryRef spec = getenv("VTPV_IDONLY") && wantedID
        ? CFDictionaryCreate(NULL, sk + 1, sv + 1, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)
        : CFDictionaryCreate(NULL, sk, sv, wantedID ? 2 : 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const int side = getenv("VTPV_SIDE") ? atoi(getenv("VTPV_SIDE")) : 64;
    VTCompressionSessionRef session = NULL;
    OSStatus status = VTCompressionSessionCreate(NULL, side, side, codec, spec, NULL, NULL, encoded, NULL, &session);
    printf("encode: session %d\n", (int)status);
    if (status) return 1;
    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTCompressionSessionPrepareToEncodeFrames(session);
    for (int i = 0; i < frames; ++i) {
        CVPixelBufferRef pixels = NULL;
        CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        const void *ak[] = {kCVPixelBufferIOSurfacePropertiesKey};
        const void *av[] = {empty};
        CFDictionaryRef attrs = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CVPixelBufferCreate(NULL, side, side, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &pixels);
        CVPixelBufferLockBaseAddress(pixels, 0);
        uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pixels, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(pixels, 1);
        for (int r = 0; r < side; ++r) memset(y + r * CVPixelBufferGetBytesPerRowOfPlane(pixels, 0), 16 + (r * 3 + i * 8) % 200, side);
        for (int r = 0; r < side / 2; ++r) memset(uv + r * CVPixelBufferGetBytesPerRowOfPlane(pixels, 1), 128, side);
        CVPixelBufferUnlockBaseAddress(pixels, 0);
        status = VTCompressionSessionEncodeFrame(session, pixels, CMTimeMake(i, 30), CMTimeMake(1, 30), NULL, NULL, NULL);
        printf("encode: frame %d submit %d\n", i, (int)status);
        CVPixelBufferRelease(pixels);
    }
    status = VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    printf("encode: complete %d\n", (int)status);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    return 0;
}

static void list(void) {
    CFArrayRef decoders = NULL;
    OSStatus (*copyList)(CFDictionaryRef, CFArrayRef *) =
        dlsym(dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW), "VTCopyVideoDecoderList");
    if (copyList) copyList(NULL, &decoders);
    for (CFIndex i = 0; decoders && i < CFArrayGetCount(decoders); ++i) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(decoders, i);
        CFStringRef name = CFDictionaryGetValue(d, CFSTR("CMClassImplementationName"));
        CFStringRef id = CFDictionaryGetValue(d, CFSTR("CMClassImplementationID"));
        char a[256] = "", b[256] = "";
        if (name) CFStringGetCString(name, a, sizeof a, kCFStringEncodingUTF8);
        if (id) CFStringGetCString(id, b, sizeof b, kCFStringEncodingUTF8);
        if (strstr(a, "aravirt") || strstr(b, "aravirt")) { printf("list: %s | %s\n", b, a); CFShow(d); }
    }
    printf("list: %ld decoders\n", decoders ? (long)CFArrayGetCount(decoders) : -1L);
    CFArrayRef encoders = NULL;
    VTCopyVideoEncoderList(NULL, &encoders);
    for (CFIndex i = 0; encoders && i < CFArrayGetCount(encoders); ++i) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(encoders, i);
        CFStringRef id = CFDictionaryGetValue(d, kVTVideoEncoderList_EncoderID);
        char b[256] = "";
        if (id) CFStringGetCString(id, b, sizeof b, kCFStringEncodingUTF8);
        if (strstr(b, "aravirt") || strstr(b, "avc") || strstr(b, "hevc")) printf("list: encoder %s\n", b);
    }
}

int main(int argc, char **argv) {
    int frames = 3;
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "frames") && i + 1 < argc) frames = atoi(argv[++i]);
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "list")) list();
        else if (!strcmp(argv[i], "decode")) decode();
        else if (!strcmp(argv[i], "encode") && i + 1 < argc) {
            ++i;
            encode(!strcmp(argv[i], "hevc") ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264, frames);
        }
    }
    sleep(1);
    return 0;
}

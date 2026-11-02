// interpose.c: VideoToolbox's paravirtualization guest transport (VideoToolboxParavirtualizationSupport.framework) replaced,
// in this process, by a loopback to VideoToolbox's own host session (VTParavirtualizationHostSession*), so that the process is
// both guest and host. Every message is written to $VTPV_LOG/NNNN-{g2h,h2g}-TYPE.bin (the raw bytes) with its IOSurface IDs
// in the name. Load with DYLD_INSERT_LIBRARIES.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>
#include <Block.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (^AllocBlock)(size_t size, CFTypeRef *object, void **bytes);
typedef int (^DeliverBlock)(CFTypeRef object, size_t size);
typedef void (^FreeBlock)(CFTypeRef object);
struct iov { const void *base; size_t length; };

extern int VTParavirtualizationGuestSupportSetUpWithHandlers(AllocBlock, DeliverBlock, FreeBlock);
extern int VTParavirtualizationGuestSupportRegisterGuestUUID(const uint8_t *uuid);
extern int VTParavirtualizationGuestSupportDeregisterGuestUUID(const uint8_t *uuid);
extern int VTParavirtualizationGuestSupportSendRawMessageToHost(const struct iov *iov, size_t count, IOSurfaceRef *surfaces,
                                                                size_t surfaceCount);

static AllocBlock guestAlloc;
static DeliverBlock guestDeliver;
static FreeBlock guestFree;
static CFTypeRef hostSession;
static dispatch_queue_t toHost, toGuest;
static atomic_uint sequence;
static OSStatus (*deliverFromGuest)(CFTypeRef, CMBlockBufferRef, void *, IOSurfaceRef *, uint64_t *, size_t);

static void record(const char *direction, const uint8_t *bytes, size_t length, const uint64_t *ids, size_t count) {
    const char *dir = getenv("VTPV_LOG");
    if (!dir) return;
    const unsigned n = atomic_fetch_add(&sequence, 1);
    char type[5] = "????";
    if (length >= 12) for (int i = 0; i < 4; ++i) type[i] = (bytes[8 + i] >= 32 && bytes[8 + i] < 127 && bytes[8 + i] != '/') ? (char)bytes[8 + i] : '_';
    char path[1024], surfaces[128] = "";
    for (size_t i = 0; i < count; ++i) snprintf(surfaces + strlen(surfaces), sizeof surfaces - strlen(surfaces), "-s%llu", ids[i]);
    snprintf(path, sizeof path, "%s/%04u-%s-%s%s.bin", dir, n, direction, type, surfaces);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(bytes, 1, length, f); fclose(f); }
}

static void toGuestMessage(CMBlockBufferRef message) {
    const size_t length = CMBlockBufferGetDataLength(message);
    uint8_t *bytes = malloc(length ? length : 1);
    CMBlockBufferCopyDataBytes(message, 0, length, bytes);
    record("h2g", bytes, length, NULL, 0);
    dispatch_async(toGuest, ^{
        CFTypeRef object = NULL;
        void *buffer = NULL;
        if (guestAlloc(length < 0x4000 ? 0x4000 : length, &object, &buffer) || !buffer) { fprintf(stderr, "interpose: alloc failed\n"); free(bytes); return; }
        memcpy(buffer, bytes, length);
        free(bytes);
        const int delivered = guestDeliver(object, length);
        if (getenv("VTPV_VERBOSE")) fprintf(stderr, "interpose: delivered %zu bytes to the guest: %d\n", length, delivered);
        if (object) guestFree(object);
    });
}

static int mySetUp(AllocBlock alloc, DeliverBlock deliver, FreeBlock release) {
    guestAlloc = Block_copy(alloc);
    guestDeliver = Block_copy(deliver);
    guestFree = Block_copy(release);
    toHost = dispatch_queue_create("loopback.to-host", NULL);
    toGuest = dispatch_queue_create("loopback.to-guest", NULL);
    void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW);
    OSStatus (*create)(void (^)(CMBlockBufferRef), CFTypeRef *) = dlsym(vt, "VTParavirtualizationHostSessionCreate");
    deliverFromGuest = dlsym(vt, "VTParavirtualizationHostSessionDeliverMessageFromGuest");
    if (!create || !deliverFromGuest) { fprintf(stderr, "interpose: no host session API\n"); return -1; }
    OSStatus status = create(^(CMBlockBufferRef message) { toGuestMessage(message); }, &hostSession);
    fprintf(stderr, "interpose: guest transport set up, host session %d\n", (int)status);
    return status;
}

static int myRegister(const uint8_t *uuid) { (void)uuid; return 0; }
static int myDeregister(const uint8_t *uuid) { (void)uuid; return 0; }

static int mySend(const struct iov *iov, size_t count, IOSurfaceRef *surfaces, size_t surfaceCount) {
    size_t length = 0;
    for (size_t i = 0; i < count; ++i) length += iov[i].length;
    uint8_t *bytes = malloc(length);
    size_t at = 0;
    for (size_t i = 0; i < count; ++i) { memcpy(bytes + at, iov[i].base, iov[i].length); at += iov[i].length; }
    uint64_t *ids = calloc(5, sizeof(uint64_t));
    IOSurfaceRef *held = calloc(5, sizeof(IOSurfaceRef));
    for (size_t i = 0; i < surfaceCount && i < 5; ++i) { ids[i] = IOSurfaceGetID(surfaces[i]); held[i] = (IOSurfaceRef)CFRetain(surfaces[i]); }
    record("g2h", bytes, length, ids, surfaceCount);
    // The decoder and encoder lists are asked inside VideoToolbox's registry initialization (a dispatch_once), which the host
    // session, in this same process, needs too: answered from a reply recorded by hostonly ($VTPV_CANNED/decs.bin, encs.bin),
    // with this message's UUID and request ID.
    const char *canned = getenv("VTPV_CANNED");
    if (canned && length >= 48 && (!memcmp(bytes + 8, "decs", 4) || !memcmp(bytes + 8, "encs", 4))) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%.4s.bin", canned, (const char *)bytes + 8);
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            const long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *reply = malloc(size);
            fread(reply, 1, size, f);
            fclose(f);
            memcpy(reply + 16, bytes + 16, 24);   // the guest UUID and the request ID
            CMBlockBufferRef message = NULL;
            CMBlockBufferCreateWithMemoryBlock(NULL, reply, size, kCFAllocatorMalloc, NULL, 0, size, 0, &message);
            toGuestMessage(message);
            CFRelease(message);
            free(bytes);
            free(ids);
            free(held);
            return 0;
        }
    }
    dispatch_async(toHost, ^{
        CMBlockBufferRef message = NULL;
        CMBlockBufferCreateWithMemoryBlock(NULL, bytes, length, kCFAllocatorMalloc, NULL, 0, length, 0, &message);
        OSStatus status = deliverFromGuest(hostSession, message, NULL, held, ids, surfaceCount);
        if (status) fprintf(stderr, "interpose: host refused a message: %d\n", (int)status);
        for (size_t i = 0; i < surfaceCount && i < 5; ++i) CFRelease(held[i]);
        free(held);
        free(ids);
        CFRelease(message);
    });
    return 0;
}

// VideoToolbox encodes in a remote service unless the process has com.apple.videotoolbox.encode-in-process (which AMFI keeps
// from an ad-hoc signature): granted here, so the encoders run in this process, in its guest mode.
typedef struct __SecTask *SecTaskRef;
extern CFTypeRef SecTaskCopyValueForEntitlement(SecTaskRef task, CFStringRef entitlement, CFErrorRef *error);
static CFTypeRef myCopyEntitlement(SecTaskRef task, CFStringRef entitlement, CFErrorRef *error) {
    if (entitlement && (CFEqual(entitlement, CFSTR("com.apple.videotoolbox.encode-in-process")) ||
                        CFEqual(entitlement, CFSTR("com.apple.videotoolbox.decode-in-process"))))
        return kCFBooleanTrue;
    return SecTaskCopyValueForEntitlement(task, entitlement, error);
}

__attribute__((used)) static struct { const void *replacement, *replacee; } interposers[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void *)myCopyEntitlement, (const void *)SecTaskCopyValueForEntitlement},
    {(const void *)mySetUp, (const void *)VTParavirtualizationGuestSupportSetUpWithHandlers},
    {(const void *)myRegister, (const void *)VTParavirtualizationGuestSupportRegisterGuestUUID},
    {(const void *)myDeregister, (const void *)VTParavirtualizationGuestSupportDeregisterGuestUUID},
    {(const void *)mySend, (const void *)VTParavirtualizationGuestSupportSendRawMessageToHost},
};

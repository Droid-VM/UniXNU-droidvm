// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The host half of VideoToolbox's paravirtualized decoder and encoder, in the guest's own kernel: what a Mac running an Apple
// virtual machine answers, played over crosvm's virtio-media decoder and encoder (DroidVMVirtioMedia.c; stateful V4L2 codecs over
// the phone's MediaCodec). The encoder's half is described where it begins ("The encoder").
// DroidVMVTParavirt.cpp is the transport (Apple's AppleVideoToolboxParavirtualizationDriver user client) and hands every message
// here. The messages and their parameters are those a loopback of VideoToolbox's own guest and host halves exchanges on macOS 27
// (DroidVMVTWire.h for the format):
//   decs   the decoder list: one entry per coded format the virtio-media decoder offers that VideoToolbox can describe (H.264,
//          HEVC). The guest registers each as "paravirtualized:<CMClassImplementationID>" (rating 400, above its software ones).
//   +dec   a decoder instance (,cod the codec type, ,cid the ID): answered with ,err ,ver ,hin.
//   dset dcop dsup   properties: taken; the supported list (dsup) names UsingHardwareAcceleratedVideoDecoder alone, which dcop
//          answers true (esup/ecop: the encoder's, UsingHardwareAcceleratedVideoEncoder).
//   dsta   the session's format (,fds an ImageDescription with avcC or hvcC): a virtio-media session is opened, its OUTPUT queue set
//          up and streaming; the guest is told the pixel buffers it should allocate (Dpba, 420v of the coded size) before the
//          answer. What the device cannot decode (outside its frame sizes, not 4:2:0) is refused here, and VideoToolbox takes its
//          software decoder instead: refused later, frame by frame, it would not.
//   dfrm   a frame (,frm its ID, ,sbf the sample buffer): its NAL units, from length-prefixed to Annex B (the parameter sets
//          before the first), kept for the worker, which puts it in a free OUTPUT buffer with the frame ID as its timestamp;
//          answered at once, asynchronous (,inf 1).
//   dfin   answered by the worker once the frames given so far are handed over (at most two seconds later): a codec keeps
//          frames back (MediaCodec holds even a lone IDR), so the decoder is drained (V4L2_DEC_CMD_STOP, until a LAST buffer
//          or EOS) and started again (V4L2_DEC_CMD_START); dinv tears the V4L2 session down; -dec forgets the instance.
// Nothing here waits, on the guest's thread, for anything the guest must answer first: VideoToolboxParavirtualizationSupport
// holds its lock across the whole send (selector 3), so a wait there for a Dbuf reply the guest cannot send would hang both.
// The session's worker thread takes the device's events: SOURCE_CHANGE sets the CAPTURE queue up (G_FMT, REQBUFS MMAP, every
// buffer queued, STREAMON); each decoded CAPTURE buffer asks the guest for a pixel buffer (Dbuf, the host's request; the guest's
// reply carries the pixel buffer's UUID and its IOSurface), copies the NV12 into that IOSurface, hands it over (Demt) and queues
// the CAPTURE buffer again.
//
// The IOSurface arrives as an ID. An IOSurfaceRootUserClient the kernel opens for the guest process looks it up (the process's
// own refuses surfaces it already holds; an IOUserClient2022's 2022 externalMethod; selector 4, IOSurface.framework's
// IOSurfaceClientLookup: a 0xc68-byte lock result whose fields IOSurfaceClientGet* read, at the client object's offsets less
// 0x70), which maps it into that process and reports where and how its planes lie; the copy goes through that mapping. The
// client is the session's: a lookup of a surface it already holds is refused, and the guest's pool hands the same few surfaces
// out again and again, so what each lookup found is kept by surface ID (the client's reference keeps the surface, and so its
// ID, alive), and closing the client at the session's end gives every reference and mapping back. A frame to encode is the app's
// surface, handed to the codec process over XPC: IOSurfaceRoot lets a client look a surface up only when the client's task made
// it, it is global, or the client knows it already, so it goes as IOSurfaceLookupFromMachPort does, through a send right made by
// the process's own client (which knows it; found by its IOUserClientCreator) and looked up by the kernel's (selectors 0x23,
// 0x22); the right is dropped from the process's space at once.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSString.h>
#include <kern/thread.h>
#include <stdarg.h>
#include "DroidVMVTWire.h"

using namespace vtwire;

extern "C" {
int droidvm_media_open(unsigned device, uint32_t *session);
int droidvm_media_close(unsigned device, uint32_t session);
int droidvm_media_ioctl(unsigned device, uint32_t session, uint32_t code, void *payload, uint32_t out, uint32_t in);
int droidvm_media_mmap(unsigned device, uint32_t session, uint32_t offset, bool writable, uint64_t *gpa, uint64_t *length);
int droidvm_media_munmap(unsigned device, uint64_t gpa);
unsigned droidvm_media_devices(void);
uint32_t droidvm_media_caps(unsigned device);
const char *droidvm_media_card(unsigned device);
bool droidvm_media_is_up(unsigned device);
void droidvm_media_set_sink(unsigned device, void (*sink)(void *, const uint8_t *, uint32_t), void *context);
void droidvm_media_set_fast(unsigned device, bool on);
int proc_selfpid(void);
ipc_space_t get_task_ipcspace(task_t task);
kern_return_t mach_port_deallocate(ipc_space_t space, mach_port_name_t name);

// For the host rig (peek.py): sessions made, frames given, frames emitted, errors; the last error's stage and value.
volatile uint32_t droidvm_vthost_sessions, droidvm_vthost_frames_in, droidvm_vthost_frames_out, droidvm_vthost_errors;
volatile uint32_t droidvm_vthost_error_stage;
volatile int32_t droidvm_vthost_error_value;
volatile uint32_t droidvm_vthost_replies, droidvm_vthost_unknown, droidvm_vthost_last_unknown;
// The host's last diagnostic lines, a ring of 16 (the kernel's message buffer turns over too fast to keep them).
char droidvm_vthost_trace[16][112];
volatile uint32_t droidvm_vthost_trace_count;
}

namespace {

void trace(const char *format, ...) __attribute__((format(printf, 1, 2)));
void trace(const char *format, ...) {
    const uint32_t n = __atomic_fetch_add(&droidvm_vthost_trace_count, 1, __ATOMIC_RELAXED);
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(droidvm_vthost_trace[n % 16], sizeof droidvm_vthost_trace[0], format, arguments);
    va_end(arguments);
}

void failed(uint32_t stage, int32_t value) {
    droidvm_vthost_errors = droidvm_vthost_errors + 1;
    droidvm_vthost_error_stage = stage;
    droidvm_vthost_error_value = value;
    if (droidvm_vthost_errors < 40) IOLog("DroidVMVTHost: stage %u failed: %d\n", stage, value);
}

// --- V4L2 over virtio-media (64-bit Linux layouts) ----------------------------------------------------------------------------
enum : uint32_t {
    IocGFmt = 4, IocSFmt = 5, IocReqbufs = 8, IocQuerybuf = 9, IocQbuf = 15, IocStreamon = 18, IocStreamoff = 19, IocGCtrl = 27,
    IocSubscribeEvent = 90, IocDecoderCmd = 96, IocEnumFramesizes = 74,
    TypeCaptureMplane = 9, TypeOutputMplane = 10, MemoryMmap = 1,
    EventEos = 2, EventSourceChange = 5,
    BufFlagLast = 0x00100000, BufFlagError = 0x40,
    CidMinBuffersForCapture = 0x00980927,
    DecCmdStart = 0, DecCmdStop = 1,
    MediaEvtError = 0, MediaEvtDqbuf = 1, MediaEvtEvent = 2,
};
struct V4l2Buffer {   // struct v4l2_buffer, then one struct v4l2_plane
    uint32_t index, type, bytesused, flags, field, pad0;
    int64_t sec, usec;
    uint8_t timecode[16];
    uint32_t sequence, memory;
    uint64_t m;
    uint32_t length, reserved2, request_fd, pad1;
    struct { uint32_t bytesused, length; uint64_t m; uint32_t data_offset, reserved[11]; } plane;
};
static_assert(sizeof(V4l2Buffer) == 88 + 64, "v4l2_buffer + v4l2_plane");
struct V4l2Format {   // struct v4l2_format with v4l2_pix_format_mplane
    uint32_t type, pad;
    uint32_t width, height, pixelformat, field, colorspace;
    struct { uint32_t sizeimage, bytesperline; uint16_t reserved[6]; } planes[8];
    uint8_t num_planes, flags, ycbcr, quantization, xfer, reserved[7];
    uint8_t rest[200 - 192];
};
static_assert(sizeof(V4l2Format) == 208, "v4l2_format");

constexpr unsigned OutputBuffers = 4, MaxCaptureBuffers = 32;   // Qualcomm's AVC decoder asks 21 output slots at 720p
constexpr uint32_t OutputBytes = 2u << 20;

// --- IOSurface, through the guest process's own IOSurfaceRootUserClient ----------------------------------------------------
struct SurfaceView {
    uint64_t base = 0, size = 0;
    uint32_t id = 0, format = 0, planes = 0;
    struct { uint64_t width, height, offset, bytesPerRow; } plane[2] = {};
};
constexpr uint32_t LockResultBytes = 0xc68;

IOService *surfaceRoot() {
    static IOService *root;
    if (!root) {
        // by class: iPadOS names the service IOCoreSurfaceRoot, macOS leaves it the class's name
        OSDictionary *matching = IOService::serviceMatching("IOSurfaceRoot");
        IOService *found = matching ? IOService::waitForMatchingService(matching, 1000000000ULL) : nullptr;
        OSSafeReleaseNULL(matching);
        if (found && !OSCompareAndSwapPtr(nullptr, found, &root)) found->release();
    }
    return root;
}

// An IOSurfaceRootUserClient of the kernel's own for the guest process `task`, as IOSurface.framework opens one (type 0): the
// process's own client already holds the surfaces it made, and answers a lookup of one with kIOReturnBadArgument (the framework
// asks the kernel only for surfaces it does not know), while a second client of the same task looks any of them up.
IOUserClient *openSurfaceClient(task_t task) {
    IOService *root = surfaceRoot();
    if (!root) return nullptr;
    IOUserClient *client = nullptr;
    const IOReturn r = root->newUserClient(task, task, 0, nullptr, &client);
    trace("surface client: %#x %s", r, client ? client->getMetaClass()->getClassName() : "none");
    if (r != kIOReturnSuccess) { OSSafeReleaseNULL(client); return nullptr; }
    return client;
}

// IOSurface.framework's selectors (IOSurfaceRootUserClient, iPadOS 27).
enum : uint32_t { SurfaceLookup = 4, SurfaceLookupFromPort = 0x22, SurfaceCreatePort = 0x23 };

IOReturn surfaceMethod(IOUserClient *client, uint32_t selector, const uint64_t *input, uint32_t inputCount, uint64_t *output,
                       uint32_t outputCount, void *structure, uint32_t structureSize) {
    IOExternalMethodArguments arguments = {};
    arguments.version = kIOExternalMethodArgumentsCurrentVersion;
    arguments.selector = selector;
    arguments.scalarInput = input;
    arguments.scalarInputCount = inputCount;
    arguments.scalarOutput = output;
    arguments.scalarOutputCount = outputCount;
    arguments.structureOutput = structure;
    arguments.structureOutputSize = structureSize;
    // IOSurfaceRootUserClient is an IOUserClient2022, whose legacy externalMethod panics; a client the kernel opened itself has
    // not been through is_io_service_open_extended, which tells callExternalMethod so, hence the 2022 method, directly.
    IOUserClient2022 *client2022 = OSDynamicCast(IOUserClient2022, client);
    return client2022 ? client2022->externalMethod(selector, reinterpret_cast<IOExternalMethodArgumentsOpaque *>(&arguments))
                      : client->callExternalMethod(selector, &arguments);
}

// The IOSurfaceRootUserClient the process `pid` opened itself (IOSurface.framework's), retained: the one that holds the surfaces
// the process was given by another (a frame to encode comes from the app, through XPC), which IOSurfaceRoot lets a client look
// up only when the client knows the surface already, it is global, or the client's task made it.
IOUserClient *processSurfaceClient(int pid) {
    IOService *root = surfaceRoot();
    OSIterator *clients = root ? root->getClientIterator() : nullptr;
    if (!clients) return nullptr;
    char prefix[24];
    snprintf(prefix, sizeof prefix, "pid %d,", pid);
    IOUserClient *found = nullptr;
    while (OSObject *object = clients->getNextObject()) {
        IOUserClient *client = OSDynamicCast(IOUserClient, object);
        OSString *creator = client ? OSDynamicCast(OSString, client->getProperty(kIOUserClientCreatorKey)) : nullptr;
        if (creator && !strncmp(creator->getCStringNoCopy(), prefix, strlen(prefix))) {
            found = client;
            found->retain();
            break;
        }
    }
    clients->release();
    return found;
}

bool lookupSurface(IOUserClient *client, uint32_t id, SurfaceView &view, IOUserClient *holder = nullptr) {
    uint8_t *result = static_cast<uint8_t *>(IOMallocZero(LockResultBytes));
    if (!result) return false;
    IOReturn status;
    if (holder) {
        // A surface this process was given: a send right to it made by the client that holds it, in the process (whose thread
        // this is), and looked up through that, as IOSurfaceLookupFromMachPort does; the right is the process's to drop.
        const uint64_t create[2] = {id, 0};
        uint64_t port = 0;
        status = surfaceMethod(holder, SurfaceCreatePort, create, 2, &port, 1, nullptr, 0);
        if (status == kIOReturnSuccess) {
            status = surfaceMethod(client, SurfaceLookupFromPort, &port, 1, nullptr, 0, result, LockResultBytes);
            mach_port_deallocate(get_task_ipcspace(current_task()), mach_port_name_t(port));
        } else {
            failed(42, status);
        }
    } else {
        const uint64_t input = id;
        status = surfaceMethod(client, SurfaceLookup, &input, 1, nullptr, 0, result, LockResultBytes);
    }
    bool good = status == kIOReturnSuccess;
    if (good) {
        auto u64 = [&](uint32_t at) { uint64_t v; memcpy(&v, result + at, 8); return v; };
        auto u32 = [&](uint32_t at) { uint32_t v; memcpy(&v, result + at, 4); return v; };
        view.base = u64(0x00);
        view.id = u32(0x18);
        view.size = u64(0x20);
        view.format = u32(0x48);
        view.planes = u32(0x58);
        for (uint32_t p = 0; p < 2 && p < view.planes; ++p) {
            const uint32_t at = 0x88 + p * 0x98;
            view.plane[p] = {u64(at), u64(at + 0x08), u64(at + 0x18), u64(at + 0x20)};
        }
        good = view.id == id && view.base && view.size && view.planes == 2;
        if (!good) failed(40, int32_t(view.planes));
    } else {
        failed(41, status);
    }
    trace("lookup %u: %#x planes %u base %#llx size %#llx", id, status, view.planes, view.base, view.size);
    IOFree(result, LockResultBytes);
    return good;
}


// --- Sessions ---------------------------------------------------------------------------------------------------------------
struct Client;

struct Mapped {
    uint64_t gpa = 0, length = 0;
    IOMemoryDescriptor *memory = nullptr;
    IOMemoryMap *map = nullptr;
    uint8_t *bytes = nullptr;
    bool queued = false;
};

struct Event {
    uint32_t kind, type, index, bytesused, flags;
    int64_t sec;
};

struct Session {
    Session *next = nullptr;
    Client *client = nullptr;
    uint8_t uuid[16] = {};
    uint32_t codec = 0, width = 0, height = 0;
    bool encoder = false;
    uint32_t nalLength = 4;
    uint8_t parameterSets[512] = {};
    uint32_t parameterSetsSize = 0;
    bool parametersSent = false;

    unsigned device = ~0u;
    uint32_t v4l2 = 0;
    bool open = false, outputStreaming = false, captureStreaming = false, fast = false;
    Mapped output[OutputBuffers];
    Mapped capture[MaxCaptureBuffers];
    unsigned captureCount = 0;
    uint32_t captureWidth = 0, captureHeight = 0, captureStride = 0;

    IOLock *lock = nullptr;
    // Frames given (dfrm) and not yet in an OUTPUT buffer: Annex B, IOMalloc'd.
    struct Input { int64_t frame; uint8_t *bytes; uint32_t size, capacity; } inputs[128];
    unsigned inputHead = 0, inputTail = 0;
    // An outstanding dfin: its request, answered when pendingFrames is 0 or at finishDeadline.
    bool finishWanted = false, draining = false;
    uint64_t finishRequest = 0;
    uint64_t finishDeadline = 0;
    Event events[64];
    unsigned eventHead = 0, eventTail = 0;
    bool stopping = false, workerRunning = false;
    thread_t worker = nullptr;
    uint32_t pendingFrames = 0;
    // The frames given and not yet handed over, by ID: at dfin's deadline each gets a Demt with an error, or VideoToolbox would
    // wait for it forever.
    int64_t outstanding[64];
    unsigned outstandingCount = 0;

    // The session's IOSurfaceRootUserClient (opened at its first pixel buffer) and the surfaces it looked up, by ID.
    IOUserClient *surfaces = nullptr;
    IOUserClient *holder = nullptr;   // the process's own client (an encoder's frames are the app's)
    IOLock *surfaceLock = nullptr;
    struct { uint32_t id; SurfaceView view; } known[48];
    unsigned knownCount = 0;

    // An encoder's frames (efrm), in the order given: the frame ID, its pixel buffer's UUID, its times, its IOSurface; fed once
    // copied into an OUTPUT buffer, forgotten once handed over (Eemt).
    struct Frame {
        int64_t frame;
        uint8_t uuid[16];
        uint8_t pts[24], duration[24];   // CMTime, as the 'para' carried it (big endian)
        SurfaceView view;
        bool fed;
    } frames[64];
    unsigned frameHead = 0, frameTail = 0;
    // The stream's parameter sets (Annex B NAL units, without start codes) and whether the guest has the description made of them.
    uint8_t sps[256] = {}, pps[256] = {}, vps[256] = {};
    uint32_t spsSize = 0, ppsSize = 0, vpsSize = 0;
    bool describedToGuest = false;

    // The host's own requests to the guest (Dbuf, Demt) and the guest's replies to them.
    uint64_t nextHostRequest = 100;
    uint64_t awaitedRequest = 0;
    bool replied = false;
    int32_t replyError = 0;
    uint8_t replyUuid[16] = {};
    bool replyHasSurface = false;
    SurfaceView replySurface;
};

struct Client {
    void *transport;
    bool (*send)(void *transport, const uint8_t *bytes, uint32_t size);
    task_t task;
    int pid;
    IOLock *lock;
    Session *sessions;
};

// Every session the device's events may name, by V4L2 session ID.
IOLock *gTableLock;
Session *gTable[32];

void registerSession(Session *s) {
    IOLockLock(gTableLock);
    for (Session *&slot : gTable) if (!slot) { slot = s; break; }
    IOLockUnlock(gTableLock);
}
void unregisterSession(Session *s) {
    IOLockLock(gTableLock);
    for (Session *&slot : gTable) if (slot == s) slot = nullptr;
    IOLockUnlock(gTableLock);
}

// The device's events (with its lock held): copied into their session's ring.
void mediaSink(void *context, const uint8_t *event, uint32_t size) {
    const unsigned device = unsigned(uintptr_t(context));
    if (size < 8) return;
    uint32_t kind, session;
    memcpy(&kind, event, 4);
    memcpy(&session, event + 4, 4);
    Event e = {kind, 0, 0, 0, 0, 0};
    if (kind == MediaEvtDqbuf && size >= 8 + sizeof(V4l2Buffer)) {
        V4l2Buffer b;
        memcpy(&b, event + 8, 88);
        memcpy(&b.plane, event + 8 + 88, sizeof b.plane);
        e.type = b.type; e.index = b.index; e.bytesused = b.plane.bytesused; e.flags = b.flags; e.sec = b.sec;
    } else if (kind == MediaEvtEvent && size >= 12) {
        memcpy(&e.type, event + 8, 4);
    }
    IOLockLock(gTableLock);
    for (Session *s : gTable) {
        if (!s || !s->open || s->device != device || s->v4l2 != session) continue;
        IOLockLock(s->lock);
        if (s->eventHead - s->eventTail < 64) s->events[s->eventHead++ % 64] = e;
        IOLockUnlock(s->lock);
        IOLockWakeup(s->lock, &s->events, false);
    }
    IOLockUnlock(gTableLock);
}

unsigned decoderDevice() {
    static unsigned device = ~0u;
    if (device != ~0u) return device;
    for (unsigned d = 0; d < droidvm_media_devices(); ++d) {
        if (!droidvm_media_is_up(d) || !strnstr(droidvm_media_card(d), "decoder", 33)) continue;
        droidvm_media_set_sink(d, mediaSink, reinterpret_cast<void *>(uintptr_t(d)));
        device = d;
        break;
    }
    return device;
}

unsigned encoderDevice() {
    static unsigned device = ~0u;
    if (device != ~0u) return device;
    for (unsigned d = 0; d < droidvm_media_devices(); ++d) {
        if (!droidvm_media_is_up(d) || !strnstr(droidvm_media_card(d), "encoder", 33)) continue;
        droidvm_media_set_sink(d, mediaSink, reinterpret_cast<void *>(uintptr_t(d)));
        device = d;
        break;
    }
    return device;
}

int ioctl(Session *s, uint32_t code, void *payload, uint32_t out, uint32_t in) {
    return droidvm_media_ioctl(s->device, s->v4l2, code, payload, out, in);
}

bool mapBuffer(Session *s, uint32_t type, uint32_t index, Mapped &m) {
    V4l2Buffer b = {};
    b.index = index; b.type = type; b.memory = MemoryMmap; b.length = 1;
    int r = ioctl(s, IocQuerybuf, &b, sizeof b, sizeof b);
    if (r) { failed(10, r); return false; }
    uint64_t gpa = 0, length = 0;
    r = droidvm_media_mmap(s->device, s->v4l2, uint32_t(b.plane.m), true, &gpa, &length);
    if (r) { failed(11, r); return false; }
    m.gpa = gpa;
    m.length = length;
    m.memory = IOMemoryDescriptor::withPhysicalAddress(gpa, length, kIODirectionInOut);
    m.map = m.memory ? m.memory->map(kIOMapCopybackCache) : nullptr;
    m.bytes = m.map ? reinterpret_cast<uint8_t *>(m.map->getVirtualAddress()) : nullptr;
    if (!m.bytes) { failed(12, 0); return false; }
    return true;
}

void unmapBuffer(Session *s, Mapped &m) {
    OSSafeReleaseNULL(m.map);
    OSSafeReleaseNULL(m.memory);
    if (m.gpa) droidvm_media_munmap(s->device, m.gpa);
    m = Mapped();
}

// The parameter sets of an avcC or hvcC, as Annex B, and the NAL length size.
bool parameterSets(Session *s, const ImageDescription &d) {
    Atom ext;
    const uint8_t start[4] = {0, 0, 0, 1};
    s->parameterSetsSize = 0;
    auto add = [&](const uint8_t *nal, uint32_t n) {
        if (s->parameterSetsSize + 4 + n > sizeof s->parameterSets) return false;
        memcpy(s->parameterSets + s->parameterSetsSize, start, 4);
        memcpy(s->parameterSets + s->parameterSetsSize + 4, nal, n);
        s->parameterSetsSize += 4 + n;
        return true;
    };
    if (find(d.extensions, d.extensionsSize, cc("avcC"), ext)) {
        const uint8_t *p = ext.body, *end = ext.body + ext.size;
        if (ext.size < 7) return false;
        s->nalLength = (p[4] & 3) + 1;
        unsigned count = p[5] & 0x1f;
        p += 6;
        for (int set = 0; set < 2; ++set) {
            for (unsigned i = 0; i < count; ++i) {
                if (end - p < 2) return false;
                const uint32_t n = be16(p);
                if (uint32_t(end - p) < 2 + n || !add(p + 2, n)) return false;
                p += 2 + n;
            }
            if (set == 0) { if (p >= end) return false; count = *p++; }
        }
        return true;
    }
    if (find(d.extensions, d.extensionsSize, cc("hvcC"), ext)) {
        const uint8_t *p = ext.body, *end = ext.body + ext.size;
        if (ext.size < 23) return false;
        s->nalLength = (p[21] & 3) + 1;
        const unsigned arrays = p[22];
        p += 23;
        for (unsigned a = 0; a < arrays; ++a) {
            if (end - p < 3) return false;
            const unsigned count = be16(p + 1);
            p += 3;
            for (unsigned i = 0; i < count; ++i) {
                if (end - p < 2) return false;
                const uint32_t n = be16(p);
                if (uint32_t(end - p) < 2 + n || !add(p + 2, n)) return false;
                p += 2 + n;
            }
        }
        return true;
    }
    return false;
}

// --- Messages to the guest -----------------------------------------------------------------------------------------------
bool sendToGuest(Client *c, const Writer &w, const uint8_t *buffer) {
    if (!w.ok()) { failed(20, int32_t(w.size())); return false; }
    droidvm_vthost_replies = droidvm_vthost_replies + 1;
    return c->send(c->transport, buffer, w.size());
}

void replyStatus(Client *c, const Header &h, int32_t error) {
    uint8_t buffer[128];
    Writer w(buffer, sizeof buffer);
    w.header(h.type, FlagReply, h.uuid, h.request);
    w.s32(cc(",err"), error);
    sendToGuest(c, w, buffer);
}

// A request of the host's (Dbuf, Demt) and the guest's reply to it (up to two seconds).
bool hostRequest(Session *s, Writer &w, const uint8_t *buffer, uint64_t request) {
    IOLockLock(s->lock);
    s->awaitedRequest = request;
    s->replied = false;
    s->replyHasSurface = false;
    IOLockUnlock(s->lock);
    if (!sendToGuest(s->client, w, buffer)) return false;
    IOLockLock(s->lock);
    AbsoluteTime deadline;
    clock_interval_to_deadline(2000, kMillisecondScale, &deadline);
    while (!s->replied && !s->stopping)
        if (IOLockSleepDeadline(s->lock, &s->replied, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) break;
    const bool replied = s->replied;
    s->awaitedRequest = 0;
    IOLockUnlock(s->lock);
    if (!replied) failed(30, int32_t(request));
    return replied;
}

// --- The worker: the device's events ---------------------------------------------------------------------------------------
bool setUpCapture(Session *s) {
    V4l2Format f = {};
    f.type = TypeCaptureMplane;
    int r = ioctl(s, IocGFmt, &f, sizeof f, sizeof f);
    if (r) { failed(50, r); return false; }
    s->captureWidth = f.width;
    s->captureHeight = f.height;
    s->captureStride = f.planes[0].bytesperline ? f.planes[0].bytesperline : f.width;
    uint32_t control[2] = {CidMinBuffersForCapture, 0};
    unsigned count = ioctl(s, IocGCtrl, control, sizeof control, sizeof control) ? 8 : control[1] + 2;
    if (count > MaxCaptureBuffers) count = MaxCaptureBuffers;
    uint32_t request[5] = {count, TypeCaptureMplane, MemoryMmap, 0, 0};
    r = ioctl(s, IocReqbufs, request, sizeof request, sizeof request);
    if (r || !request[0]) { failed(51, r); return false; }
    s->captureCount = request[0] < MaxCaptureBuffers ? request[0] : MaxCaptureBuffers;
    for (unsigned i = 0; i < s->captureCount; ++i) {
        if (!mapBuffer(s, TypeCaptureMplane, i, s->capture[i])) return false;
        V4l2Buffer b = {};
        b.index = i; b.type = TypeCaptureMplane; b.memory = MemoryMmap; b.length = 1;
        b.plane.length = uint32_t(s->capture[i].length);
        r = ioctl(s, IocQbuf, &b, sizeof b, sizeof b);
        if (r) { failed(52, r); return false; }
        s->capture[i].queued = true;
    }
    uint32_t type = TypeCaptureMplane;
    r = ioctl(s, IocStreamon, &type, 4, 0);
    if (r) { failed(53, r); return false; }
    s->captureStreaming = true;
    IOLog("DroidVMVTHost: capture %ux%u stride %u, %u buffers\n", s->captureWidth, s->captureHeight, s->captureStride, s->captureCount);
    return true;
}

void copyFrame(Session *s, const Mapped &from, const SurfaceView &to) {
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(to.base, to.size, kIODirectionInOut, s->client->task);
    if (!memory || memory->prepare() != kIOReturnSuccess) { OSSafeReleaseNULL(memory); failed(60, 0); return; }
    IOMemoryMap *map = memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere);
    uint8_t *surface = map ? reinterpret_cast<uint8_t *>(map->getVirtualAddress()) : nullptr;
    if (surface) {
        const uint32_t width = s->captureWidth < to.plane[0].width ? s->captureWidth : uint32_t(to.plane[0].width);
        const uint32_t height = s->captureHeight < to.plane[0].height ? s->captureHeight : uint32_t(to.plane[0].height);
        const uint8_t *luma = from.bytes, *chroma = from.bytes + uint64_t(s->captureStride) * s->captureHeight;
        for (uint32_t y = 0; y < height; ++y)
            memcpy(surface + to.plane[0].offset + y * to.plane[0].bytesPerRow, luma + uint64_t(y) * s->captureStride, width);
        for (uint32_t y = 0; y < (height + 1) / 2 && y < to.plane[1].height; ++y)
            memcpy(surface + to.plane[1].offset + y * to.plane[1].bytesPerRow, chroma + uint64_t(y) * s->captureStride,
                   width < to.plane[1].bytesPerRow ? width : uint32_t(to.plane[1].bytesPerRow));
    } else {
        failed(61, 0);
    }
    OSSafeReleaseNULL(map);
    memory->complete();
    memory->release();
}

void emitFrame(Session *s, unsigned index, int64_t frame, uint32_t bytesused) {
    Mapped &buffer = s->capture[index];
    uint8_t message[256];
    // Dbuf: a pixel buffer for the frame.
    const uint64_t request = s->nextHostRequest++;
    Writer w(message, sizeof message);
    w.header(cc("Dbuf"), FlagHostRequest, s->uuid, request);
    w.s64(cc(",frm"), frame);
    if (!hostRequest(s, w, message, request) || s->replyError || !s->replyHasSurface) {
        failed(31, s->replyError);
        return;
    }
    uint8_t pixelBuffer[16];
    memcpy(pixelBuffer, s->replyUuid, 16);
    const SurfaceView surface = s->replySurface;
    if (bytesused) copyFrame(s, buffer, surface);
    // Demt: the frame, decoded into that pixel buffer.
    const uint64_t emit = s->nextHostRequest++;
    Writer e(message, sizeof message);
    e.header(cc("Demt"), FlagHostRequest, s->uuid, emit);
    e.s64(cc(",frm"), frame);
    e.s32(cc(",err"), bytesused ? 0 : -12911);
    e.u32(cc(",inf"), 1);
    e.uuid(cc(",uui"), pixelBuffer);
    if (hostRequest(s, e, message, emit)) droidvm_vthost_frames_out = droidvm_vthost_frames_out + 1;
}

void forgetFrame(Session *s, int64_t frame) {
    IOLockLock(s->lock);
    for (unsigned i = 0; i < s->outstandingCount; ++i)
        if (s->outstanding[i] == frame) { s->outstanding[i] = s->outstanding[--s->outstandingCount]; break; }
    IOLockUnlock(s->lock);
}

// The frames that never came out (a codec that dropped them, a decoder too small for the stream): each handed over failed.
void dropOutstanding(Session *s) {
    for (;;) {
        IOLockLock(s->lock);
        if (!s->outstandingCount) { IOLockUnlock(s->lock); return; }
        const int64_t frame = s->outstanding[--s->outstandingCount];
        IOLockUnlock(s->lock);
        uint8_t message[160];
        const uint64_t request = s->nextHostRequest++;
        Writer e(message, sizeof message);
        e.header(cc("Demt"), FlagHostRequest, s->uuid, request);
        e.s64(cc(",frm"), frame);
        e.s32(cc(",err"), -12911);
        e.u32(cc(",inf"), 2);   // kVTDecodeInfo_FrameDropped
        hostRequest(s, e, message, request);
    }
}

void requeueCapture(Session *s, unsigned index) {
    V4l2Buffer b = {};
    b.index = index; b.type = TypeCaptureMplane; b.memory = MemoryMmap; b.length = 1;
    b.plane.length = uint32_t(s->capture[index].length);
    const int r = ioctl(s, IocQbuf, &b, sizeof b, sizeof b);
    if (r) failed(54, r);
    else s->capture[index].queued = true;
}

// The oldest kept frame into a free OUTPUT buffer, if both exist (the lock held, and released while the buffer is filled).
bool feedInput(Session *s) {
    unsigned index = OutputBuffers;
    for (unsigned i = 0; i < OutputBuffers; ++i) if (!s->output[i].queued) { index = i; break; }
    if (index == OutputBuffers || s->inputHead == s->inputTail) return false;
    const Session::Input input = s->inputs[s->inputTail++ % 128];
    s->output[index].queued = true;
    IOLockUnlock(s->lock);
    Mapped &out = s->output[index];
    const uint32_t size = input.size < out.length ? input.size : uint32_t(out.length);
    memcpy(out.bytes, input.bytes, size);
    IOFree(input.bytes, input.capacity);
    V4l2Buffer b = {};
    b.index = index; b.type = TypeOutputMplane; b.memory = MemoryMmap; b.length = 1;
    b.sec = input.frame;
    b.plane.bytesused = size;
    b.plane.length = uint32_t(out.length);
    const int r = ioctl(s, IocQbuf, &b, sizeof b, sizeof b);
    IOLockLock(s->lock);
    if (r) {
        failed(8, r);
        out.queued = false;
        if (s->pendingFrames) --s->pendingFrames;
    }
    return true;
}

// dfin's answer, once nothing given is still on its way (or at its deadline). The lock held.
int decoderCommand(Session *s, uint32_t command) {
    uint32_t cmd[18] = {command};
    return ioctl(s, IocDecoderCmd, cmd, sizeof cmd, sizeof cmd);
}

void answerFinish(Session *s) {
    if (!s->finishWanted) return;
    // Every given frame fed and some still inside the codec: drain it (once per dfin).
    if (s->pendingFrames && !s->draining && s->inputHead == s->inputTail && mach_absolute_time() < s->finishDeadline) {
        s->draining = true;
        IOLockUnlock(s->lock);
        const int r = decoderCommand(s, DecCmdStop);
        IOLockLock(s->lock);
        if (r) { failed(55, r); s->draining = false; }
        return;
    }
    if (s->pendingFrames && mach_absolute_time() < s->finishDeadline) return;
    if (s->outstandingCount) {
        IOLockUnlock(s->lock);
        dropOutstanding(s);
        IOLockLock(s->lock);
        s->pendingFrames = 0;
    }
    Header h;
    h.type = cc("dfin");
    memcpy(h.uuid, s->uuid, 16);
    h.request = s->finishRequest;
    s->finishWanted = false;
    IOLockUnlock(s->lock);
    replyStatus(s->client, h, 0);
    IOLockLock(s->lock);
}

// A drain's end (its LAST buffer, or EOS): the decoder takes frames again.
void endDrain(Session *s) {
    IOLockLock(s->lock);
    const bool draining = s->draining;
    s->draining = false;
    if (draining && s->pendingFrames) s->pendingFrames = 0;   // what the drain did not bring out never will
    IOLockUnlock(s->lock);
    if (draining) {
        const int r = decoderCommand(s, DecCmdStart);
        if (r) failed(56, r);
    }
}

void workerMain(void *argument, wait_result_t) {
    Session *s = static_cast<Session *>(argument);
    for (;;) {
        IOLockLock(s->lock);
        for (;;) {
            if (s->stopping || s->eventHead != s->eventTail) break;
            if (feedInput(s)) continue;
            answerFinish(s);
            if (s->stopping || s->eventHead != s->eventTail) break;
            if (s->finishWanted) IOLockSleepDeadline(s->lock, &s->events, s->finishDeadline, THREAD_UNINT);
            else IOLockSleep(s->lock, &s->events, THREAD_UNINT);
        }
        if (s->stopping) break;
        const Event e = s->events[s->eventTail++ % 64];
        IOLockUnlock(s->lock);
        if (e.kind == MediaEvtEvent && e.type == EventSourceChange) {
            if (!s->captureStreaming) setUpCapture(s);
        } else if (e.kind == MediaEvtDqbuf && e.type == TypeOutputMplane && e.index < OutputBuffers) {
            IOLockLock(s->lock);
            s->output[e.index].queued = false;
            IOLockUnlock(s->lock);
        } else if (e.kind == MediaEvtDqbuf && e.type == TypeCaptureMplane && e.index < s->captureCount) {
            s->capture[e.index].queued = false;
            if (e.bytesused && !(e.flags & BufFlagError)) {
                emitFrame(s, e.index, e.sec, e.bytesused);
                forgetFrame(s, e.sec);
                IOLockLock(s->lock);
                if (s->pendingFrames) --s->pendingFrames;
                IOLockUnlock(s->lock);
            }
            if (e.flags & BufFlagLast) endDrain(s);
            requeueCapture(s, e.index);
        } else if (e.kind == MediaEvtEvent && e.type == EventEos) {
            endDrain(s);
        } else if (e.kind == MediaEvtError) {
            failed(70, 0);
        }
    }
    s->workerRunning = false;
    IOLockUnlock(s->lock);
    IOLockWakeup(s->lock, &s->workerRunning, false);
    thread_terminate(current_thread());
}

// --- The encoder ----------------------------------------------------------------------------------------------------------
// The exchange VideoToolbox's ParavirtualizedVideoEncoder has with a host (recorded by replaying a guest's messages through
// macOS's host session, tools/vtpv-oracle): +enc, eset/emul, esta (,vdi the size; the host sends Epba, the pixel buffers it
// takes), esup, epfr, efrm (,frm ,pts ,dur ,uui and the frame's IOSurface; answered at once, asynchronous), then for each coded
// frame Efds when the stream's parameter sets are new (,cod ,wdt ,hgt ,ext with the avcC/hvcC: the guest makes the description
// and answers with it) and Eemt (,frm ,err ,efl ,uui and ,sbf the sample buffer: atoms sbuf{opts stia sdat nsmp ssiz fdsc{mdia
// vdim codc extn{dict}} sary{aray{dict}}}, little endian inside, keys as strings or indices; a message to the guest is at most
// 16 KiB, so a larger one goes in pieces flagged 0x10, the last with the request), ecfr (answered once every frame given is
// handed over), einv, -enc. Over virtio-media's stateful encoder: OUTPUT tightly packed NV12, CAPTURE the bitstream (Annex B,
// the parameter sets alone in a buffer of their own or before a key frame), V4L2_ENC_CMD_STOP to drain.
void teardown(Session *s);

enum : uint32_t { IocEncoderCmd = 77, EncCmdStart = 0, EncCmdStop = 1, BufFlagKeyframe = 0x8 };
constexpr uint32_t MaxGuestMessage = 16384;

// Atoms written as FigAtomWriter does: 32-bit big-endian size, type, body.
class AtomOut {
public:
    AtomOut(uint8_t *buffer, uint32_t capacity) : out(buffer), cap(capacity) {}
    uint32_t begin(uint32_t type) { const uint32_t at = len; uint8_t h[8] = {}; put32(h + 4, type); bytes(h, 8); return at; }
    void end(uint32_t at) { if (good) put32(out + at, len - at); }
    void leaf(uint32_t type, const void *data, uint32_t n) { const uint32_t at = begin(type); bytes(data, n); end(at); }
    void bytes(const void *data, uint32_t n) {
        if (!good || len + n > cap) { good = false; return; }
        memcpy(out + len, data, n);
        len += n;
    }
    bool ok() const { return good; }
    uint32_t size() const { return len; }
private:
    uint8_t *out;
    uint32_t cap, len = 0;
    bool good = true;
};

// A property list of one object (a bool, an ASCII string or data), as the atom leaves carry them.
uint32_t tinyPlist(uint8_t *out, uint32_t capacity, char kind, const void *data, uint32_t n) {
    uint32_t at = 0;
    auto put = [&](const void *src, uint32_t count) { if (at + count <= capacity) memcpy(out + at, src, count); at += count; };
    auto byte = [&](uint8_t b) { put(&b, 1); };
    put("bplist00", 8);
    if (kind == 'b') byte(n ? 0x09 : 0x08);
    else {
        const uint8_t high = kind == 's' ? 0x50 : 0x40;
        if (n < 15) byte(uint8_t(high | n));
        else if (n < 256) { byte(uint8_t(high | 0x0f)); byte(0x10); byte(uint8_t(n)); }
        else { byte(uint8_t(high | 0x0f)); byte(0x11); byte(uint8_t(n >> 8)); byte(uint8_t(n)); }
        put(data, n);
    }
    const uint32_t table = at;
    byte(8);   // the one object's offset
    uint8_t trailer[32] = {};
    trailer[6] = 1;
    trailer[7] = 1;
    put64(trailer + 8, 1);
    put64(trailer + 24, table);
    put(trailer, 32);
    return at <= capacity ? at : 0;
}

void plistLeaf(AtomOut &a, uint32_t type, char kind, const void *data, uint32_t n) {
    uint8_t buffer[512];
    const uint32_t size = tinyPlist(buffer, sizeof buffer, kind, data, n);
    if (size) a.leaf(type, buffer, size);
    else a.leaf(type, nullptr, 0);
}
void stringKey(AtomOut &a, const char *key) { plistLeaf(a, cc("strk"), 's', key, uint32_t(strlen(key))); }

// A CMTime of a 'para' (value, timescale, flags, epoch; big endian) as CMTime's own memory (little endian).
void cmTimeLittle(const uint8_t *big, uint8_t *little) {
    const uint64_t value = be64(big), epoch = be64(big + 16);
    const uint32_t scale = be32(big + 8), flags = be32(big + 12);
    memcpy(little, &value, 8);
    memcpy(little + 8, &scale, 4);
    memcpy(little + 12, &flags, 4);
    memcpy(little + 16, &epoch, 8);
}

bool hevcSession(const Session *s) { return s->codec == cc("hvc1") || s->codec == cc("hev1"); }

// The avcC or hvcC of the session's parameter sets.
uint32_t configurationRecord(const Session *s, uint8_t *out, uint32_t capacity) {
    if (!hevcSession(s)) {
        if (s->spsSize < 4 || !s->ppsSize || 11 + s->spsSize + s->ppsSize > capacity) return 0;
        uint32_t at = 0;
        out[at++] = 1; out[at++] = s->sps[1]; out[at++] = s->sps[2]; out[at++] = s->sps[3];
        out[at++] = 0xff;   // 4-byte NAL lengths
        out[at++] = 0xe1;   // one SPS
        out[at++] = uint8_t(s->spsSize >> 8); out[at++] = uint8_t(s->spsSize);
        memcpy(out + at, s->sps, s->spsSize); at += s->spsSize;
        out[at++] = 1;
        out[at++] = uint8_t(s->ppsSize >> 8); out[at++] = uint8_t(s->ppsSize);
        memcpy(out + at, s->pps, s->ppsSize); at += s->ppsSize;
        return at;
    }
    // hvcC: the general profile, tier, level and their flags straight from the SPS's profile_tier_level (bytes 3-14 after the
    // two-byte NAL header and the byte of IDs), 4:2:0 8-bit, 4-byte NAL lengths, then the VPS, SPS and PPS arrays.
    if (s->spsSize < 15 || !s->vpsSize || !s->ppsSize || 23 + 15 + s->vpsSize + s->spsSize + s->ppsSize > capacity) return 0;
    uint32_t at = 0;
    out[at++] = 1;
    memcpy(out + at, s->sps + 3, 12); at += 12;   // profile space/tier/idc, compatibility flags, constraint flags, level
    out[at++] = 0xf0; out[at++] = 0x00;            // min_spatial_segmentation_idc 0
    out[at++] = 0xfc;                              // parallelismType 0
    out[at++] = 0xfd;                              // chroma_format_idc 1
    out[at++] = 0xf8;                              // bit_depth_luma 8
    out[at++] = 0xf8;                              // bit_depth_chroma 8
    out[at++] = 0; out[at++] = 0;                  // avgFrameRate 0
    out[at++] = 0x0f;                              // constantFrameRate 0, numTemporalLayers 1, temporalIdNested 1, length 4
    out[at++] = 3;
    const struct { uint8_t type; const uint8_t *nal; uint32_t size; } arrays[3] = {
        {32, s->vps, s->vpsSize}, {33, s->sps, s->spsSize}, {34, s->pps, s->ppsSize}};
    for (const auto &a : arrays) {
        out[at++] = uint8_t(0x80 | a.type);
        out[at++] = 0; out[at++] = 1;
        out[at++] = uint8_t(a.size >> 8); out[at++] = uint8_t(a.size);
        memcpy(out + at, a.nal, a.size); at += a.size;
    }
    return at;
}

// Efds: the guest makes the stream's format description (and answers with it, which this host does not need).
bool describeToGuest(Session *s) {
    uint8_t record[1024];
    const uint32_t recordSize = configurationRecord(s, record, sizeof record);
    if (!recordSize) { failed(80, int32_t(s->spsSize)); return false; }
    Plist *p = new Plist();
    uint8_t *message = static_cast<uint8_t *>(IOMallocZero(4096));
    bool sent = false;
    if (p && message) {
        const int atomKeys[1] = {p->string(hevcSession(s) ? "hvcC" : "avcC")};
        const int atomValues[1] = {p->data(record, recordSize)};
        const int keys[2] = {p->string("FormatName"), p->string("SampleDescriptionExtensionAtoms")};
        const int values[2] = {p->string(hevcSession(s) ? "HEVC" : "H.264"), p->dict(atomKeys, atomValues, 1)};
        const int extensions = p->dict(keys, values, 2);
        const uint64_t request = s->nextHostRequest++;
        Writer w(message, 4096);
        w.header(cc("Efds"), FlagHostRequest, s->uuid, request);
        w.s32(cc(",cod"), int32_t(s->codec));
        w.s32(cc(",wdt"), int32_t(s->width));
        w.s32(cc(",hgt"), int32_t(s->height));
        w.plist(cc(",ext"), *p, extensions);
        sent = hostRequest(s, w, message, request) && !s->replyError;
        if (!sent) failed(81, s->replyError);
    }
    delete p;
    if (message) IOFree(message, 4096);
    return sent;
}

// Eemt: one coded frame (length-prefixed NAL units), or a frame dropped (data null).
void emitEncoded(Session *s, const Session::Frame &f, const uint8_t *data, uint32_t size, bool key, int32_t error) {
    const uint32_t capacity = size + 4096;
    uint8_t *sbuf = static_cast<uint8_t *>(IOMalloc(capacity));
    uint8_t *message = static_cast<uint8_t *>(IOMalloc(MaxGuestMessage));
    if (!sbuf || !message) { if (sbuf) IOFree(sbuf, capacity); if (message) IOFree(message, MaxGuestMessage); return; }
    uint32_t sbufSize = 0;
    if (data) {
        AtomOut a(sbuf, capacity);
        const uint32_t top = a.begin(cc("sbuf"));
        uint8_t pts[24], duration[24], timing[72] = {};
        cmTimeLittle(f.pts, pts);
        cmTimeLittle(f.duration, duration);
        a.leaf(cc("opts"), pts, 24);
        memcpy(timing, duration, 24);
        memcpy(timing + 24, pts, 24);   // decode time: invalid (no reordering)
        a.leaf(cc("stia"), timing, 72);
        a.leaf(cc("sdat"), data, size);
        const uint32_t one = 1;
        a.leaf(cc("nsmp"), &one, 4);
        a.leaf(cc("ssiz"), &size, 4);
        const uint32_t fdsc = a.begin(cc("fdsc"));
        const uint32_t media = cc("vide"), codec = s->codec;
        const int32_t dimensions[2] = {int32_t(s->width), int32_t(s->height)};
        a.leaf(cc("mdia"), &media, 4);   // little endian, as CoreMedia keeps a FourCC
        a.leaf(cc("vdim"), dimensions, 8);
        a.leaf(cc("codc"), &codec, 4);
        uint8_t record[1024];
        const uint32_t recordSize = configurationRecord(s, record, sizeof record);
        const uint32_t extn = a.begin(cc("extn"));
        const uint32_t dict = a.begin(cc("dict"));
        const uint32_t entry = a.begin(cc("keyv"));
        stringKey(a, "SampleDescriptionExtensionAtoms");
        const uint32_t atoms = a.begin(cc("dict"));
        const uint32_t atomEntry = a.begin(cc("keyv"));
        stringKey(a, hevcSession(s) ? "hvcC" : "avcC");
        plistLeaf(a, cc("datv"), 'd', record, recordSize);
        a.end(atomEntry);
        a.end(atoms);
        a.end(entry);
        a.end(dict);
        a.end(extn);
        a.end(fdsc);
        const uint32_t sary = a.begin(cc("sary"));
        const uint32_t array = a.begin(cc("aray"));
        // A sync sample has no NotSync at all (WebRTC and others test for the key, not its value).
        const uint32_t attachments = a.begin(cc("dict"));
        if (!key) {
            const uint32_t notSync = a.begin(cc("keyv"));
            stringKey(a, "NotSync");
            plistLeaf(a, cc("bulv"), 'b', nullptr, 1);
            a.end(notSync);
        }
        const uint32_t depends = a.begin(cc("keyv"));
        stringKey(a, "DependsOnOthers");
        plistLeaf(a, cc("bulv"), 'b', nullptr, key ? 0 : 1);
        a.end(depends);
        a.end(attachments);
        a.end(array);
        a.end(sary);
        a.end(top);
        if (!a.ok()) { failed(82, int32_t(size)); error = -12902; }
        else sbufSize = a.size();
    }
    // The head parameters, then the sample buffer in pieces of what fits a message.
    const uint64_t request = s->nextHostRequest++;
    uint32_t sent = 0;
    bool first = true;
    for (;;) {
        Writer w(message, MaxGuestMessage);
        const uint32_t room = first ? MaxGuestMessage - 48 - 24 - 20 - 20 - 32 - 16 : MaxGuestMessage - 48 - 16;
        const uint32_t chunk = sbufSize - sent < room ? sbufSize - sent : room;
        const bool last = sent + chunk == sbufSize;
        w.header(cc("Eemt"), last ? FlagHostRequest : 0x10, s->uuid, last ? request : 0);
        if (first) {
            w.s64(cc(",frm"), f.frame);
            w.s32(cc(",err"), error);
            w.u32(cc(",efl"), error ? 3 : 1);   // kVTEncodeInfo_Asynchronous, | FrameDropped
            w.uuid(cc(",uui"), f.uuid);
        }
        if (chunk) {
            uint8_t prefix[16];
            put32(prefix, 16 + chunk);
            put32(prefix + 4, cc("para"));
            put32(prefix + 8, cc(",sbf"));
            put32(prefix + 12, cc("=sbf"));
            if (w.size() + 16 + chunk <= MaxGuestMessage) {
                memcpy(message + w.size(), prefix, 16);
                memcpy(message + w.size() + 16, sbuf + sent, chunk);
            }
        }
        const uint32_t length = w.size() + (chunk ? 16 + chunk : 0);
        sent += chunk;
        first = false;
        if (last) {
            // The final piece is the host's request: sent and waited for as one.
            IOLockLock(s->lock);
            s->awaitedRequest = request;
            s->replied = false;
            IOLockUnlock(s->lock);
            droidvm_vthost_replies = droidvm_vthost_replies + 1;
            s->client->send(s->client->transport, message, length);
            IOLockLock(s->lock);
            AbsoluteTime deadline;
            clock_interval_to_deadline(2000, kMillisecondScale, &deadline);
            while (!s->replied && !s->stopping)
                if (IOLockSleepDeadline(s->lock, &s->replied, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) break;
            const bool replied = s->replied;
            s->awaitedRequest = 0;
            IOLockUnlock(s->lock);
            if (replied && !error) droidvm_vthost_frames_out = droidvm_vthost_frames_out + 1;
            if (!replied) failed(83, int32_t(request));
            break;
        }
        droidvm_vthost_replies = droidvm_vthost_replies + 1;
        s->client->send(s->client->transport, message, length);
    }
    IOFree(sbuf, capacity);
    IOFree(message, MaxGuestMessage);
}

// The oldest frame not yet fed, into a free OUTPUT buffer as tightly packed NV12 (the lock held, released for the copy).
bool feedFrame(Session *s) {
    unsigned index = OutputBuffers;
    for (unsigned i = 0; i < OutputBuffers; ++i) if (!s->output[i].queued) { index = i; break; }
    if (index == OutputBuffers) return false;
    Session::Frame *f = nullptr;
    for (unsigned i = s->frameTail; i != s->frameHead; ++i)
        if (!s->frames[i % 64].fed) { f = &s->frames[i % 64]; break; }
    if (!f) return false;
    f->fed = true;
    s->output[index].queued = true;
    const SurfaceView view = f->view;
    const int64_t frame = f->frame;
    IOLockUnlock(s->lock);
    Mapped &out = s->output[index];
    const uint32_t w = s->width, h = s->height, need = w * h * 3 / 2;
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(view.base, view.size, kIODirectionInOut, s->client->task);
    IOMemoryMap *map = memory && memory->prepare() == kIOReturnSuccess ? memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere) : nullptr;
    const uint8_t *surface = map ? reinterpret_cast<const uint8_t *>(map->getVirtualAddress()) : nullptr;
    if (surface && need <= out.length) {
        for (uint32_t y = 0; y < h && y < view.plane[0].height; ++y)
            memcpy(out.bytes + uint64_t(y) * w, surface + view.plane[0].offset + y * view.plane[0].bytesPerRow, w);
        for (uint32_t y = 0; y < h / 2 && y < view.plane[1].height; ++y)
            memcpy(out.bytes + uint64_t(w) * h + uint64_t(y) * w, surface + view.plane[1].offset + y * view.plane[1].bytesPerRow, w);
    } else {
        failed(84, int32_t(need));
    }
    OSSafeReleaseNULL(map);
    if (memory) { memory->complete(); memory->release(); }
    V4l2Buffer b = {};
    b.index = index; b.type = TypeOutputMplane; b.memory = MemoryMmap; b.length = 1;
    b.sec = frame;
    b.plane.bytesused = need;
    b.plane.length = uint32_t(out.length);
    const int r = ioctl(s, IocQbuf, &b, sizeof b, sizeof b);
    IOLockLock(s->lock);
    if (r) { failed(85, r); out.queued = false; }
    return true;
}

Session::Frame *findFrame(Session *s, int64_t frame) {
    for (unsigned i = s->frameTail; i != s->frameHead; ++i)
        if (s->frames[i % 64].frame == frame) return &s->frames[i % 64];
    return nullptr;
}

// A frame handed over: out of the ring (whose tail moves past every frame handed over).
void retireFrame(Session *s, Session::Frame *f) {
    IOLockLock(s->lock);
    f->frame = INT64_MIN;
    while (s->frameTail != s->frameHead && s->frames[s->frameTail % 64].frame == INT64_MIN) ++s->frameTail;
    IOLockUnlock(s->lock);
}

// A coded CAPTURE buffer: its parameter sets kept, its picture handed over.
void handleCoded(Session *s, unsigned index, uint32_t bytesused, uint32_t flags, int64_t frame) {
    const uint8_t *p = s->capture[index].bytes, *end = p + bytesused;
    uint8_t *avcc = static_cast<uint8_t *>(IOMalloc(bytesused + 64));
    if (!avcc) return;
    uint32_t avccSize = 0;
    bool picture = false, key = flags & BufFlagKeyframe;
    const bool hevc = hevcSession(s);
    while (p < end) {
        // The next start code, then the NAL unit up to the one after it.
        const uint8_t *start = p;
        while (start + 3 <= end && !(start[0] == 0 && start[1] == 0 && start[2] == 1)) ++start;
        if (start + 3 > end) break;
        const uint8_t *nal = start + 3, *next = nal;
        while (next + 3 <= end && !(next[0] == 0 && next[1] == 0 && (next[2] == 1 || (next[2] == 0 && next + 3 < end && next[3] == 1)))) ++next;
        if (next + 3 > end) next = end;
        const uint32_t n = uint32_t(next - nal);
        p = next;
        if (!n) continue;
        const unsigned type = hevc ? (nal[0] >> 1) & 0x3f : nal[0] & 0x1f;
        uint8_t *set = nullptr;
        uint32_t *setSize = nullptr;
        if (hevc ? type == 32 : false) { set = s->vps; setSize = &s->vpsSize; }
        else if (hevc ? type == 33 : type == 7) { set = s->sps; setSize = &s->spsSize; }
        else if (hevc ? type == 34 : type == 8) { set = s->pps; setSize = &s->ppsSize; }
        if (set) {
            if (n <= 256 && (n != *setSize || memcmp(set, nal, n))) {
                memcpy(set, nal, n);
                *setSize = n;
                s->describedToGuest = false;
            }
            continue;
        }
        if (hevc ? (type == 35) : (type == 9)) continue;   // access unit delimiters
        const bool vcl = hevc ? type < 32 : (type >= 1 && type <= 5);
        if (vcl) { picture = true; if (hevc ? (type >= 16 && type <= 21) : type == 5) key = true; }
        const uint8_t length[4] = {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
        memcpy(avcc + avccSize, length, 4);
        memcpy(avcc + avccSize + 4, nal, n);
        avccSize += 4 + n;
    }
    IOLockLock(s->lock);
    Session::Frame *f = picture ? findFrame(s, frame) : nullptr;
    IOLockUnlock(s->lock);
    if (picture && !f) failed(86, int32_t(frame));
    if (f) {
        if (!s->describedToGuest) s->describedToGuest = describeToGuest(s);
        emitEncoded(s, *f, avcc, avccSize, key, 0);
        retireFrame(s, f);
    }
    IOFree(avcc, bytesused + 64);
}

int encoderCommand(Session *s, uint32_t command) {
    uint32_t cmd[10] = {command};
    return ioctl(s, IocEncoderCmd, cmd, sizeof cmd, sizeof cmd);
}

// ecfr's answer, once every frame given is handed over: a codec that keeps frames back is drained; at the deadline the frames
// still out are handed over dropped. The lock held.
void answerEncodeFinish(Session *s) {
    if (!s->finishWanted) return;
    bool fedAll = true;
    for (unsigned i = s->frameTail; i != s->frameHead; ++i) if (!s->frames[i % 64].fed) fedAll = false;
    const bool pending = s->frameTail != s->frameHead;
    if (pending && fedAll && !s->draining && mach_absolute_time() < s->finishDeadline) {
        s->draining = true;
        IOLockUnlock(s->lock);
        const int r = encoderCommand(s, EncCmdStop);
        IOLockLock(s->lock);
        if (r) { failed(87, r); s->draining = false; }
        return;
    }
    if (pending && mach_absolute_time() < s->finishDeadline) return;
    while (s->frameTail != s->frameHead) {
        Session::Frame f = s->frames[s->frameTail % 64];
        ++s->frameTail;
        if (f.frame == INT64_MIN) continue;
        IOLockUnlock(s->lock);
        emitEncoded(s, f, nullptr, 0, false, -12911);
        IOLockLock(s->lock);
    }
    Header h;
    h.type = cc("ecfr");
    memcpy(h.uuid, s->uuid, 16);
    h.request = s->finishRequest;
    s->finishWanted = false;
    IOLockUnlock(s->lock);
    replyStatus(s->client, h, 0);
    IOLockLock(s->lock);
}

void endEncodeDrain(Session *s) {
    IOLockLock(s->lock);
    const bool draining = s->draining;
    s->draining = false;
    IOLockUnlock(s->lock);
    if (draining) {
        const int r = encoderCommand(s, EncCmdStart);
        if (r) failed(88, r);
    }
}

void encoderWorkerMain(void *argument, wait_result_t) {
    Session *s = static_cast<Session *>(argument);
    for (;;) {
        IOLockLock(s->lock);
        for (;;) {
            if (s->stopping || s->eventHead != s->eventTail) break;
            if (feedFrame(s)) continue;
            answerEncodeFinish(s);
            if (s->stopping || s->eventHead != s->eventTail) break;
            if (s->finishWanted) IOLockSleepDeadline(s->lock, &s->events, s->finishDeadline, THREAD_UNINT);
            else IOLockSleep(s->lock, &s->events, THREAD_UNINT);
        }
        if (s->stopping) break;
        const Event e = s->events[s->eventTail++ % 64];
        IOLockUnlock(s->lock);
        if (e.kind == MediaEvtDqbuf && e.type == TypeOutputMplane && e.index < OutputBuffers) {
            IOLockLock(s->lock);
            s->output[e.index].queued = false;
            IOLockUnlock(s->lock);
        } else if (e.kind == MediaEvtDqbuf && e.type == TypeCaptureMplane && e.index < s->captureCount) {
            s->capture[e.index].queued = false;
            if (e.bytesused && !(e.flags & BufFlagError)) handleCoded(s, e.index, e.bytesused, e.flags, e.sec);
            if (e.flags & BufFlagLast) endEncodeDrain(s);
            requeueCapture(s, e.index);
        } else if (e.kind == MediaEvtEvent && e.type == EventEos) {
            endEncodeDrain(s);
        } else if (e.kind == MediaEvtError) {
            failed(89, 0);
        }
    }
    s->workerRunning = false;
    IOLockUnlock(s->lock);
    IOLockWakeup(s->lock, &s->workerRunning, false);
    thread_terminate(current_thread());
}

// Epba: the pixel buffers the encoder takes (420v of the session's size). Sent, not waited for: this runs on the guest's thread.
void encoderPixelBufferAttributes(Session *s) {
    Plist *p = new Plist();
    uint8_t buffer[512];
    if (!p) return;
    const int format = p->integer(875704438);   // '420v'
    const int keys[6] = {p->string("Width"), p->string("Height"), p->string("PixelFormatType"), p->string("BytesPerRowAlignment"),
                         p->string("PlaneAlignment"), p->string("IOSurfaceProperties")};
    const int values[6] = {p->integer(s->width), p->integer(s->height), p->array(&format, 1), p->integer(64), p->integer(64),
                           p->dict(nullptr, nullptr, 0)};
    const int attributes = p->dict(keys, values, 6);
    Writer w(buffer, sizeof buffer);
    w.header(cc("Epba"), FlagHostRequest, s->uuid, s->nextHostRequest++);
    w.plist(cc(",pba"), *p, attributes);
    sendToGuest(s->client, w, buffer);
    delete p;
}

// esta: the encoder's virtio-media session, both queues streaming.
int startEncoder(Session *s, const uint8_t *m, uint32_t n) {
    Param size;
    if (!param(m, n, cc(",vdi"), size) || size.size < 8) return -12902;
    const uint32_t width = be32(size.value), height = be32(size.value + 4);
    if (!width || !height || (width & 1) || (height & 1)) return -12902;
    teardown(s);
    s->width = width;
    s->height = height;
    s->spsSize = s->ppsSize = s->vpsSize = 0;
    s->describedToGuest = false;
    s->frameHead = s->frameTail = 0;
    s->device = encoderDevice();
    if (s->device == ~0u) return -12908;
    int r = droidvm_media_open(s->device, &s->v4l2);
    if (r) { failed(90, r); return -12911; }
    s->open = true;
    registerSession(s);
    droidvm_media_set_fast(s->device, true);
    s->fast = true;
    uint32_t subscription[8] = {EventEos, 0, 0, 0, 0, 0, 0, 0};
    ioctl(s, IocSubscribeEvent, subscription, sizeof subscription, 0);
    V4l2Format f = {};
    f.type = TypeCaptureMplane;
    f.width = width;
    f.height = height;
    f.pixelformat = hevcSession(s) ? cc("CVEH") : cc("462H");
    f.num_planes = 1;
    f.planes[0].sizeimage = width * height / 2 + 65536;
    r = ioctl(s, IocSFmt, &f, sizeof f, sizeof f);
    if (r) { failed(91, r); return -12911; }
    f = {};
    f.type = TypeOutputMplane;
    f.width = width;
    f.height = height;
    f.pixelformat = cc("21VN");   // V4L2_PIX_FMT_NV12
    f.num_planes = 1;
    f.planes[0].bytesperline = width;
    f.planes[0].sizeimage = width * height * 3 / 2;
    r = ioctl(s, IocSFmt, &f, sizeof f, sizeof f);
    if (r) { failed(92, r); return -12911; }
    uint32_t request[5] = {OutputBuffers, TypeOutputMplane, MemoryMmap, 0, 0};
    r = ioctl(s, IocReqbufs, request, sizeof request, sizeof request);
    if (r || request[0] < OutputBuffers) { failed(93, r ? r : int32_t(request[0])); return -12904; }
    for (unsigned i = 0; i < OutputBuffers; ++i)
        if (!mapBuffer(s, TypeOutputMplane, i, s->output[i])) return -12904;
    uint32_t captures[5] = {6, TypeCaptureMplane, MemoryMmap, 0, 0};
    r = ioctl(s, IocReqbufs, captures, sizeof captures, sizeof captures);
    if (r || !captures[0]) { failed(94, r); return -12904; }
    s->captureCount = captures[0] < MaxCaptureBuffers ? captures[0] : MaxCaptureBuffers;
    for (unsigned i = 0; i < s->captureCount; ++i) {
        if (!mapBuffer(s, TypeCaptureMplane, i, s->capture[i])) return -12904;
        requeueCapture(s, i);
    }
    uint32_t type = TypeOutputMplane;
    r = ioctl(s, IocStreamon, &type, 4, 0);
    if (r) { failed(95, r); return -12911; }
    s->outputStreaming = true;
    type = TypeCaptureMplane;
    r = ioctl(s, IocStreamon, &type, 4, 0);
    if (r) { failed(96, r); return -12911; }
    s->captureStreaming = true;
    s->stopping = false;
    s->workerRunning = true;
    if (kernel_thread_start(encoderWorkerMain, s, &s->worker) != KERN_SUCCESS) { s->workerRunning = false; return -12904; }
    thread_deallocate(s->worker);
    encoderPixelBufferAttributes(s);
    trace("encoder session %u %ux%u %c%c%c%c", s->v4l2, width, height, s->codec >> 24, s->codec >> 16, s->codec >> 8, s->codec);
    return 0;
}

// efrm: the frame's IOSurface looked up on the guest's thread, the copy left to the worker.
int queueEncodeFrame(Session *s, const uint8_t *m, uint32_t n, const uint32_t *surfaces, unsigned surfaceCount) {
    int64_t frame = 0;
    Param pts, duration, uuid;
    if (!s->outputStreaming || !surfaceCount || !paramS64(m, n, cc(",frm"), frame) || !param(m, n, cc(",pts"), pts) || pts.size < 24 ||
        !param(m, n, cc(",uui"), uuid) || uuid.size < 16) return -12902;
    SurfaceView view;
    bool surface = false;
    IOLockLock(s->surfaceLock);
    for (unsigned i = 0; i < s->knownCount && !surface; ++i)
        if (s->known[i].id == surfaces[0]) { view = s->known[i].view; surface = true; }
    if (!surface) {
        if (!s->surfaces) s->surfaces = openSurfaceClient(s->client->task);
        if (!s->holder) s->holder = processSurfaceClient(s->client->pid);
        surface = s->surfaces && s->holder && lookupSurface(s->surfaces, surfaces[0], view, s->holder);
        if (surface && s->knownCount < 48) s->known[s->knownCount++] = {surfaces[0], view};
    }
    IOLockUnlock(s->surfaceLock);
    if (!surface) { failed(97, int32_t(surfaces[0])); return -12902; }
    IOLockLock(s->lock);
    if (s->frameHead - s->frameTail >= 64) { IOLockUnlock(s->lock); failed(98, 0); return -12911; }
    Session::Frame &f = s->frames[s->frameHead++ % 64];
    f.frame = frame;
    memcpy(f.uuid, uuid.value, 16);
    memcpy(f.pts, pts.value, 24);
    if (param(m, n, cc(",dur"), duration) && duration.size >= 24) memcpy(f.duration, duration.value, 24);
    else memset(f.duration, 0, 24);
    f.view = view;
    f.fed = false;
    IOLockUnlock(s->lock);
    IOLockWakeup(s->lock, &s->events, false);
    droidvm_vthost_frames_in = droidvm_vthost_frames_in + 1;
    return 0;
}

// --- Requests from the guest -----------------------------------------------------------------------------------------------
Session *findSession(Client *c, const uint8_t *uuid) {
    for (Session *s = c->sessions; s; s = s->next)
        if (!memcmp(s->uuid, uuid, 16)) return s;
    return nullptr;
}

void teardown(Session *s) {
    if (s->workerRunning) {
        IOLockLock(s->lock);
        s->stopping = true;
        IOLockWakeup(s->lock, &s->events, false);
        IOLockWakeup(s->lock, &s->replied, false);
        while (s->workerRunning) IOLockSleep(s->lock, &s->workerRunning, THREAD_UNINT);
        IOLockUnlock(s->lock);
        s->stopping = false;
    }
    if (s->open) {
        uint32_t type = TypeOutputMplane;
        if (s->outputStreaming) ioctl(s, IocStreamoff, &type, 4, 0);
        type = TypeCaptureMplane;
        if (s->captureStreaming) ioctl(s, IocStreamoff, &type, 4, 0);
        for (Mapped &m : s->output) unmapBuffer(s, m);
        for (Mapped &m : s->capture) unmapBuffer(s, m);
        unregisterSession(s);
        droidvm_media_close(s->device, s->v4l2);
        if (s->fast) droidvm_media_set_fast(s->device, false);
    }
    s->open = s->outputStreaming = s->captureStreaming = s->fast = false;
    s->captureCount = 0;
    while (s->inputTail != s->inputHead) {
        const Session::Input &input = s->inputs[s->inputTail++ % 128];
        IOFree(input.bytes, input.capacity);
    }
    s->pendingFrames = 0;
    s->outstandingCount = 0;
    s->frameHead = s->frameTail = 0;
    s->finishWanted = s->draining = false;
    if (s->surfaces) {
        IOLockLock(s->surfaceLock);
        s->surfaces->clientClose();
        OSSafeReleaseNULL(s->surfaces);
        s->knownCount = 0;
        IOLockUnlock(s->surfaceLock);
    }
    OSSafeReleaseNULL(s->holder);
}

// dsup/esup: the properties the codec has, by name, each {PropertyType, ReadWriteStatus} (as macOS's host lists its
// hardware codecs'). VideoToolbox answers a property missing here itself (kVTPropertyNotSupportedErr) and sends no dcop/ecop
// or dset/eset for it: only that the codec runs on hardware, which dcop/ecop answer, is listed.
void supportedProperties(Client *c, const Header &h) {
    Plist *p = new Plist();
    uint8_t buffer[384];
    if (!p) return;
    const int typeKeys[2] = {p->string("PropertyType"), p->string("ReadWriteStatus")};
    const int typeValues[2] = {p->string("Boolean"), p->string("ReadOnly")};
    const int names[1] = {p->string(h.type == cc("dsup") ? "UsingHardwareAcceleratedVideoDecoder" : "UsingHardwareAcceleratedVideoEncoder")};
    const int values[1] = {p->dict(typeKeys, typeValues, 2)};
    const int supported = p->dict(names, values, 1);
    Writer w(buffer, sizeof buffer);
    w.header(h.type, FlagReply, h.uuid, h.request);
    w.s32(cc(",err"), 0);
    w.plist(cc(",sup"), *p, supported);
    sendToGuest(c, w, buffer);
    delete p;
}

void decoderList(Client *c, const Header &h) {
    Plist *p = new Plist();
    uint8_t *buffer = static_cast<uint8_t *>(IOMallocZero(4096));
    if (p && buffer) {
        // H.264 and HEVC: what the virtio-media decoder takes (ENUM_FMT) and VideoToolbox can describe for it.
        static const char *const codecs[2][3] = {
            {"avc1", "com.droidvm.videodecoder.virtio-media.avc", "DroidVM Video Decoder - AVC (virtio-media)"},
            {"hvc1", "com.droidvm.videodecoder.virtio-media.hevc", "DroidVM Video Decoder - HEVC (virtio-media)"},
        };
        int entries[2];
        for (int i = 0; i < 2; ++i) {
            const int type = p->string(codecs[i][0]);
            const int keys[5] = {p->string("VTCodecType"), p->string("CMClassImplementationName"), p->string("IsPreferredInternalPlugin"),
                                 p->string("VTRating"), p->string("CMClassImplementationID")};
            const int values[5] = {p->array(&type, 1), p->string(codecs[i][2]), p->boolean(false), p->integer(400), p->string(codecs[i][1])};
            entries[i] = p->dict(keys, values, 5);
        }
        const int list = p->array(entries, decoderDevice() == ~0u ? 0 : 2);
        Writer w(buffer, 4096);
        w.header(h.type, FlagReply, h.uuid, h.request);
        w.plist(cc(",dec"), *p, list);
        w.u32(cc(",ver"), 6);
        sendToGuest(c, w, buffer);
    }
    delete p;
    if (buffer) IOFree(buffer, 4096);
}

// encs: H.264 and HEVC encoders over the virtio-media encoder (VTCodecType a number here, as the Mac's host answers).
void encoderList(Client *c, const Header &h) {
    Plist *p = new Plist();
    uint8_t *buffer = static_cast<uint8_t *>(IOMallocZero(4096));
    if (p && buffer) {
        static const struct { uint32_t type; const char *name, *id, *description; } codecs[2] = {
            {cc("avc1"), "H.264", "com.droidvm.videoencoder.virtio-media.avc", "DroidVM Video Encoder - AVC (virtio-media)"},
            {cc("hvc1"), "HEVC", "com.droidvm.videoencoder.virtio-media.hevc", "DroidVM Video Encoder - HEVC (virtio-media)"},
        };
        int entries[2];
        for (int i = 0; i < 2; ++i) {
            const int keys[5] = {p->string("VTCodecType"), p->string("VTCodecName"), p->string("CMClassImplementationName"),
                                 p->string("VTRating"), p->string("CMClassImplementationID")};
            const int values[5] = {p->integer(codecs[i].type), p->string(codecs[i].name), p->string(codecs[i].description),
                                   p->integer(400), p->string(codecs[i].id)};
            entries[i] = p->dict(keys, values, 5);
        }
        const int list = p->array(entries, encoderDevice() == ~0u ? 0 : 2);
        Writer w(buffer, 4096);
        w.header(h.type, FlagReply, h.uuid, h.request);
        w.plist(cc(",enc"), *p, list);
        w.u32(cc(",ver"), 6);
        sendToGuest(c, w, buffer);
    }
    delete p;
    if (buffer) IOFree(buffer, 4096);
}

void createDecoder(Client *c, const Header &h, const uint8_t *m, uint32_t n) {
    int32_t codec = 0;
    paramS32(m, n, cc(",cod"), codec);
    Session *s = findSession(c, h.uuid);
    if (!s) {
        s = new Session();
        if (!s || !(s->lock = IOLockAlloc()) || !(s->surfaceLock = IOLockAlloc())) {
            if (s && s->lock) IOLockFree(s->lock);
            delete s;
            replyStatus(c, h, -12904);
            return;
        }
        s->client = c;
        memcpy(s->uuid, h.uuid, 16);
        s->next = c->sessions;
        c->sessions = s;
        droidvm_vthost_sessions = droidvm_vthost_sessions + 1;
    }
    s->codec = uint32_t(codec);
    s->encoder = h.type == cc("+enc");
    Plist *p = new Plist();
    uint8_t buffer[256];
    if (p) {
        const int keys[1] = {p->string("VTHostBuildVersion")};
        const int values[1] = {p->string("DroidVM virtio-media")};
        const int info = p->dict(keys, values, 1);
        Writer w(buffer, sizeof buffer);
        w.header(h.type, FlagReply, h.uuid, h.request);
        w.s32(cc(",err"), 0);
        w.u32(cc(",ver"), 6);
        w.plist(cc(",hin"), *p, info);
        sendToGuest(c, w, buffer);
    }
    delete p;
}

// Dpba: the pixel buffers the guest should make for this session's frames.
void pixelBufferAttributes(Session *s) {
    Plist *p = new Plist();
    uint8_t buffer[512];
    if (!p) return;
    const int format = p->integer(875704438);   // '420v'
    const int keys[6] = {p->string("Width"), p->string("Height"), p->string("PixelFormatType"), p->string("BytesPerRowAlignment"),
                         p->string("PlaneAlignment"), p->string("IOSurfaceProperties")};
    const int values[6] = {p->integer(s->width), p->integer(s->height), p->array(&format, 1), p->integer(64), p->integer(256),
                           p->dict(nullptr, nullptr, 0)};
    const int attributes = p->dict(keys, values, 6);
    Writer w(buffer, sizeof buffer);
    w.header(cc("Dpba"), 0, s->uuid, 0);
    w.plist(cc(",pba"), *p, attributes);
    sendToGuest(s->client, w, buffer);
    delete p;
}

// What the phone's decoder is not given, so that VideoToolbox decodes it in software instead: a picture outside the coded sizes
// the device reports (ENUM_FRAMESIZES; Qualcomm's start at 96x96), or a stream that is not 4:2:0, since the CAPTURE queue is
// NV12 alone (H.264 High 10, 4:2:2 and 4:4:4 profiles, which MediaCodec fails frame by frame; HEVC by the hvcC's chroma format
// and bit depths: Main 10 comes out as 8-bit NV12, which the 420v pixel buffers VideoToolbox is given want anyway).
bool decodableFormat(const ImageDescription &d) {
    Atom record;
    if (find(d.extensions, d.extensionsSize, cc("avcC"), record)) {
        if (record.size < 4) return false;
        const uint8_t profile = record.body[1];
        return profile == 66 || profile == 77 || profile == 100;   // Baseline, Main, High
    }
    if (find(d.extensions, d.extensionsSize, cc("hvcC"), record)) {
        if (record.size < 23) return false;
        const uint8_t chroma = record.body[16] & 3, lumaDepth = (record.body[17] & 7) + 8, chromaDepth = (record.body[18] & 7) + 8;
        return chroma == 1 && lumaDepth <= 10 && chromaDepth <= 10;
    }
    return false;
}

bool decodableSize(Session *s, uint32_t pixelformat) {
    uint32_t sizes[11] = {0, pixelformat};   // struct v4l2_frmsizeenum: index, pixel_format, type, stepwise, reserved
    const int r = ioctl(s, IocEnumFramesizes, sizes, sizeof sizes, sizeof sizes);
    if (r) return true;   // a device that does not say lets the codec decide
    const uint32_t minWidth = sizes[3], maxWidth = sizes[4], minHeight = sizes[6], maxHeight = sizes[7];
    const bool fits = s->width >= minWidth && s->width <= maxWidth && s->height >= minHeight && s->height <= maxHeight;
    if (!fits) trace("%ux%u outside %u..%u x %u..%u", s->width, s->height, minWidth, maxWidth, minHeight, maxHeight);
    return fits;
}

int startSession(Session *s, const uint8_t *m, uint32_t n) {
    Param fds;
    ImageDescription d;
    if (!param(m, n, cc(",fds"), fds) || !imageDescription(fds.value, fds.size, d)) return -12902;
    if (d.codec != cc("avc1") && d.codec != cc("hvc1") && d.codec != cc("hev1")) return -12906;
    if (!decodableFormat(d)) return -12910;   // kVTVideoDecoderUnsupportedDataFormatErr
    if (!parameterSets(s, d)) return -12909;
    teardown(s);
    s->width = d.width;
    s->height = d.height;
    s->parametersSent = false;
    s->pendingFrames = 0;
    s->device = decoderDevice();
    if (s->device == ~0u) return -12906;
    int r = droidvm_media_open(s->device, &s->v4l2);
    if (r) { failed(1, r); return -12911; }
    s->open = true;
    registerSession(s);
    droidvm_media_set_fast(s->device, true);
    s->fast = true;
    if (!decodableSize(s, d.codec == cc("avc1") ? cc("462H") : cc("CVEH"))) { teardown(s); return -12910; }
    uint32_t subscription[8] = {EventSourceChange, 0, 0, 0, 0, 0, 0, 0};
    r = ioctl(s, IocSubscribeEvent, subscription, sizeof subscription, 0);
    if (r) failed(2, r);
    subscription[0] = EventEos;
    ioctl(s, IocSubscribeEvent, subscription, sizeof subscription, 0);
    V4l2Format f = {};
    f.type = TypeOutputMplane;
    f.width = d.width;
    f.height = d.height;
    f.pixelformat = d.codec == cc("avc1") ? cc("462H") : cc("CVEH");   // V4L2_PIX_FMT_H264 'H264', _HEVC 'HEVC' (little endian)
    f.num_planes = 1;
    f.planes[0].sizeimage = OutputBytes;
    r = ioctl(s, IocSFmt, &f, sizeof f, sizeof f);
    if (r) { failed(3, r); return -12911; }
    uint32_t request[5] = {OutputBuffers, TypeOutputMplane, MemoryMmap, 0, 0};
    r = ioctl(s, IocReqbufs, request, sizeof request, sizeof request);
    if (r || request[0] < OutputBuffers) { failed(4, r ? r : int32_t(request[0])); return -12904; }
    for (unsigned i = 0; i < OutputBuffers; ++i)
        if (!mapBuffer(s, TypeOutputMplane, i, s->output[i])) return -12904;
    uint32_t type = TypeOutputMplane;
    r = ioctl(s, IocStreamon, &type, 4, 0);
    if (r) { failed(5, r); return -12911; }
    s->outputStreaming = true;
    s->stopping = false;
    s->workerRunning = true;
    if (kernel_thread_start(workerMain, s, &s->worker) != KERN_SUCCESS) { s->workerRunning = false; return -12904; }
    thread_deallocate(s->worker);
    pixelBufferAttributes(s);
    IOLog("DroidVMVTHost: session %u %ux%u %c%c%c%c\n", s->v4l2, s->width, s->height, d.codec >> 24, d.codec >> 16, d.codec >> 8, d.codec);
    return 0;
}

// dfrm: the frame's NAL units as Annex B, kept for the worker (selector 3 never waits for an OUTPUT buffer).
int queueFrame(Session *s, const uint8_t *m, uint32_t n) {
    int64_t frame = 0;
    paramS64(m, n, cc(",frm"), frame);
    Param sbf;
    Atom sbuf, sdat;
    if (!s->outputStreaming || !param(m, n, cc(",sbf"), sbf) || !find(sbf.value, sbf.size, cc("sbuf"), sbuf) ||
        !find(sbuf.body, sbuf.size, cc("sdat"), sdat)) return -12902;
    const uint32_t capacity = sdat.size + (s->nalLength < 4 ? sdat.size : 0) + (s->parametersSent ? 0 : s->parameterSetsSize) + 64;
    uint8_t *bytes = static_cast<uint8_t *>(IOMalloc(capacity));
    if (!bytes) return -12904;
    uint32_t at = 0;
    const uint8_t start[4] = {0, 0, 0, 1};
    if (!s->parametersSent) {
        memcpy(bytes, s->parameterSets, s->parameterSetsSize);
        at = s->parameterSetsSize;
        s->parametersSent = true;
    }
    const uint8_t *p = sdat.body, *end = sdat.body + sdat.size;
    while (uint32_t(end - p) > s->nalLength) {
        uint32_t length = 0;
        for (uint32_t i = 0; i < s->nalLength; ++i) length = length << 8 | p[i];
        p += s->nalLength;
        if (length > uint32_t(end - p) || at + 4 + length > capacity) { failed(7, int32_t(length)); break; }
        memcpy(bytes + at, start, 4);
        memcpy(bytes + at + 4, p, length);
        at += 4 + length;
        p += length;
    }
    IOLockLock(s->lock);
    if (s->inputHead - s->inputTail >= 128) {
        IOLockUnlock(s->lock);
        IOFree(bytes, capacity);
        failed(6, 0);
        return -12911;
    }
    s->inputs[s->inputHead++ % 128] = {frame, bytes, at, capacity};
    ++s->pendingFrames;
    if (s->outstandingCount < 64) s->outstanding[s->outstandingCount++] = frame;
    IOLockUnlock(s->lock);
    IOLockWakeup(s->lock, &s->events, false);
    droidvm_vthost_frames_in = droidvm_vthost_frames_in + 1;
    return 0;
}

}  // namespace

extern "C" {

// Whether crosvm gave the guest a virtio-media decoder or encoder: the transport publishes Apple's driver only then.
bool droidvm_vthost_available(void) { return decoderDevice() != ~0u || encoderDevice() != ~0u; }

void *droidvm_vthost_client_open(void *transport, bool (*send)(void *, const uint8_t *, uint32_t), task_t task, int pid) {
    if (!gTableLock) {
        IOLock *lock = IOLockAlloc();
        if (!OSCompareAndSwapPtr(nullptr, lock, &gTableLock)) IOLockFree(lock);
    }
    Client *c = new Client();
    if (!c) return nullptr;
    c->transport = transport;
    c->send = send;
    c->task = task;
    c->pid = pid;
    c->lock = IOLockAlloc();
    c->sessions = nullptr;
    return c;
}

void droidvm_vthost_client_close(void *host) {
    Client *c = static_cast<Client *>(host);
    if (!c) return;
    while (Session *s = c->sessions) {
        c->sessions = s->next;
        teardown(s);
        IOLockFree(s->lock);
        IOLockFree(s->surfaceLock);
        delete s;
    }
    if (c->lock) IOLockFree(c->lock);
    delete c;
}

// One message from the guest, on the guest's thread, with the IDs of the IOSurfaces that came with it.
int droidvm_vthost_message(void *host, const uint8_t *m, uint32_t n, const uint32_t *surfaces, unsigned surfaceCount) {
    Client *c = static_cast<Client *>(host);
    Header h;
    if (!c || !header(m, n, h)) return -1;
    // The guest's replies to the host's requests (Dbuf, Demt).
    if (h.flags & FlagGuestReply) {
        Session *s = findSession(c, h.uuid);
        if (!s) return 0;
        int32_t error = 0;
        paramS32(m, n, cc(",err"), error);
        Param uuid;
        SurfaceView view;
        bool surface = false;
        if (h.type == cc("Dbuf") && !error && surfaceCount && param(m, n, cc(",uui"), uuid) && uuid.size >= 16) {
            IOLockLock(s->surfaceLock);
            for (unsigned i = 0; i < s->knownCount && !surface; ++i)
                if (s->known[i].id == surfaces[0]) { view = s->known[i].view; surface = true; }
            if (!surface) {
                if (!s->surfaces) s->surfaces = openSurfaceClient(c->task);
                surface = s->surfaces && lookupSurface(s->surfaces, surfaces[0], view);
                if (surface && s->knownCount < 48) s->known[s->knownCount++] = {surfaces[0], view};
            }
            IOLockUnlock(s->surfaceLock);
            if (!surface) failed(42, int32_t(surfaces[0]));
        }
        IOLockLock(s->lock);
        if (s->awaitedRequest == h.request) {
            s->replyError = error;
            s->replyHasSurface = surface;
            if (surface) { s->replySurface = view; memcpy(s->replyUuid, uuid.value, 16); }
            s->replied = true;
        }
        IOLockUnlock(s->lock);
        IOLockWakeup(s->lock, &s->replied, false);
        return 0;
    }
    IOLockLock(c->lock);
    Session *s = findSession(c, h.uuid);
    switch (h.type) {
    case cc("decs"): decoderList(c, h); break;
    case cc("encs"): encoderList(c, h); break;
    case cc("+enc"): createDecoder(c, h, m, n); break;
    case cc("eset"): case cc("emul"): case cc("epfr"):
        replyStatus(c, h, 0);
        break;
    case cc("esta"): replyStatus(c, h, s ? startEncoder(s, m, n) : -12903); break;
    case cc("efrm"): {
        const int error = s ? queueEncodeFrame(s, m, n, surfaces, surfaceCount) : -12903;
        uint8_t buffer[128];
        Writer w(buffer, sizeof buffer);
        w.header(h.type, FlagReply, h.uuid, h.request);
        w.s32(cc(",err"), error);
        w.u32(cc(",inf"), 1);
        sendToGuest(c, w, buffer);
        break;
    }
    case cc("ecfr"):
        if (s && s->workerRunning) {
            IOLockLock(s->lock);
            s->finishWanted = true;
            s->finishRequest = h.request;
            clock_interval_to_deadline(2000, kMillisecondScale, &s->finishDeadline);
            IOLockUnlock(s->lock);
            IOLockWakeup(s->lock, &s->events, false);
        } else {
            replyStatus(c, h, 0);
        }
        break;
    case cc("einv"):
        if (s) teardown(s);
        replyStatus(c, h, 0);
        break;
    case cc("-enc"):
        if (s) {
            teardown(s);
            for (Session **at = &c->sessions; *at; at = &(*at)->next)
                if (*at == s) { *at = s->next; break; }
            IOLockFree(s->lock);
            IOLockFree(s->surfaceLock);
            delete s;
        }
        break;
    case cc("+dec"): createDecoder(c, h, m, n); break;
    case cc("dset"): replyStatus(c, h, 0); break;
    case cc("dmul"): replyStatus(c, h, 0); break;
    case cc("dsta"): replyStatus(c, h, s ? startSession(s, m, n) : -12903); break;
    case cc("dsup"): case cc("esup"):
        supportedProperties(c, h);
        break;
    case cc("dcop"): case cc("ecop"): {
        // Properties the session asks of the codec. The one it has is that it runs on hardware; anything else (an HEVC encoder
        // session asks MVHEVCVideoLayerIDs, the layers of a multiview stream) is kVTPropertyNotSupportedErr, as a codec without it
        // answers.
        Param key;
        char name[96] = "";
        if (param(m, n, cc(",key"), key)) plistString(key.value, key.size, name, sizeof name);
        if (!strcmp(name, h.type == cc("dcop") ? "UsingHardwareAcceleratedVideoDecoder" : "UsingHardwareAcceleratedVideoEncoder")) {
            Plist *p = new Plist();
            uint8_t buffer[192];
            if (p) {
                const int yes = p->boolean(true);
                Writer w(buffer, sizeof buffer);
                w.header(h.type, FlagReply, h.uuid, h.request);
                w.s32(cc(",err"), 0);
                w.plist(cc(",val"), *p, yes);
                sendToGuest(c, w, buffer);
            }
            delete p;
        } else {
            replyStatus(c, h, -12900);
        }
        break;
    }
    case cc("dfrm"): {
        const int error = s ? queueFrame(s, m, n) : -12903;
        uint8_t buffer[128];
        Writer w(buffer, sizeof buffer);
        w.header(h.type, FlagReply, h.uuid, h.request);
        w.s32(cc(",err"), error);
        w.u32(cc(",inf"), 1);
        sendToGuest(c, w, buffer);
        break;
    }
    case cc("dfin"):
        if (s && s->workerRunning) {
            IOLockLock(s->lock);
            s->finishWanted = true;
            s->finishRequest = h.request;
            clock_interval_to_deadline(2000, kMillisecondScale, &s->finishDeadline);
            IOLockUnlock(s->lock);
            IOLockWakeup(s->lock, &s->events, false);
        } else {
            replyStatus(c, h, 0);
        }
        break;
    case cc("dinv"):
        if (s) teardown(s);
        replyStatus(c, h, 0);
        break;
    case cc("-dec"):
        if (s) {
            teardown(s);
            for (Session **at = &c->sessions; *at; at = &(*at)->next)
                if (*at == s) { *at = s->next; break; }
            IOLockFree(s->lock);
            IOLockFree(s->surfaceLock);
            delete s;
        }
        break;
    default:
        droidvm_vthost_unknown = droidvm_vthost_unknown + 1;
        droidvm_vthost_last_unknown = h.type;
        if (h.flags & FlagRequest) replyStatus(c, h, -12900);
        break;
    }
    IOLockUnlock(c->lock);
    return 0;
}

}  // extern "C"

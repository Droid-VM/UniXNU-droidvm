// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The host half of VideoToolbox's paravirtualized decoder, in the guest's own kernel: what a Mac running an Apple virtual machine
// answers, played over crosvm's virtio-media decoder (DroidVMVirtioMedia.c; a stateful V4L2 decoder over the phone's MediaCodec).
// DroidVMVTParavirt.cpp is the transport (Apple's AppleVideoToolboxParavirtualizationDriver user client) and hands every message
// here. The messages and their parameters are those a loopback of VideoToolbox's own guest and host halves exchanges on macOS 27
// (DroidVMVTWire.h for the format):
//   decs   the decoder list: one entry per coded format the virtio-media decoder offers that VideoToolbox can describe (H.264,
//          HEVC). The guest registers each as "paravirtualized:<CMClassImplementationID>" (rating 400, above its software ones).
//   +dec   a decoder instance (,cod the codec type, ,cid the ID): answered with ,err ,ver ,hin.
//   dset dcop dsup   properties: taken, UsingHardwareAcceleratedVideoDecoder is true, no supported-property list.
//   dsta   the session's format (,fds an ImageDescription with avcC or hvcC): a virtio-media session is opened, its OUTPUT queue set
//          up and streaming; the guest is told the pixel buffers it should allocate (Dpba, 420v of the coded size) before the
//          answer.
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
// ID, alive), and closing the client at the session's end gives every reference and mapping back.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
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
    IocSubscribeEvent = 90, IocDecoderCmd = 96,
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
        OSDictionary *matching = IOService::nameMatching("IOCoreSurfaceRoot");
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

bool lookupSurface(IOUserClient *client, uint32_t id, SurfaceView &view) {
    uint8_t *result = static_cast<uint8_t *>(IOMallocZero(LockResultBytes));
    if (!result) return false;
    uint64_t input = id;
    IOExternalMethodArguments arguments = {};
    arguments.version = kIOExternalMethodArgumentsCurrentVersion;
    arguments.selector = 4;
    arguments.scalarInput = &input;
    arguments.scalarInputCount = 1;
    arguments.structureOutput = result;
    arguments.structureOutputSize = LockResultBytes;
    // IOSurfaceRootUserClient is an IOUserClient2022, whose legacy externalMethod panics; a client the kernel opened itself has
    // not been through is_io_service_open_extended, which tells callExternalMethod so, hence the 2022 method, directly.
    IOUserClient2022 *client2022 = OSDynamicCast(IOUserClient2022, client);
    const IOReturn status = client2022 ? client2022->externalMethod(4, reinterpret_cast<IOExternalMethodArgumentsOpaque *>(&arguments))
                                       : client->callExternalMethod(4, &arguments);
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
    IOLock *surfaceLock = nullptr;
    struct { uint32_t id; SurfaceView view; } known[48];
    unsigned knownCount = 0;

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
void mediaSink(void *, const uint8_t *event, uint32_t size) {
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
        if (!s || !s->open || s->v4l2 != session) continue;
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
        droidvm_media_set_sink(d, mediaSink, nullptr);
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
    s->finishWanted = s->draining = false;
    if (s->surfaces) {
        IOLockLock(s->surfaceLock);
        s->surfaces->clientClose();
        OSSafeReleaseNULL(s->surfaces);
        s->knownCount = 0;
        IOLockUnlock(s->surfaceLock);
    }
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

void emptyList(Client *c, const Header &h, uint32_t key) {
    Plist *p = new Plist();
    uint8_t buffer[256];
    if (p) {
        const int list = p->array(nullptr, 0);
        Writer w(buffer, sizeof buffer);
        w.header(h.type, FlagReply, h.uuid, h.request);
        w.plist(key, *p, list);
        w.u32(cc(",ver"), 6);
        sendToGuest(c, w, buffer);
    }
    delete p;
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

int startSession(Session *s, const uint8_t *m, uint32_t n) {
    Param fds;
    ImageDescription d;
    if (!param(m, n, cc(",fds"), fds) || !imageDescription(fds.value, fds.size, d)) return -12902;
    if (d.codec != cc("avc1") && d.codec != cc("hvc1") && d.codec != cc("hev1")) return -12906;
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
    case cc("encs"): emptyList(c, h, cc(",enc")); break;
    case cc("+dec"): createDecoder(c, h, m, n); break;
    case cc("dset"): replyStatus(c, h, 0); break;
    case cc("dmul"): replyStatus(c, h, 0); break;
    case cc("dsta"): replyStatus(c, h, s ? startSession(s, m, n) : -12903); break;
    case cc("dsup"): {
        Plist *p = new Plist();
        uint8_t buffer[192];
        if (p) {
            const int empty = p->dict(nullptr, nullptr, 0);
            Writer w(buffer, sizeof buffer);
            w.header(h.type, FlagReply, h.uuid, h.request);
            w.s32(cc(",err"), 0);
            w.plist(cc(",sup"), *p, empty);
            sendToGuest(c, w, buffer);
        }
        delete p;
        break;
    }
    case cc("dcop"): {
        Param key;
        char name[96] = "";
        if (param(m, n, cc(",key"), key)) plistString(key.value, key.size, name, sizeof name);
        if (!strcmp(name, "UsingHardwareAcceleratedVideoDecoder")) {
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

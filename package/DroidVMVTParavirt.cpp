// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// VideoToolbox's paravirtualized codecs, as Apple's virtual machines offer them: the guest half of the channel. VideoToolbox in
// iPadOS 27 carries a whole paravirtualized decoder, encoder, JPEG session and motion estimation processor
// (ParavirtualizedVideoDecoder_*, ParavirtualizedVideoEncoder_*), and uses them for every eligible codec once
// VTParavirtualizationIsRunningInGuest: once VideoToolboxParavirtualizationSupport.framework (AppleParavirtGPU-1) has opened
// the service AppleVideoToolboxParavirtualizationDriver. That framework is the whole guest transport, read from the 24A446
// dyld shared cache (IOServiceOpen type 0):
//   selector 0  scalars {size} -> {capacity, send buffer, queue}: the buffer a message to the host is written into and the
//               IODataQueue the host's messages arrive in, both mapped into the caller by the kernel (16 KiB first; grown to a
//               larger message, page rounded, before it is written).
//   selector 1  structure in: a guest UUID (16 bytes) a session registers; selector 2 deregisters it.
//   selector 3  scalars {length, IOSurface IDs (up to 5)} -> {status}: the message of `length` bytes now in the send buffer.
//   selector 4  no arguments: the queue was drained (after the notification's dequeue loop).
//   notification port type 0: the queue's (IODataQueueAllocateNotificationPort; each dequeue takes at most 16 KiB).
// A message (VTParavirtualizationCreateMessageAndByteStream) is a FigAtom 'vtpv' of 48 bytes, big endian: its size and type,
// the message type (a FourCC), flags, the guest UUID, a request ID and 8 reserved bytes; atoms of serialized CF types and
// sample buffers follow.
//
// This file is the transport: the classes under Apple's names (the platform sandbox lets a process open only the user clients
// it names, as DroidVMSensorProcessor.cpp's), the buffers, the queue, and a record of every message for the host rig (peek.py):
// droidvm_vtpv_log, and whole messages in a contiguous capture region at droidvm_vtpv_capture_pa: the first of each type and
// direction, CaptureEntries slots of CaptureBytes (the length, the direction -- 0 from the guest, 1 to it -- then the message).
// Each message then goes to the host's side (DroidVMVTHost.cpp: the decoder and encoder over crosvm's virtio-media), whose
// messages to the guest are queued here. The service is
// published at boot with droidvm-vt-paravirt=1 (among the board's built-in boot arguments; =0 on the command line turns it off),
// or when root writes 1 to kern.droidvm_vt_paravirt (registered at run time, as DroidVMMotion.c's; it reads the stage), and only
// when crosvm gives a virtio-media decoder or encoder: without it VideoToolbox keeps its own codecs. Each process asks once
// (VTParavirtualizationIsRunningInGuest is a dispatch_once), so only processes started after the write use the channel.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSharedDataQueue.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/sysctl.h>

extern kmod_info_t g_kernel_kmod_info;
static IOBufferMemoryDescriptor *capture;
extern "C" {
void *droidvm_vthost_client_open(void *transport, bool (*send)(void *, const uint8_t *, uint32_t), task_t task, int pid);
bool droidvm_vthost_available(void);
void droidvm_vthost_client_close(void *host);
int droidvm_vthost_message(void *host, const uint8_t *m, uint32_t n, const uint32_t *surfaces, unsigned surfaceCount);
volatile uint32_t droidvm_vtpv_sent, droidvm_vtpv_dropped;
}
extern "C" int proc_selfpid(void);

namespace {
constexpr uint32_t FirstSendBytes = 0x4000;       // the framework's first request
constexpr uint64_t MaxSendBytes = 64ull << 20;    // a message's limit (a compressed frame and its attachments)
constexpr uint32_t QueueBytes = 256 * 1024;       // the host's messages waiting for the guest
constexpr uint32_t HeaderBytes = 48;              // the 'vtpv' atom
constexpr uint32_t MaxSurfaces = 5;
constexpr unsigned LogEntries = 32, CaptureEntries = 32, CaptureBytes = 8192;

uint32_t big32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t big64(const uint8_t *p) { return uint64_t(big32(p)) << 32 | big32(p + 4); }
}

extern "C" {
// One message from the guest: who sent it, its header, its IOSurfaces and its first payload bytes (after the header).
struct droidvm_vtpv_record {
    uint32_t seq, pid, length, surface_count;
    uint32_t type, flags;
    uint64_t request;
    uint8_t uuid[16];
    uint32_t surfaces[MaxSurfaces];
    uint32_t reserved;
    uint8_t payload[64];
};
// For the host rig: 1 the class registered, 2 the service published; selector calls by selector; opens; refused messages.
volatile uint32_t droidvm_vtpv_stage, droidvm_vtpv_opens, droidvm_vtpv_messages, droidvm_vtpv_refused, droidvm_vtpv_uuids;
volatile uint32_t droidvm_vtpv_selectors[8];
// registerNotificationPort: calls, the last type and result; the step buffers() last failed at (1 the queue, 2 its mapping,
// 3 the send buffer, 4 its mapping, 5 too large); clients closed.
volatile uint32_t droidvm_vtpv_notify_calls, droidvm_vtpv_notify_type, droidvm_vtpv_notify_result, droidvm_vtpv_buffer_fail;
volatile uint32_t droidvm_vtpv_closes;
struct droidvm_vtpv_record droidvm_vtpv_log[LogEntries];
volatile uint64_t droidvm_vtpv_capture_pa;
int droidvm_vt_paravirt_attach(void);
}

// The first message of each type and direction, whole, into the capture region.
static IOLock *captureLock;
static void captureMessage(const uint8_t *bytes, uint32_t length, uint32_t direction) {
    if (!capture || !captureLock || length < HeaderBytes) return;
    const uint32_t type = big32(bytes + 8);
    IOLockLock(captureLock);
    uint8_t *region = static_cast<uint8_t *>(capture->getBytesNoCopy());
    for (unsigned i = 0; i < CaptureEntries; ++i) {
        uint8_t *slot = region + i * CaptureBytes;
        uint32_t used, dir;
        memcpy(&used, slot, 4);
        memcpy(&dir, slot + 4, 4);
        if (used && dir == direction && big32(slot + 8 + 8) == type) break;   // that type is kept already
        if (used) continue;
        const uint32_t kept = length < CaptureBytes - 8 ? length : CaptureBytes - 8;
        memcpy(slot + 4, &direction, 4);
        memcpy(slot + 8, bytes, kept);
        memcpy(slot, &length, 4);
        break;
    }
    IOLockUnlock(captureLock);
}

class AppleVideoToolboxParavirtualizationDriver : public IOService {
    OSDeclareDefaultStructors(AppleVideoToolboxParavirtualizationDriver)
public:
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        setProperty("IOUserClientClass", "AppleVideoToolboxParavirtualizationUserClient");
        // Synchronously: a process that writes kern.droidvm_vt_paravirt finds the service as soon as the write returns.
        registerService(kIOServiceSynchronous);
        return true;
    }
};
OSDefineMetaClassAndStructors(AppleVideoToolboxParavirtualizationDriver, IOService)

class AppleVideoToolboxParavirtualizationUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AppleVideoToolboxParavirtualizationUserClient)
    typedef IOUserClient super;
    task_t owner;
    IOLock *lock;
    IOBufferMemoryDescriptor *send;
    IOMemoryMap *sendMap;
    IOSharedDataQueue *queue;
    IOMemoryMap *queueMap;
    uint64_t capacity;
    IOLock *sendLock;     // the queue has one producer at a time: the guest's thread and the host's workers both send
    void *host;
    static bool sendToGuest(void *transport, const uint8_t *bytes, uint32_t size);

    IOReturn buffers(uint64_t size, uint64_t *out);
    IOReturn message(uint64_t length, const uint64_t *surfaces, unsigned count, uint64_t *status);
public:
    bool initWithTask(task_t task, void *token, UInt32 type, OSDictionary *properties) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    void free() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
    IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) override;
};
OSDefineMetaClassAndStructors(AppleVideoToolboxParavirtualizationUserClient, IOUserClient)

bool AppleVideoToolboxParavirtualizationUserClient::initWithTask(task_t task, void *token, UInt32 type, OSDictionary *properties) {
    if (type != 0 || !super::initWithTask(task, token, type, properties)) return false;
    owner = task;
    lock = IOLockAlloc();
    sendLock = IOLockAlloc();
    return lock && sendLock;
}

bool AppleVideoToolboxParavirtualizationUserClient::start(IOService *provider) {
    if (!super::start(provider)) return false;
    droidvm_vtpv_opens = droidvm_vtpv_opens + 1;
    host = droidvm_vthost_client_open(this, sendToGuest, owner, proc_selfpid());
    return true;
}

IOReturn AppleVideoToolboxParavirtualizationUserClient::clientClose() {
    droidvm_vtpv_closes = droidvm_vtpv_closes + 1;
    void *closing = host;
    host = nullptr;
    droidvm_vthost_client_close(closing);
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}

void AppleVideoToolboxParavirtualizationUserClient::free() {
    if (host) { droidvm_vthost_client_close(host); host = nullptr; }
    OSSafeReleaseNULL(sendMap);
    OSSafeReleaseNULL(send);
    OSSafeReleaseNULL(queueMap);
    OSSafeReleaseNULL(queue);
    if (lock) IOLockFree(lock);
    if (sendLock) IOLockFree(sendLock);
    super::free();
}

// Selector 0: the send buffer at least `size` bytes (a new one, mapped anew, when it must grow; the framework stops using the
// old one as it takes the answer) and the queue, made and mapped once.
IOReturn AppleVideoToolboxParavirtualizationUserClient::buffers(uint64_t size, uint64_t *out) {
    size = size < FirstSendBytes ? FirstSendBytes : (size + page_size - 1) & ~uint64_t(page_size - 1);
    if (size > MaxSendBytes) { droidvm_vtpv_buffer_fail = 5; return kIOReturnNoMemory; }
    if (!queue) {
        queue = IOSharedDataQueue::withCapacity(QueueBytes);
        if (!queue) { droidvm_vtpv_buffer_fail = 1; return kIOReturnNoMemory; }
        IOMemoryDescriptor *memory = queue->getMemoryDescriptor();
        queueMap = memory ? memory->createMappingInTask(owner, 0, kIOMapAnywhere) : nullptr;
        OSSafeReleaseNULL(memory);
        if (!queueMap) { droidvm_vtpv_buffer_fail = 2; return kIOReturnNoMemory; }
    }
    if (!send || size > capacity) {
        IOBufferMemoryDescriptor *buffer =
            IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryKernelUserShared),
                                                      size, page_size);
        IOMemoryMap *map = buffer ? buffer->createMappingInTask(owner, 0, kIOMapAnywhere) : nullptr;
        if (!map) {
            droidvm_vtpv_buffer_fail = buffer ? 4 : 3;
            OSSafeReleaseNULL(buffer);
            return kIOReturnNoMemory;
        }
        OSSafeReleaseNULL(sendMap);
        OSSafeReleaseNULL(send);
        send = buffer;
        sendMap = map;
        capacity = size;
    }
    out[0] = capacity;
    out[1] = sendMap->getAddress();
    out[2] = queueMap->getAddress();
    return kIOReturnSuccess;
}

// Selector 3: a message of `length` bytes in the send buffer, with the IDs of the IOSurfaces that go with it. Recorded; the
// host's side is not there yet, so nothing answers it.
IOReturn AppleVideoToolboxParavirtualizationUserClient::message(uint64_t length, const uint64_t *surfaces, unsigned count,
                                                               uint64_t *status) {
    if (!send || length < HeaderBytes || length > capacity) {
        droidvm_vtpv_refused = droidvm_vtpv_refused + 1;
        return kIOReturnBadArgument;
    }
    const uint8_t *bytes = static_cast<const uint8_t *>(send->getBytesNoCopy());
    const uint32_t seq = droidvm_vtpv_messages;
    droidvm_vtpv_record &r = droidvm_vtpv_log[seq % LogEntries];
    r.seq = seq;
    r.pid = uint32_t(proc_selfpid());
    r.length = uint32_t(length);
    r.type = big32(bytes + 8);
    r.flags = big32(bytes + 12);
    memcpy(r.uuid, bytes + 16, sizeof r.uuid);
    r.request = big64(bytes + 32);
    r.surface_count = 0;
    for (unsigned i = 0; i < count && i < MaxSurfaces && surfaces[i]; ++i) r.surfaces[r.surface_count++] = uint32_t(surfaces[i]);
    memset(r.payload, 0, sizeof r.payload);
    memcpy(r.payload, bytes + HeaderBytes, length - HeaderBytes < sizeof r.payload ? length - HeaderBytes : sizeof r.payload);
    captureMessage(bytes, uint32_t(length), 0);
    droidvm_vtpv_messages = seq + 1;
    uint32_t ids[MaxSurfaces];
    for (unsigned i = 0; i < r.surface_count; ++i) ids[i] = r.surfaces[i];
    if (host) droidvm_vthost_message(host, bytes, uint32_t(length), ids, r.surface_count);
    *status = 0;
    return kIOReturnSuccess;
}

// A message to the guest, into its queue (whose notification wakes the framework's dequeue loop).
bool AppleVideoToolboxParavirtualizationUserClient::sendToGuest(void *transport, const uint8_t *bytes, uint32_t size) {
    auto *self = static_cast<AppleVideoToolboxParavirtualizationUserClient *>(transport);
    captureMessage(bytes, size, 1);
    IOLockLock(self->sendLock);
    const bool sent = self->queue && self->queue->enqueue(const_cast<uint8_t *>(bytes), size);
    IOLockUnlock(self->sendLock);
    if (sent) droidvm_vtpv_sent = droidvm_vtpv_sent + 1;
    else droidvm_vtpv_dropped = droidvm_vtpv_dropped + 1;
    return sent;
}

IOReturn AppleVideoToolboxParavirtualizationUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
                                                                       IOExternalMethodDispatch *, OSObject *, void *) {
    if (selector < 8) droidvm_vtpv_selectors[selector] = droidvm_vtpv_selectors[selector] + 1;
    IOReturn result = kIOReturnUnsupported;
    IOLockLock(lock);
    switch (selector) {
    case 0:
        if (a->scalarInputCount != 1 || a->scalarOutputCount < 3) { result = kIOReturnBadArgument; break; }
        result = buffers(a->scalarInput[0], a->scalarOutput);
        if (!result) a->scalarOutputCount = 3;
        break;
    case 1: case 2:
        if (a->structureInputSize != 16) { result = kIOReturnBadArgument; break; }
        if (selector == 1) droidvm_vtpv_uuids = droidvm_vtpv_uuids + 1;
        result = kIOReturnSuccess;
        break;
    case 3:
        if (a->scalarInputCount < 1 || a->scalarInputCount > 1 + MaxSurfaces || a->scalarOutputCount < 1) {
            result = kIOReturnBadArgument;
            break;
        }
        result = message(a->scalarInput[0], a->scalarInput + 1, a->scalarInputCount - 1, a->scalarOutput);
        if (!result) a->scalarOutputCount = 1;
        break;
    case 4:
        result = kIOReturnSuccess;
        break;
    }
    IOLockUnlock(lock);
    return result;
}

IOReturn AppleVideoToolboxParavirtualizationUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t) {
    droidvm_vtpv_notify_calls = droidvm_vtpv_notify_calls + 1;
    droidvm_vtpv_notify_type = type;
    if (type != 0) { droidvm_vtpv_notify_result = kIOReturnBadArgument; return kIOReturnBadArgument; }
    IOLockLock(lock);
    IOReturn result = kIOReturnNotReady;
    if (!queue) {
        uint64_t out[3];
        result = buffers(FirstSendBytes, out);
    }
    if (queue) {
        queue->setNotificationPort(port);
        result = kIOReturnSuccess;
    }
    IOLockUnlock(lock);
    droidvm_vtpv_notify_result = uint32_t(result);
    return result;
}

#if !DROIDVM_NATIVE_KEXT
#define DROIDVM_LATE_INIT(file) extern "C" void droidvm_late_init_##file(void) __asm__("__GLOBAL__sub_I_" #file ".cpp");
DROIDVM_LATE_INIT(DroidVMVTParavirt)
#endif

// Publishes the service on IOResources, once.
static int publish(void) {
    if (droidvm_vtpv_stage != 1) return droidvm_vtpv_stage == 2 ? 0 : -1;
    if (!droidvm_vthost_available()) return -1;   // no codec to stand for: VideoToolbox keeps its own
    capture = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, IOOptionBits(kIODirectionInOut) |
                                                               IOOptionBits(kIOMemoryPhysicallyContiguous),
                                                               CaptureEntries * CaptureBytes, 0x0000fffffffff000ULL);
    captureLock = IOLockAlloc();
    if (capture) {
        bzero(capture->getBytesNoCopy(), CaptureEntries * CaptureBytes);
        droidvm_vtpv_capture_pa = capture->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    }
    IOService *resources = IOService::getResourceService();
    AppleVideoToolboxParavirtualizationDriver *driver = OSTypeAlloc(AppleVideoToolboxParavirtualizationDriver);
    if (!driver || !resources || !driver->init() || !driver->attach(resources)) {
        OSSafeReleaseNULL(driver);
        return -1;
    }
    if (!driver->start(resources)) {
        driver->detach(resources);
        driver->release();
        return -1;
    }
    driver->release();
    droidvm_vtpv_stage = 2;
    return 0;
}

static IOLock *publishLock;

static int droidvm_vtpv_sysctl(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
    int value = int(droidvm_vtpv_stage);
    const int error = sysctl_handle_int(oidp, &value, 0, req);
    if (error || !req->newptr) return error;
    if (value != 1) return EINVAL;
    IOLockLock(publishLock);
    const int published = publish();
    IOLockUnlock(publishLock);
    return published ? EIO : 0;
}

static struct sysctl_oid droidvm_vtpv_oid;

// Registers the classes in their own OSMetaClass module load (the static initializer staged for this call, as the other board
// classes are) and the sysctl, and with droidvm-vt-paravirt=1 publishes the service.
int droidvm_vt_paravirt_attach(void) {
    if (droidvm_vtpv_stage) return -1;
#if !DROIDVM_NATIVE_KEXT
    void *module = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!module) return -1;
    droidvm_late_init_DroidVMVTParavirt();
    if (OSMetaClass::postModLoad(module)) return -1;
#endif   // a native kext's classes are registered as it loads
    droidvm_vtpv_stage = 1;
    if (!(publishLock = IOLockAlloc())) return -1;
    droidvm_vtpv_oid.oid_parent = &sysctl__kern_children;
    droidvm_vtpv_oid.oid_number = OID_AUTO;
    // CTLFLAG_OID2: the structure carries oid_descr and oid_version (SYSCTL_OID sets it; arm64 panics without it).
    droidvm_vtpv_oid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
    droidvm_vtpv_oid.oid_name = "droidvm_vt_paravirt";
    droidvm_vtpv_oid.oid_handler = droidvm_vtpv_sysctl;
    droidvm_vtpv_oid.oid_fmt = "I";
    droidvm_vtpv_oid.oid_descr = "VideoToolbox paravirtualization: 1 classes registered, 2 service published; write 1 to publish";
    droidvm_vtpv_oid.oid_version = SYSCTL_OID_VERSION;
    sysctl_register_oid(&droidvm_vtpv_oid);
    uint32_t enabled = 0;
    PE_parse_boot_argn("droidvm-vt-paravirt", &enabled, sizeof enabled);
    return enabled ? publish() : 0;
}

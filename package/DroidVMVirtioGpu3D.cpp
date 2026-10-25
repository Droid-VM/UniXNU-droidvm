// SPDX-License-Identifier: BSD-2-Clause-Patent
// The guest's 3D context on the virtio GPU: virglrenderer's DRM native context (capset 6), which the host's drm2kgsl serves on
// KGSL. A context's msm shmem (where the host answers its ccmds) is a HOST3D blob the host carves from the drm2kgsl_host pool;
// MAP_BLOB answers with MAP_INFO_POOL and the offset in it, and the pool's address is /arm-io/virtio-gpu drm-host-pool.
//
// vgpu-selftest=1 runs one context through at boot, before any client exists: the DRM capset, a context, its shmem,
// and a GET_PARAM(GPU_ID) ccmd answered there. Its progress is droidvm_virtgpu_selftest, for peek.py: the stage it reached
// and the status there, the capset's GPU id and context type, the shmem's pool offset, and the ccmd's return value and GPU id.
#include "DroidVMVirtioGpu.h"
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
extern "C" {
// stage, status, capset gpu id, context type, pool offset, ccmd ret, ccmd gpu id, capset bytes; then the shmem's seqno and response
// offset, and the first 32 bytes of the response as they were read
volatile uint64_t droidvm_virtgpu_selftest[14];
}
namespace DroidVMVirtioGpu {
namespace {
enum : uint32_t {
    CmdUnref = 0x102, CmdGetCapsetInfo = 0x108, CmdGetCapset = 0x109, CmdCreateBlob = 0x10c, CmdCtxCreate = 0x200,
    CmdCtxDestroy = 0x201, CmdSubmit3D = 0x207, CmdMapBlob = 0x208, CmdUnmapBlob = 0x209,
    RespCapsetInfo = 0x1102, RespCapset = 0x1103, RespMapInfo = 0x1106,
    CapsetIdDrm = 6, BlobHost3D = 2, BlobMappable = 1, MapInfoPool = 1u << 31, ShmemBytes = 16384,
    FeatureVirgl = 1u << 0, FeatureBlob = 1u << 3, FeatureContextInit = 1u << 4,
    CcmdIoctlSimple = 2, MsmGetParam = 0xc0186440, MsmPipe3D0 = 0x10, MsmParamGpuId = 1,
};
struct CapsetInfo { Header h; uint32_t id, maxVersion, maxSize, padding; };
struct CapsetDrm {   // virgl_renderer_capset_drm, its msm part
    uint32_t wireFormat, major, minor, patch, contextType, pad, hasCachedCoherent, priorities;
    uint64_t vaStart, vaSize;
    uint32_t gpuId, gmemSize;
};
struct CtxCreate { Header h; uint32_t nameLength, contextInit; char name[64]; };
struct CreateBlob { Header h; uint32_t resource, blobMem, blobFlags, entries; uint64_t blobId, size; };
struct MapBlob { Header h; uint32_t resource, padding; uint64_t offset; };
struct MapInfo { Header h; uint32_t mapInfo, poolOffset; };
struct ResourceCommand { Header h; uint32_t resource, padding; };
struct Submit { Header h; uint32_t size, padding; };
struct Shmem { uint32_t seqno, responseOffset; };
// vdrm_ccmd_req, then msm_ccmd_ioctl_simple_req's ioctl and its drm_msm_param payload (at byte 20, so assembled bytewise).
struct DrmMsmParam { uint32_t pipe, param; uint64_t value; uint32_t length, pad; };
void stage(uint64_t reached, uint64_t status) { droidvm_virtgpu_selftest[0] = reached; droidvm_virtgpu_selftest[1] = status; }
bool pool(uint64_t &base, uint64_t &size) {
    DTEntry node; const void *value; unsigned length;
    if (SecureDTLookupEntry(nullptr, "/arm-io/virtio-gpu", &node) != kSuccess ||
        SecureDTGetProperty(node, "drm-host-pool", &value, &length) != kSuccess || length != 16) return false;
    memcpy(&base, value, 8); memcpy(&size, static_cast<const uint8_t *>(value) + 8, 8);
    return base && size;
}
}
void selftest3D() {
    unsigned enabled = 0; PE_parse_boot_argn("vgpu-selftest", &enabled, sizeof enabled);
    if (!enabled || !ready()) return;
    IOReturn status;
    stage(1, 0);
    if ((features() & (FeatureVirgl | FeatureBlob | FeatureContextInit)) != (FeatureVirgl | FeatureBlob | FeatureContextInit)) {
        stage(1, features()); IOLog("virtgpu-selftest: no 3D, blob or context-init feature (%llx)\n", features()); return;
    }
    uint32_t capsetBytes = 0;
    for (uint32_t index = 0; index < 8 && !capsetBytes; ++index) {
        struct { Header h; uint32_t index, padding; } ask = {{CmdGetCapsetInfo, 0, 0, 0, 0, {}}, index, 0};
        CapsetInfo info = {};
        if (command(&ask, sizeof ask, nullptr, 0, &info, sizeof info) || info.h.type != RespCapsetInfo) break;
        IOLog("virtgpu-selftest: capset %u: id %u version %u size %u\n", index, info.id, info.maxVersion, info.maxSize);
        if (info.id == CapsetIdDrm) capsetBytes = info.maxSize;
    }
    stage(2, capsetBytes);
    if (!capsetBytes || capsetBytes > 4096 - sizeof(Header)) { IOLog("virtgpu-selftest: no DRM capset\n"); return; }
    droidvm_virtgpu_selftest[7] = capsetBytes;
    struct { Header h; uint32_t id, version; } ask = {{CmdGetCapset, 0, 0, 0, 0, {}}, CapsetIdDrm, 0};
    static uint8_t answer[4096];   // the capset, once at boot; too large for a kernel stack frame
    status = command(&ask, sizeof ask, nullptr, 0, answer, uint32_t(sizeof(Header) + capsetBytes));
    if (status || reinterpret_cast<Header *>(answer)->type != RespCapset) { stage(3, status); return; }
    CapsetDrm caps = {}; memcpy(&caps, answer + sizeof(Header), capsetBytes < sizeof caps ? capsetBytes : sizeof caps);
    droidvm_virtgpu_selftest[2] = caps.gpuId; droidvm_virtgpu_selftest[3] = caps.contextType;
    IOLog("virtgpu-selftest: DRM capset wire %u context type %u gpu %u va %llx+%llx gmem %u\n", caps.wireFormat, caps.contextType,
          caps.gpuId, caps.vaStart, caps.vaSize, caps.gmemSize);
    uint64_t poolBase = 0, poolSize = 0;
    if (!pool(poolBase, poolSize)) { stage(4, 0); IOLog("virtgpu-selftest: no drm-host-pool in the ADT\n"); return; }
    const uint32_t context = 1;
    CtxCreate create = {{CmdCtxCreate, 0, 0, context, 0, {}}, 8, CapsetIdDrm, "selftest"};
    Header reply = {};
    status = command(&create, sizeof create, nullptr, 0, &reply, sizeof reply);
    stage(5, status);
    if (status) return;
    const uint32_t resource = resourceId();
    CreateBlob blob = {{CmdCreateBlob, 0, 0, context, 0, {}}, resource, BlobHost3D, BlobMappable, 0, 0, ShmemBytes};
    status = command(&blob, sizeof blob, nullptr, 0, &reply, sizeof reply);
    stage(6, status);
    IOMemoryDescriptor *memory = nullptr;
    IOMemoryMap *map = nullptr;
    if (!status) {
        MapBlob mapBlob = {{CmdMapBlob, 0, 0, context, 0, {}}, resource, 0, 0};
        MapInfo info = {};
        status = command(&mapBlob, sizeof mapBlob, nullptr, 0, &info, sizeof info);
        stage(7, status ? status : info.mapInfo);
        if (!status && info.h.type == RespMapInfo && (info.mapInfo & MapInfoPool) && info.poolOffset + uint64_t(ShmemBytes) <= poolSize) {
            droidvm_virtgpu_selftest[4] = info.poolOffset;
            memory = IOMemoryDescriptor::withPhysicalAddress(poolBase + info.poolOffset, ShmemBytes, kIODirectionInOut);
            map = memory ? memory->map(kIOMapCopybackCache) : nullptr;
        }
    }
    if (map) {
        auto shmem = reinterpret_cast<volatile Shmem *>(map->getVirtualAddress());
        uint8_t request[16 + 4 + sizeof(DrmMsmParam)] = {};
        const uint32_t header[4] = {CcmdIoctlSimple, uint32_t(sizeof request), 1, 0}, ioctl = MsmGetParam;
        const DrmMsmParam param = {MsmPipe3D0, MsmParamGpuId, 0, 0, 0};
        memcpy(request, header, sizeof header); memcpy(request + 16, &ioctl, 4); memcpy(request + 20, &param, sizeof param);
        // The host copies back at most the length the guest wrote at the response (its guard against the guest changing it
        // meanwhile), as vdrm_alloc_rsp does: msm_ccmd_ioctl_simple_rsp, its length and return value and the drm_msm_param.
        *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<volatile uint8_t *>(shmem) + shmem->responseOffset) = 8 + sizeof(DrmMsmParam);
        Submit submit = {{CmdSubmit3D, 0, 0, context, 0, {}}, uint32_t(sizeof request), 0};
        uint32_t ticket = 0;
        status = post(&submit, sizeof submit, request, sizeof request, sizeof reply, false, &ticket);
        if (!status) status = wait(ticket, 1000000, &reply, sizeof reply);
        stage(8, status);
        for (unsigned i = 0; !status && shmem->seqno < 1 && i < 1000; ++i) IOSleep(1);
        if (!status && shmem->seqno >= 1) {
            // msm_ccmd_ioctl_simple_rsp: the length, the ioctl's return value, then the drm_msm_param as the host filled it.
            const volatile uint8_t *response = reinterpret_cast<const volatile uint8_t *>(shmem) + shmem->responseOffset;
            int32_t ret; DrmMsmParam answered;
            memcpy(&ret, const_cast<const uint8_t *>(response) + 4, 4);
            memcpy(&answered, const_cast<const uint8_t *>(response) + 8, sizeof answered);
            droidvm_virtgpu_selftest[5] = uint32_t(ret); droidvm_virtgpu_selftest[6] = answered.value;
            droidvm_virtgpu_selftest[8] = shmem->seqno; droidvm_virtgpu_selftest[9] = shmem->responseOffset;
            for (unsigned i = 0; i < 4; ++i) memcpy(const_cast<uint64_t *>(&droidvm_virtgpu_selftest[10 + i]), const_cast<const uint8_t *>(response) + 8 * i, 8);
            stage(9, 0);
            IOLog("virtgpu-selftest: shmem at pool %llx, seqno %u; GET_PARAM(GPU_ID) ret %d value %llu\n", droidvm_virtgpu_selftest[4],
                  shmem->seqno, ret, answered.value);
        } else if (!status) {
            stage(8, kIOReturnTimeout); IOLog("virtgpu-selftest: the host never answered the ccmd (seqno %u)\n", shmem->seqno);
        }
        map->release();
    }
    if (memory) memory->release();
    ResourceCommand unmap = {{CmdUnmapBlob, 0, 0, context, 0, {}}, resource, 0};
    (void)command(&unmap, sizeof unmap, nullptr, 0, &reply, sizeof reply);
    (void)unref(resource);
    Header destroy = {CmdCtxDestroy, 0, 0, context, 0, {}};
    (void)command(&destroy, sizeof destroy, nullptr, 0, &reply, sizeof reply);
}
}

// ---- user space: the virtgpu methods of the accelerator's user client (DroidVMVirtgpuABI.h) -------------------------------------
// One client per connection that called a method: its DRM context, its resources by handle and its fenced submissions in flight.
// A client's lock serializes its methods (a Metal device's threads share one connection); a fence wait sleeps without it.
#include "DroidVMVirtgpuABI.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMultiMemoryDescriptor.h>
extern "C" void proc_selfname(char *, int);
extern "C" {
// clients now, contexts made, blobs made, maps made, submissions, fenced submissions, fence waits that timed out, refusals
volatile uint64_t droidvm_virtgpu_stats[8];
// the last refusal: op, status, input and output size; and refusals by op
volatile uint64_t droidvm_virtgpu_last_refusal[4], droidvm_virtgpu_refusals_by_op[16];
}
namespace DroidVMVirtioGpu {
namespace {
enum : uint32_t { MaxClients = 64, MaxRings = 64, MaxPending = 256, FirstContext = 16, ContiguousLimit = 4u << 20,
                  MaxBlobEntries = (MaxCommand - sizeof(CreateBlob)) / sizeof(Entry) };
struct Resource {
    uint32_t handle, id;
    uint64_t size;
    bool hostVisible;
    IOMemoryDescriptor *pages;         // a guest blob's own pages, or the caller's (CREATE_BLOB_USER)
    bool wired;                        // pages is the caller's memory, prepared (wired) until the blob goes
    IOMemoryDescriptor *window;        // a host blob's range of the pool, once mapped
    IOMemoryMap *map;                  // in the client's task
};
struct Pending { uint32_t ticket, ring; uint64_t fence; };
struct Client {
    void *connection;
    IOLock *lock;
    uint32_t context, rings, nextHandle, resourceCount, resourceCapacity, pendingCount;
    Resource *resources;
    Pending pending[MaxPending];
    uint64_t nextFence[MaxRings], completed[MaxRings];
};
Client *gClients[MaxClients];
IOLock *gClientsLock;
uint32_t gNextContext = FirstContext;
uint64_t gCapsetMask;
uint32_t gCapsetBytes[64];   // each capset's size (GET_CAPSET_INFO)
bool gCapsetsQueried;
void count(unsigned index) { droidvm_virtgpu_stats[index] = droidvm_virtgpu_stats[index] + 1; }
uint64_t nowUsLocal() { uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000; }
Client *client(void *connection, bool create) {
    if (!gClientsLock) {   // the first call comes from a user client, long after IOKit is up
        IOLock *made = IOLockAlloc();
        if (made && !OSCompareAndSwapPtr(nullptr, made, reinterpret_cast<void *volatile *>(&gClientsLock))) IOLockFree(made);
        if (!gClientsLock) return nullptr;
    }
    IOLockLock(gClientsLock);
    Client *found = nullptr; int empty = -1;
    for (int i = 0; i < int(MaxClients); ++i) {
        if (gClients[i] && gClients[i]->connection == connection) { found = gClients[i]; break; }
        if (!gClients[i] && empty < 0) empty = i;
    }
    if (!found && create && empty >= 0) {
        found = IONewZero(Client, 1);
        if (found) {
            found->connection = connection; found->lock = IOLockAlloc(); found->nextHandle = 1;
            if (!found->lock) { IODelete(found, Client, 1); found = nullptr; }
            else { gClients[empty] = found; count(0); }
        }
    }
    IOLockUnlock(gClientsLock);
    return found;
}
Resource *resource(Client *c, uint32_t handle) {
    for (uint32_t i = 0; i < c->resourceCount; ++i) if (c->resources[i].handle == handle) return &c->resources[i];
    return nullptr;
}
// Takes every finished fenced submission off the client's list; with `fence` set, whether that fence on `ring` has signalled.
void reapPending(Client *c) {
    for (uint32_t i = 0; i < c->pendingCount;) {
        IOReturn result;
        if (!poll(c->pending[i].ticket, &result)) { ++i; continue; }
        const Pending done = c->pending[i];
        if (done.fence > c->completed[done.ring]) c->completed[done.ring] = done.fence;
        c->pending[i] = c->pending[--c->pendingCount];
    }
}
// Waits for every submission of the client in flight (before its resources' pages go back), at most `timeoutUs`.
void drain(Client *c, uint64_t timeoutUs) {
    const uint64_t begin = nowUsLocal();
    for (reapPending(c); c->pendingCount && nowUsLocal() - begin < timeoutUs; reapPending(c)) IOSleep(1);
}
IOReturn submitCommand(Client *c, const void *command, uint32_t size) {
    Submit submit = {{CmdSubmit3D, 0, 0, c->context, 0, {}}, size, 0};
    Header reply = {};
    uint32_t ticket = 0;
    IOReturn status = post(&submit, sizeof submit, command, size, sizeof reply, false, &ticket);
    return status ? status : wait(ticket, 5000000, &reply, sizeof reply);
}
void dropPages(Resource &r) {
    if (r.pages && r.wired) r.pages->complete();
    if (r.pages) r.pages->release();
    r.pages = nullptr; r.wired = false;
}
void releaseResource(Client *c, Resource &r) {
    Header reply = {};
    if (r.map) { r.map->release(); r.map = nullptr; }
    if (r.window) {
        ResourceCommand unmap = {{CmdUnmapBlob, 0, 0, c->context, 0, {}}, r.id, 0};
        (void)command(&unmap, sizeof unmap, nullptr, 0, &reply, sizeof reply);
        r.window->release(); r.window = nullptr;
    }
    (void)unref(r.id);
    // The host has dropped its import of the pages with the resource; only then do they go back.
    dropPages(r);
}
void queryCapsets() {
    if (gCapsetsQueried) return;
    for (uint32_t index = 0; index < 16; ++index) {
        struct { Header h; uint32_t index, padding; } ask = {{CmdGetCapsetInfo, 0, 0, 0, 0, {}}, index, 0};
        CapsetInfo info = {};
        if (command(&ask, sizeof ask, nullptr, 0, &info, sizeof info) || info.h.type != RespCapsetInfo) break;
        if (info.id < 64) { gCapsetMask |= uint64_t(1) << info.id; gCapsetBytes[info.id] = info.maxSize; }
    }
    gCapsetsQueried = true;
}
IOReturn getParam(const droidvm_virtgpu_getparam &in, droidvm_virtgpu_getparam_reply &out) {
    uint64_t base, size;
    switch (in.param) {
    case DROIDVM_VIRTGPU_PARAM_3D_FEATURES: out.value = (features() & FeatureVirgl) != 0; break;
    case DROIDVM_VIRTGPU_PARAM_CAPSET_QUERY_FIX: out.value = 1; break;
    case DROIDVM_VIRTGPU_PARAM_RESOURCE_BLOB: out.value = (features() & FeatureBlob) != 0; break;
    case DROIDVM_VIRTGPU_PARAM_HOST_VISIBLE: out.value = pool(base, size); break;
    case DROIDVM_VIRTGPU_PARAM_CROSS_DEVICE: out.value = 0; break;
    case DROIDVM_VIRTGPU_PARAM_CONTEXT_INIT: out.value = (features() & FeatureContextInit) != 0; break;
    case DROIDVM_VIRTGPU_PARAM_SUPPORTED_CAPSET_IDS: queryCapsets(); out.value = gCapsetMask; break;
    case DROIDVM_VIRTGPU_PARAM_CREATE_GUEST_HANDLE: out.value = (features() & (1u << 6)) != 0; break;
    case DROIDVM_VIRTGPU_PARAM_ABI_VERSION: out.value = DROIDVM_VIRTGPU_ABI_VERSION; break;
    case DROIDVM_VIRTGPU_PARAM_DEFAULT_3D: {
        unsigned on = 0; PE_parse_boot_argn("vgpu-3d", &on, sizeof on); out.value = on != 0; break;
    }
    default: return kIOReturnUnsupported;
    }
    return kIOReturnSuccess;
}
IOReturn getCaps(const droidvm_virtgpu_get_caps &in, void *out, size_t outSize) {
    static uint8_t answer[4096];   // under the client's lock; one capset at a time across clients is enough
    if (!outSize || outSize > sizeof answer - sizeof(Header) || in.capset_id >= 64) return kIOReturnBadArgument;
    struct { Header h; uint32_t id, version; } ask = {{CmdGetCapset, 0, 0, 0, 0, {}}, in.capset_id, in.version};
    IOLockLock(gClientsLock);
    queryCapsets();
    // The device answers with the whole capset, so it gets room for that; the caller gets the part it asked for, zero-padded.
    const uint32_t bytes = gCapsetBytes[in.capset_id];
    IOReturn status = bytes && bytes <= sizeof answer - sizeof(Header) ? kIOReturnSuccess : kIOReturnUnsupported;
    if (!status) { memset(answer, 0, sizeof answer); status = command(&ask, sizeof ask, nullptr, 0, answer, uint32_t(sizeof(Header) + bytes)); }
    if (!status && reinterpret_cast<Header *>(answer)->type != RespCapset) status = kIOReturnIOError;
    if (!status) memcpy(out, answer + sizeof(Header), outSize);
    IOLockUnlock(gClientsLock);
    return status;
}
IOReturn contextInit(Client *c, const droidvm_virtgpu_context_init &in, droidvm_virtgpu_context_init_reply &out) {
    if (c->context) return kIOReturnExclusiveAccess;
    if (!in.num_rings || in.num_rings > MaxRings) return kIOReturnBadArgument;
    IOLockLock(gClientsLock); const uint32_t context = gNextContext++; IOLockUnlock(gClientsLock);
    CtxCreate create = {{CmdCtxCreate, 0, 0, context, 0, {}}, 0, in.capset_id, {}};
    proc_selfname(create.name, sizeof create.name);
    create.nameLength = uint32_t(strnlen(create.name, sizeof create.name));
    Header reply = {};
    IOReturn status = command(&create, sizeof create, nullptr, 0, &reply, sizeof reply);
    if (status) return status;
    c->context = context; c->rings = in.num_rings; out.context_id = context; count(1);
    return kIOReturnSuccess;
}
// A guest blob's pages: wired and zeroed, physically contiguous in chunks of at most ContiguousLimit (halved down to 64 KiB
// while an allocation fails), so a blob of any size the host is told about in few entries (one per page refused blobs above
// 16 MiB). Several chunks become one IOMultiMemoryDescriptor.
IOMemoryDescriptor *guestPages(uint64_t bytes) {
    const uint32_t most = uint32_t((bytes + 0xffff) >> 16);
    IOMemoryDescriptor **chunks = IONew(IOMemoryDescriptor *, most);
    if (!chunks) return nullptr;
    uint32_t count = 0; uint64_t chunk = ContiguousLimit, offset = 0;
    while (offset < bytes) {
        const uint64_t length = bytes - offset < chunk ? bytes - offset : chunk;
        IOBufferMemoryDescriptor *piece = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous), length, ~uint64_t(4095));
        if (!piece && chunk > 0x10000) { chunk >>= 1; continue; }
        if (!piece || count == most) { if (piece) piece->release(); break; }
        bzero(piece->getBytesNoCopy(), length);
        chunks[count++] = piece; offset += length;
    }
    IOMemoryDescriptor *pages = nullptr;
    if (offset == bytes)
        pages = count == 1 ? chunks[0] : IOMultiMemoryDescriptor::withDescriptors(chunks, count, kIODirectionInOut, false);
    if (count == 1 && pages == chunks[0]) chunks[0] = nullptr;
    for (uint32_t i = 0; i < count; ++i) if (chunks[i]) chunks[i]->release();
    IODelete(chunks, IOMemoryDescriptor *, most);
    return pages;
}
// A client's own pages for CREATE_BLOB_USER, wired, as NativeAdreno imports them (NativeAdrenoMemory.cpp importRange): through a
// memory entry of their own (kIOMemoryPersistent), so the owner may unmap its range while the GPU still uses it, and read-only where
// the owner's mapping cannot be written (a mapped file, constant data; Metal only reads no-copy buffers over such memory).
IOMemoryDescriptor *userPages(task_t task, uint64_t address, uint64_t bytes) {
    const IODirection directions[] = {kIODirectionInOut, kIODirectionOut};
    for (IODirection direction : directions) {
        const IOOptionBits options = IOOptionBits(direction) | IOOptionBits(kIOMemoryMapperNone);
        IOMemoryDescriptor *pages = IOMemoryDescriptor::withAddressRange(address, bytes, options | kIOMemoryPersistent, task);
        if (!pages) pages = IOMemoryDescriptor::withAddressRange(address, bytes, options, task);
        if (!pages) continue;
        if (pages->prepare() == kIOReturnSuccess) return pages;
        pages->release();
    }
    return nullptr;
}
// The size of the last guest blob whose pages or entries ran out, for peek.py.
volatile uint64_t droidvm_virtgpu_last_blob_refused;
// A blob: the host's (HOST3D), guest memory the kernel allocates (HOST3D_GUEST), or with `user` the caller's own pages at that
// address in `task` (CREATE_BLOB_USER), wired while the blob lives.
IOReturn createBlob(Client *c, const droidvm_virtgpu_create_blob &in, const uint8_t *ccmd, droidvm_virtgpu_create_blob_reply &out,
                    task_t task = nullptr, uint64_t user = 0) {
    if (!c->context) return kIOReturnNotReady;
    const bool guest = in.blob_mem == DROIDVM_VIRTGPU_BLOB_MEM_HOST3D_GUEST;
    if ((!guest && in.blob_mem != DROIDVM_VIRTGPU_BLOB_MEM_HOST3D) || !in.size || in.size > (uint64_t(1) << 32) || (in.cmd_size & 3))
        return kIOReturnBadArgument;
    if (c->resourceCount == c->resourceCapacity) {
        const uint32_t capacity = c->resourceCapacity ? c->resourceCapacity * 2 : 64;
        Resource *grown = IONewZero(Resource, capacity);
        if (!grown) return kIOReturnNoMemory;
        if (c->resources) { memcpy(grown, c->resources, sizeof(Resource) * c->resourceCount); IODelete(c->resources, Resource, c->resourceCapacity); }
        c->resources = grown; c->resourceCapacity = capacity;
    }
    // The ccmd that describes the blob to the host (GEM_NEW: its iova) goes first, as Linux's virtgpu sends it.
    IOReturn status = in.cmd_size ? submitCommand(c, ccmd, in.cmd_size) : kIOReturnSuccess;
    if (status) return status;
    Resource r = {}; r.handle = c->nextHandle++; r.id = resourceId(); r.size = in.size; r.hostVisible = !guest;
    Entry *entries = nullptr; uint32_t entryCount = 0;
    if (guest) {
        // Guest memory the host imports (a pseudo-unprotected VM shares all of its RAM).
        const uint64_t bytes = (in.size + 4095) & ~uint64_t(4095);
        if (user) {
            if ((user & 4095) || bytes != in.size) return kIOReturnBadArgument;
            r.pages = userPages(task, user, bytes);
            if (!r.pages) return kIOReturnVMError;
            r.wired = true;
        } else {
            r.pages = guestPages(bytes);
            if (!r.pages) { droidvm_virtgpu_last_blob_refused = bytes; return kIOReturnNoMemory; }
        }
        entries = IONewData(Entry, MaxBlobEntries);
        if (!entries) { dropPages(r); return kIOReturnNoMemory; }
        for (uint64_t offset = 0; offset < bytes;) {
            IOByteCount length = 0;
            const addr64_t physical = r.pages->getPhysicalSegment(offset, &length, kIOMemoryMapperNone);
            if (!physical || !length) { status = kIOReturnVMError; break; }
            if (length > bytes - offset) length = bytes - offset;
            if (entryCount && entries[entryCount - 1].address + entries[entryCount - 1].length == physical &&
                entries[entryCount - 1].length + length <= 0x80000000u) entries[entryCount - 1].length += uint32_t(length);
            else if (entryCount == MaxBlobEntries) { status = kIOReturnNoResources; droidvm_virtgpu_last_blob_refused = bytes; break; }
            else entries[entryCount++] = {physical, uint32_t(length), 0};
            offset += length;
        }
    }
    if (!status) {
        CreateBlob blob = {{CmdCreateBlob, 0, 0, c->context, 0, {}}, r.id, in.blob_mem, in.blob_flags, entryCount, in.blob_id, in.size};
        Header reply = {};
        status = command(&blob, sizeof blob, entries, entryCount * uint32_t(sizeof(Entry)), &reply, sizeof reply);
    }
    if (entries) IODeleteData(entries, Entry, MaxBlobEntries);
    if (status) { dropPages(r); return status; }
    c->resources[c->resourceCount++] = r;
    out.handle = r.handle; out.resource_id = r.id; count(2);
    return kIOReturnSuccess;
}
IOReturn mapResource(Client *c, uint32_t handle, task_t task, droidvm_virtgpu_map_reply &out) {
    Resource *r = resource(c, handle);
    if (!r) return kIOReturnBadArgument;
    if (!r->map) {
        IOMemoryDescriptor *memory = r->pages;
        if (r->hostVisible) {
            uint64_t base, size;
            if (!pool(base, size)) return kIOReturnUnsupported;
            MapBlob mapBlob = {{CmdMapBlob, 0, 0, c->context, 0, {}}, r->id, 0, 0};
            MapInfo info = {};
            IOReturn status = command(&mapBlob, sizeof mapBlob, nullptr, 0, &info, sizeof info);
            if (status) return status;
            const uint64_t bytes = (r->size + 4095) & ~uint64_t(4095);
            if (info.h.type != RespMapInfo || !(info.mapInfo & MapInfoPool) || info.poolOffset + bytes > size) return kIOReturnUnsupported;
            r->window = IOMemoryDescriptor::withPhysicalAddress(base + info.poolOffset, bytes, kIODirectionInOut);
            if (!r->window) return kIOReturnNoMemory;
            memory = r->window;
        }
        // Write-back: the host's KGSL imports guest pages IO-coherent, and the pool is ordinary shared memory.
        r->map = memory->createMappingInTask(task, 0, IOOptionBits(kIOMapAnywhere) | IOOptionBits(kIOMapCopybackCache));
        if (!r->map) return kIOReturnVMError;
        count(3);
    }
    out.address = r->map->getAddress(); out.size = r->map->getLength();
    return kIOReturnSuccess;
}
IOReturn execbuffer(Client *c, const droidvm_virtgpu_execbuffer &in, const uint8_t *commands, droidvm_virtgpu_execbuffer_reply &out) {
    if (!c->context) return kIOReturnNotReady;
    if (!in.size || (in.size & 3) || in.size > DROIDVM_VIRTGPU_MAX_COMMAND) return kIOReturnBadArgument;
    count(4);
    if (!(in.flags & DROIDVM_VIRTGPU_EXEC_FENCE)) { out.fence = 0; return submitCommand(c, commands, in.size); }
    if (in.ring_idx >= c->rings) return kIOReturnBadArgument;
    for (reapPending(c); c->pendingCount == MaxPending; reapPending(c)) IOSleep(1);
    // Numbered on its ring: the host retires a ring's fences in order, and crosvm returns the command when its fence signals.
    const uint64_t fence = c->nextFence[in.ring_idx] + 1;
    Submit submit = {{CmdSubmit3D, uint32_t(FlagFence) | uint32_t(FlagRingIndex), fence, c->context, uint8_t(in.ring_idx), {}}, in.size, 0};
    uint32_t ticket = 0;
    IOReturn status = post(&submit, sizeof submit, commands, in.size, sizeof(Header), false, &ticket);
    if (status) return status;
    c->nextFence[in.ring_idx] = fence;
    c->pending[c->pendingCount++] = {ticket, in.ring_idx, fence};
    out.fence = fence; count(5);
    return kIOReturnSuccess;
}
IOReturn waitFence(Client *c, const droidvm_virtgpu_wait_fence &in) {
    if (in.ring_idx >= MaxRings) return kIOReturnBadArgument;
    const uint64_t begin = nowUsLocal(), timeoutUs = in.timeout_ns / 1000;
    for (;;) {
        IOLockLock(c->lock);
        reapPending(c);
        const bool done = c->completed[in.ring_idx] >= in.fence || in.fence > c->nextFence[in.ring_idx];
        IOLockUnlock(c->lock);
        if (done) return kIOReturnSuccess;
        if (nowUsLocal() - begin >= timeoutUs) { if (timeoutUs) count(6); return kIOReturnTimeout; }
        IOSleep(1);   // a fence is a GPU job: sleep, so the vCPU goes idle (see pause() in DroidVMVirtioGpu.cpp)
    }
}
IOReturn closeResource(Client *c, uint32_t handle) {
    Resource *r = resource(c, handle);
    if (!r) return kIOReturnBadArgument;
    drain(c, 5000000);   // the GPU may still use it: the context's submissions in flight finish first
    releaseResource(c, *r);
    *r = c->resources[--c->resourceCount];
    return kIOReturnSuccess;
}
}
}
extern "C" IOReturn droidvm_virtgpu_call(void *connection, uint32_t op, task_t task, const void *in, size_t inSize, void *out, size_t outSize) {
    using namespace DroidVMVirtioGpu;
    if (!ready()) return kIOReturnNotReady;
    if (op >= DROIDVM_VIRTGPU_OPS) { count(7); return kIOReturnUnsupported; }
    Client *c = client(connection, true);
    if (!c) return kIOReturnNoResources;
    auto fixed = [&](size_t inBytes, size_t outBytes, bool variableIn = false) {
        return (variableIn ? inSize >= inBytes : inSize == inBytes) && outSize == outBytes && (!inBytes || in) && (!outBytes || out);
    };
    if (op == DROIDVM_VIRTGPU_WAIT_FENCE) {   // sleeps; takes the client's lock only to look
        if (!fixed(sizeof(droidvm_virtgpu_wait_fence), 0)) return kIOReturnBadArgument;
        return waitFence(c, *static_cast<const droidvm_virtgpu_wait_fence *>(in));
    }
    IOReturn status = kIOReturnBadArgument;
    IOLockLock(c->lock);
    switch (op) {
    case DROIDVM_VIRTGPU_GETPARAM:
        if (fixed(sizeof(droidvm_virtgpu_getparam), sizeof(droidvm_virtgpu_getparam_reply)))
            status = getParam(*static_cast<const droidvm_virtgpu_getparam *>(in), *static_cast<droidvm_virtgpu_getparam_reply *>(out));
        break;
    case DROIDVM_VIRTGPU_GET_CAPS:
        if (inSize == sizeof(droidvm_virtgpu_get_caps) && in && out && outSize)
            status = getCaps(*static_cast<const droidvm_virtgpu_get_caps *>(in), out, outSize);
        break;
    case DROIDVM_VIRTGPU_CONTEXT_INIT:
        if (fixed(sizeof(droidvm_virtgpu_context_init), sizeof(droidvm_virtgpu_context_init_reply)))
            status = contextInit(c, *static_cast<const droidvm_virtgpu_context_init *>(in), *static_cast<droidvm_virtgpu_context_init_reply *>(out));
        break;
    case DROIDVM_VIRTGPU_CREATE_BLOB:
        if (fixed(sizeof(droidvm_virtgpu_create_blob), sizeof(droidvm_virtgpu_create_blob_reply), true)) {
            auto blob = static_cast<const droidvm_virtgpu_create_blob *>(in);
            if (blob->cmd_size <= DROIDVM_VIRTGPU_MAX_COMMAND && inSize == sizeof *blob + blob->cmd_size)
                status = createBlob(c, *blob, static_cast<const uint8_t *>(in) + sizeof *blob, *static_cast<droidvm_virtgpu_create_blob_reply *>(out));
        }
        break;
    case DROIDVM_VIRTGPU_CREATE_BLOB_USER:
        if (fixed(sizeof(droidvm_virtgpu_create_blob_user), sizeof(droidvm_virtgpu_create_blob_reply), true)) {
            auto user = static_cast<const droidvm_virtgpu_create_blob_user *>(in);
            const droidvm_virtgpu_create_blob blob = {DROIDVM_VIRTGPU_BLOB_MEM_HOST3D_GUEST, user->blob_flags, user->size, user->blob_id,
                                                      user->cmd_size, 0};
            if (user->cmd_size <= DROIDVM_VIRTGPU_MAX_COMMAND && inSize == sizeof *user + user->cmd_size && user->address)
                status = createBlob(c, blob, static_cast<const uint8_t *>(in) + sizeof *user,
                                    *static_cast<droidvm_virtgpu_create_blob_reply *>(out), task, user->address);
        }
        break;
    case DROIDVM_VIRTGPU_MAP:
        if (fixed(sizeof(droidvm_virtgpu_handle), sizeof(droidvm_virtgpu_map_reply)))
            status = mapResource(c, static_cast<const droidvm_virtgpu_handle *>(in)->handle, task, *static_cast<droidvm_virtgpu_map_reply *>(out));
        break;
    case DROIDVM_VIRTGPU_EXECBUFFER:
        if (fixed(sizeof(droidvm_virtgpu_execbuffer), sizeof(droidvm_virtgpu_execbuffer_reply), true)) {
            auto exec = static_cast<const droidvm_virtgpu_execbuffer *>(in);
            if (inSize == sizeof *exec + exec->size)
                status = execbuffer(c, *exec, static_cast<const uint8_t *>(in) + sizeof *exec, *static_cast<droidvm_virtgpu_execbuffer_reply *>(out));
        }
        break;
    case DROIDVM_VIRTGPU_RESOURCE_INFO:
        if (fixed(sizeof(droidvm_virtgpu_handle), sizeof(droidvm_virtgpu_resource_info_reply))) {
            Resource *r = resource(c, static_cast<const droidvm_virtgpu_handle *>(in)->handle);
            auto reply = static_cast<droidvm_virtgpu_resource_info_reply *>(out);
            if (r) { reply->resource_id = r->id; reply->pad = 0; reply->size = r->size; status = kIOReturnSuccess; }
        }
        break;
    case DROIDVM_VIRTGPU_GEM_CLOSE:
        if (fixed(sizeof(droidvm_virtgpu_handle), 0)) status = closeResource(c, static_cast<const droidvm_virtgpu_handle *>(in)->handle);
        break;
    }
    IOLockUnlock(c->lock);
    if (status) {
        count(7);
        droidvm_virtgpu_last_refusal[0] = op; droidvm_virtgpu_last_refusal[1] = uint32_t(status);
        droidvm_virtgpu_last_refusal[2] = inSize; droidvm_virtgpu_last_refusal[3] = outSize;
        droidvm_virtgpu_refusals_by_op[op] = droidvm_virtgpu_refusals_by_op[op] + 1;
    }
    return status;
}
// The connection closed (or its process ended): its submissions finish, its resources go, its context is destroyed.
extern "C" void droidvm_virtgpu_close(void *connection) {
    using namespace DroidVMVirtioGpu;
    Client *c = client(connection, false);
    if (!c) return;
    IOLockLock(gClientsLock);
    for (uint32_t i = 0; i < MaxClients; ++i) if (gClients[i] == c) gClients[i] = nullptr;
    IOLockUnlock(gClientsLock);
    IOLockLock(c->lock);
    drain(c, 5000000);
    for (uint32_t i = 0; i < c->resourceCount; ++i) releaseResource(c, c->resources[i]);
    if (c->resources) IODelete(c->resources, Resource, c->resourceCapacity);
    if (c->context) {
        Header destroy = {CmdCtxDestroy, 0, 0, c->context, 0, {}}, reply = {};
        (void)command(&destroy, sizeof destroy, nullptr, 0, &reply, sizeof reply);
    }
    IOLockUnlock(c->lock);
    IOLockFree(c->lock);
    IODelete(c, Client, 1);
    droidvm_virtgpu_stats[0] = droidvm_virtgpu_stats[0] - 1;
}

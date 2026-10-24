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

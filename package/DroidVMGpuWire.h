/* SPDX-License-Identifier: MIT */
#ifndef DROIDVM_GPU_WIRE_H
#define DROIDVM_GPU_WIRE_H
#include <stdint.h>

/* The wire between the guest's paravirtual Adreno backend (DroidVMGpu.cpp) and crosvm's droidvm-gpu device
 * (devices/src/virtio/droidvm_gpu.rs), which runs every request against the host's KGSL (/dev/kgsl-3d0).
 *
 * One virtqueue, one request at a time: a chain of a device-readable buffer (header + payload) and a device-writable
 * buffer (response header + payload). The guest's RAM is shared with the host (a pseudo-unprotected Gunyah VM), so a BO
 * is guest memory: IMPORT names guest physical ranges, which crosvm turns into a udmabuf and KGSL imports; BIND places a
 * range of an imported object at a GPU address inside the one VBO the device reserved (INFO va_start/va_size). Command
 * streams carry those GPU addresses, so the guest chooses them (NativeAdrenoDeviceClientIova).
 *
 * SUBMIT queues IBs on the device's single KGSL context with the guest's timestamp (KGSL user-generated timestamps): one
 * in-order timeline, as the native driver's ring. RETIRED reads how far it got; WAIT blocks for at most
 * DROIDVM_GPU_WAIT_MAX_MS. All integers little endian; every struct is naturally aligned and has a fixed size. */
enum {
    DROIDVM_GPU_DEVICE_ID = 59,           /* virtio device id: crosvm's nonstandard ids count down from 63 */
    DROIDVM_GPU_WIRE_VERSION = 3,
    DROIDVM_GPU_MAX_RANGES = 1024,        /* IMPORT entries per request */
    DROIDVM_GPU_MAX_IBS = 16,             /* NativeAdrenoAbiMaxBuffers */
    DROIDVM_GPU_WAIT_MAX_MS = 4,

    DROIDVM_GPU_OP_INFO = 1,
    DROIDVM_GPU_OP_IMPORT = 2,
    DROIDVM_GPU_OP_FREE = 3,
    DROIDVM_GPU_OP_BIND = 4,
    DROIDVM_GPU_OP_SUBMIT = 5,
    DROIDVM_GPU_OP_RETIRED = 6,
    DROIDVM_GPU_OP_WAIT = 7,
    DROIDVM_GPU_OP_TICKS = 8,
    DROIDVM_GPU_OP_CLIENT = 9,            /* a client slot's next user starts with a fresh KGSL context */
    DROIDVM_GPU_MAX_CLIENTS = 32,
    /* Optional OP_INFO query: legacy hosts return their unchanged 64-byte INFO. */
    DROIDVM_GPU_INFO_MAGIC = 0x58504744,   /* little-endian "DGPX" */
    DROIDVM_GPU_INFO_CAPABILITIES = 1,
    DROIDVM_GPU_EXTENSION_VERSION = 1,
    DROIDVM_GPU_CAP_RETIRED_LOST_MASK = 1,

    DROIDVM_GPU_BIND = 1,                 /* KGSL_GPUMEM_RANGE_OP_BIND */
    DROIDVM_GPU_UNBIND = 2,               /* KGSL_GPUMEM_RANGE_OP_UNBIND */
    DROIDVM_GPU_IMPORT_READ_ONLY = 1,     /* the GPU only reads the object */
    DROIDVM_GPU_IMPORT_COHERENT = 2,      /* the GPU snoops the CPU caches (KGSL_MEMFLAGS_IOCOHERENT), as drm2kgsl imports */
    DROIDVM_GPU_IMPORT_CACHED = 4,        /* the guest maps it cacheable (write-back), else write-combined */
};

struct droidvm_gpu_request {
    uint32_t op;
    uint32_t bytes;                       /* payload bytes after this header */
};
struct droidvm_gpu_response {
    int32_t status;                       /* 0 or a negative errno of the host */
    uint32_t bytes;                       /* payload bytes after this header */
};

struct droidvm_gpu_info {                 /* OP_INFO reply */
    uint32_t version;                     /* DROIDVM_GPU_WIRE_VERSION */
    uint32_t chip_id;                     /* as freedreno's device table keys it */
    uint32_t kgsl_chip_id;                /* as KGSL reported it */
    uint32_t gmem_bytes;
    uint64_t gmem_base;
    uint64_t uche_trap_base;
    uint64_t va_start;                    /* the VBO: every BIND iova lies inside */
    uint64_t va_size;
    uint32_t highest_bank_bit;
    uint32_t ubwc_mode;                   /* KGSL_PROP_UBWC_MODE, 0 when absent */
    uint64_t faults;                      /* GPU commands KGSL refused or contexts it invalidated */
};
struct droidvm_gpu_range {
    uint64_t address;                     /* guest physical, 4 KiB aligned */
    uint64_t bytes;                       /* 4 KiB multiple */
};
struct droidvm_gpu_info_query {
    uint32_t magic, kind, version, reserved;
};
struct droidvm_gpu_capabilities {
    uint32_t magic, version, capabilities, max_clients;
};
struct droidvm_gpu_import {               /* OP_IMPORT request, followed by `count` ranges */
    uint32_t count;
    uint32_t flags;                       /* DROIDVM_GPU_IMPORT_* */
};
struct droidvm_gpu_import_reply {
    uint32_t object;                      /* KGSL object id, nonzero */
    uint32_t coherent;                    /* 1: KGSL kept IOCOHERENT (an MMU without IO coherence drops it) */
    uint64_t bytes;                       /* total bytes imported */
};
struct droidvm_gpu_free {                 /* OP_FREE request */
    uint32_t object;
    uint32_t reserved;
};
struct droidvm_gpu_bind {                 /* OP_BIND request */
    uint32_t object;
    uint32_t op;                          /* DROIDVM_GPU_BIND or DROIDVM_GPU_UNBIND */
    uint64_t offset;                      /* inside the object */
    uint64_t iova;                        /* inside [va_start, va_start + va_size) */
    uint64_t bytes;
};
struct droidvm_gpu_ib {
    uint64_t iova;
    uint64_t bytes;
};
struct droidvm_gpu_submit {               /* OP_SUBMIT request, followed by `count` IBs */
    uint32_t timestamp;                   /* nonzero, one more than the last submit's (across the clients) */
    uint16_t count;                       /* 1..DROIDVM_GPU_MAX_IBS */
    uint16_t client;                      /* NativeAdrenoPublish's slot: each has a KGSL context of its own (EDEADLK: lost) */
};
struct droidvm_gpu_client_reset {         /* OP_CLIENT request */
    uint32_t client;                      /* < DROIDVM_GPU_MAX_CLIENTS */
    uint32_t reserved;
};
struct droidvm_gpu_timestamp {            /* OP_RETIRED reply; OP_WAIT request */
    uint32_t timestamp;
    uint32_t timeout_ms;                  /* WAIT: milliseconds; RETIRED with capability: sticky lost-client mask */
};
struct droidvm_gpu_ticks {                /* OP_TICKS reply: the host's 19.2 MHz counter */
    uint64_t ticks;
};

#if defined(__cplusplus)
static_assert(sizeof(droidvm_gpu_info) == 64, "wire info");
static_assert(sizeof(droidvm_gpu_info_query) == 16 && sizeof(droidvm_gpu_capabilities) == 16, "wire extensions");
static_assert(sizeof(droidvm_gpu_range) == 16 && sizeof(droidvm_gpu_import) == 8, "wire import");
static_assert(sizeof(droidvm_gpu_import_reply) == 16 && sizeof(droidvm_gpu_bind) == 32, "wire bind");
static_assert(sizeof(droidvm_gpu_ib) == 16 && sizeof(droidvm_gpu_submit) == 8, "wire submit");
#endif
#endif

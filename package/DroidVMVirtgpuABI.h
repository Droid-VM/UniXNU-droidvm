/* SPDX-License-Identifier: MIT */
#ifndef DROIDVM_VIRTGPU_ABI_H
#define DROIDVM_VIRTGPU_ABI_H
#include <stdint.h>

/* The guest's virtio-gpu 3D interface for user space: what Linux's DRM_IOCTL_VIRTGPU_* give mesa's vdrm, as methods of the
 * accelerator's user client (NativeAdreno's AGXDeviceUserClient, the class the sandbox already admits). A process opens
 * IOAcceleratorES with open type 0x4149 (virtgpu alone: no KGSL proxy slot, granted in sandboxed clients) and calls method
 * DROIDVM_VIRTGPU_METHOD + op through the tunnel ({'NTNL', method} then the input record), with the exact output record. The
 * kernel side is package/DroidVMVirtioGpu3D.cpp.
 *
 * One connection is one DRM context (CONTEXT_INIT once). Handles name its resources; MAP maps one into the caller's task (the
 * msm shmem from the host's drm2kgsl pool, a BO's guest pages) and GEM_CLOSE or the connection's close unmaps and frees it.
 * EXECBUFFER with FENCE numbers the submission on its ring and answers the number; WAIT_FENCE waits for it. All integers little
 * endian; every record has a fixed size, the variable part (a command, a capset) follows it. */
enum {
    DROIDVM_VIRTGPU_METHOD = 0x7200,
    DROIDVM_VIRTGPU_OPEN_TYPE = 0x4149,
    DROIDVM_VIRTGPU_ABI_VERSION = 1,

    DROIDVM_VIRTGPU_GETPARAM = 0,
    DROIDVM_VIRTGPU_GET_CAPS = 1,
    DROIDVM_VIRTGPU_CONTEXT_INIT = 2,
    DROIDVM_VIRTGPU_CREATE_BLOB = 3,
    DROIDVM_VIRTGPU_MAP = 4,
    DROIDVM_VIRTGPU_EXECBUFFER = 5,
    DROIDVM_VIRTGPU_WAIT_FENCE = 6,
    DROIDVM_VIRTGPU_RESOURCE_INFO = 7,
    DROIDVM_VIRTGPU_GEM_CLOSE = 8,
    DROIDVM_VIRTGPU_OPS = 9,

    /* GETPARAM: the VIRTGPU_PARAM_* numbers, and this ABI's version. */
    DROIDVM_VIRTGPU_PARAM_3D_FEATURES = 1,
    DROIDVM_VIRTGPU_PARAM_CAPSET_QUERY_FIX = 2,
    DROIDVM_VIRTGPU_PARAM_RESOURCE_BLOB = 3,
    DROIDVM_VIRTGPU_PARAM_HOST_VISIBLE = 4,
    DROIDVM_VIRTGPU_PARAM_CROSS_DEVICE = 5,
    DROIDVM_VIRTGPU_PARAM_CONTEXT_INIT = 6,
    DROIDVM_VIRTGPU_PARAM_SUPPORTED_CAPSET_IDS = 7,
    DROIDVM_VIRTGPU_PARAM_CREATE_GUEST_HANDLE = 9,
    DROIDVM_VIRTGPU_PARAM_ABI_VERSION = 0x100,

    DROIDVM_VIRTGPU_BLOB_MEM_HOST3D = 2,
    DROIDVM_VIRTGPU_BLOB_MEM_HOST3D_GUEST = 3,
    DROIDVM_VIRTGPU_BLOB_FLAG_MAPPABLE = 1,
    DROIDVM_VIRTGPU_BLOB_FLAG_CREATE_GUEST_HANDLE = 8,

    DROIDVM_VIRTGPU_EXEC_FENCE = 1,          /* number the submission on its ring and answer the number */
    DROIDVM_VIRTGPU_MAX_COMMAND = 65536,     /* a command (EXECBUFFER, CREATE_BLOB's ccmd) */
};

struct droidvm_virtgpu_getparam { uint64_t param; };
struct droidvm_virtgpu_getparam_reply { uint64_t value; };
struct droidvm_virtgpu_get_caps { uint32_t capset_id, version; };            /* reply: the capset, up to the output size */
struct droidvm_virtgpu_context_init { uint32_t capset_id, num_rings; };
struct droidvm_virtgpu_context_init_reply { uint32_t context_id, pad; };
struct droidvm_virtgpu_create_blob {                                          /* followed by cmd_size bytes of ccmd */
    uint32_t blob_mem, blob_flags;
    uint64_t size, blob_id;
    uint32_t cmd_size, pad;
};
struct droidvm_virtgpu_create_blob_reply { uint32_t handle, resource_id; };
struct droidvm_virtgpu_handle { uint32_t handle, pad; };                      /* MAP, RESOURCE_INFO, GEM_CLOSE */
struct droidvm_virtgpu_map_reply { uint64_t address, size; };
struct droidvm_virtgpu_execbuffer { uint32_t flags, ring_idx, size, pad; };   /* followed by size bytes of commands */
struct droidvm_virtgpu_execbuffer_reply { uint64_t fence; };
struct droidvm_virtgpu_wait_fence { uint32_t ring_idx, pad; uint64_t fence, timeout_ns; };
struct droidvm_virtgpu_resource_info_reply { uint32_t resource_id, pad; uint64_t size; };
#endif

// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The disks as a kext of the kernel collection (ipados27/kexts/DroidVMStorage.json): what the storage kext hands the kernel as it
// starts (DroidVMSourceStorage.cpp), and the kernel side that answers the XNU changes with it (DroidVMStorageHooks.c).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct droidvm_storage_ops {
    // NativeVirtioBlock: is the device one of the verified disks; which device holds the volume with this UUID
    int (*root_authorized)(dev_t device);
    int (*volume_device)(const char *uuid, dev_t *device, char *path, size_t size);
    // and the SHA-384 of the verified manifest of that disk's volume (the owner boot manifest, IOKitBSDInit.cpp)
    int (*manifest_sha384)(dev_t device, const char *uuid, uint8_t digest[48]);
    // NativeRamBlock: the Data seed's volume; the GPU add-on image
    int (*volume_seed)(const char *uuid, dev_t *device, char *path, size_t size, uint64_t *, uint64_t *, uint8_t *);
    int (*gpu_addon)(const uint8_t **data, uint64_t *bytes);
};

// Once, as the storage kext starts, before it adds the disk drivers: 1 if these are the kernel's answers from now on.
int droidvm_storage_register(const struct droidvm_storage_ops *ops);

#ifdef __cplusplus
}
#endif

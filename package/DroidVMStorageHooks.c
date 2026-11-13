// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The kernel's side of the disks when they are a kext of the collection (ipados27/kexts/DroidVMStorage.json). The XNU changes call
// these names: the root mount and the owner chain ask whether a device is one of the verified disks and which device holds a
// volume (bsd_init.c, kern_exec.c, vm_unix.c), the owner boot manifest the digests of their manifests (IOKitBSDInit.cpp), the Data mount asks for the Data seed and the launch policy for the GPU add-on
// (kern_exec.c). Built in, the storage archive defines them; as a kext, the kext registers its functions once as it starts,
// before it adds the disk drivers (DroidVMSourceStorage.cpp), and these answer with them. Until then every answer is no: no
// device is authorized, no volume is found -- what the built-in drivers answer before their disks have started.
//
// build-droidvm-source-storage.py links this object alone in the storage archive when the disks are a kext, so the XNU changes
// and the kernel configuration stay as they are.
#include "DroidVMStorageHooks.h"

int native_verified_block_root_authorized(dev_t device);
int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size);
int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48]);
int native_ram_block_volume_seed(const char *uuid, dev_t *device, char *path, size_t size, uint64_t *a, uint64_t *b, uint8_t *c);
int native_ram_block_gpu_addon(const uint8_t **data, uint64_t *bytes);
int sheng_boot_watchdog_policy(void *process, uint64_t scratch);
void native_source_storage_start(void);

static const struct droidvm_storage_ops *registered;

int droidvm_storage_register(const struct droidvm_storage_ops *ops)
{
    const struct droidvm_storage_ops *none = NULL;
    if (!ops || !ops->root_authorized || !ops->volume_device || !ops->manifest_sha384 || !ops->volume_seed || !ops->gpu_addon)
        return 0;
    return __atomic_compare_exchange_n(&registered, &none, ops, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

static const struct droidvm_storage_ops *storage(void)
{
    return __atomic_load_n(&registered, __ATOMIC_ACQUIRE);
}

int native_verified_block_root_authorized(dev_t device)
{
    const struct droidvm_storage_ops *ops = storage();
    return ops ? ops->root_authorized(device) : 0;
}

int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size)
{
    const struct droidvm_storage_ops *ops = storage();
    return ops ? ops->volume_device(uuid, device, path, size) : 0;
}

int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48])
{
    const struct droidvm_storage_ops *ops = storage();
    return ops ? ops->manifest_sha384(device, uuid, digest) : 0;
}

int native_ram_block_volume_seed(const char *uuid, dev_t *device, char *path, size_t size, uint64_t *a, uint64_t *b, uint8_t *c)
{
    const struct droidvm_storage_ops *ops = storage();
    return ops ? ops->volume_seed(uuid, device, path, size, a, b, c) : 0;
}

int native_ram_block_gpu_addon(const uint8_t **data, uint64_t *bytes)
{
    const struct droidvm_storage_ops *ops = storage();
    return ops ? ops->gpu_addon(data, bytes) : 0;
}

// sheng's launchd job policy after the Data mount: nothing to do on this board (DroidVMSourceStorage.cpp's default, built in).
int sheng_boot_watchdog_policy(void *process, uint64_t scratch)
{
    (void)process;
    (void)scratch;
    return 0;
}

// bsd_init's hook after vfsinit: the kext starts the disks itself, once IOBSD is published; the root mount waits for them.
void native_source_storage_start(void)
{
}

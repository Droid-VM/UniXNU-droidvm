// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The disks of the iPadOS 27 board (ipados27/, xnu-12377, arm64e). Its kernel collection binds no class of the original
// IOStorageFamily: the public IOBlockStorageDevice, built from IOStorageFamily's source (ipados27/scripts/
// build-droidvm-source-storage.py), registers below the original class once the original family has loaded, so the original
// storage driver, partition schemes and APFS take the devices of the source-built drivers. Then come the drivers of every DroidVM
// guest's disks, matched through the IOCatalogue with the personalities the iPadOS 17 kernel has built in (patches/xnu
// native-aes-personality, XNU_NATIVE_VIRTIO_BLOCK and XNU_NATIVE_RAM_BLOCK): NativeVirtioBlock on each virtio-mmio disk (the
// verified OS and SystemOS images; DroidVMDataStore.c drives the Data store's), NativeRamBlock with the Data seed once BSD is up.
// After upstream sheng's package/SourceStorage.cpp, which starts its UFS host this way.
//
// The bsd_init hook (ipados27/patches/xnu-12377/source-storage-start.patch) calls native_source_storage_start after vfsinit;
// native-source-storage=1 (package/DroidVMBootArgs.c) enables it. Every failure panics: without its disks the guest has no root.
//
// As a kext of the collection (ipados27/kexts/DroidVMStorage.json, compiled with DROIDVM_NATIVE_KEXT=1): OSKext loads
// IOStorageFamily first (a library of the kext) and registers the classes as the kext loads; the kext's starter calls
// native_source_storage_start once IOBSD is published, and the root mount waits for the disks. It hands the kernel the functions
// the XNU changes ask (DroidVMStorageHooks.c) before it adds the disk drivers.
#include <IOKit/IOService.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSUnserialize.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

#if !DROIDVM_NATIVE_KEXT
extern kmod_info_t g_kernel_kmod_info;
// The static initializers of the source-built classes: their objects lose __mod_init_func when the archive is built, so the
// classes register here, between preModLoad and postModLoad, and not at kernel start before the original family exists.
extern "C" void source_storage_base_init(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void source_storage_virtio_init(void) __asm__("__GLOBAL__sub_I_NativeVirtioBlock.cpp");
extern "C" void source_storage_ram_init(void) __asm__("__GLOBAL__sub_I_NativeRamBlock.cpp");
#else
#include "DroidVMStorageHooks.h"
extern "C" int native_verified_block_root_authorized(dev_t);
extern "C" int native_verified_block_volume_device(const char *, dev_t *, char *, size_t);
extern "C" int native_verified_block_manifest_sha384(dev_t, const char *, uint8_t[48]);
extern "C" int native_ram_block_volume_seed(const char *, dev_t *, char *, size_t, uint64_t *, uint64_t *, uint8_t *);
extern "C" int native_ram_block_gpu_addon(const uint8_t **, uint64_t *);
// The kext's own disks answer the kernel (these names resolve inside the kext first; the kernel's are DroidVMStorageHooks.c's).
static const droidvm_storage_ops kStorageOps = {
    native_verified_block_root_authorized, native_verified_block_volume_device, native_verified_block_manifest_sha384,
    native_ram_block_volume_seed, native_ram_block_gpu_addon,
};
#endif
extern "C" bool native_source_root_policies_load(void);

// The arm64e kernel's static link leaves no weak import undefined (the iPadOS 17 arm64 link resolved them to 0), so the hooks of
// sheng's components this board does not have get their defaults here; a real definition wins. NativeVirtioBlock asks NativeUfs
// whether a provider is one of its windows: -1, none is. The kernel runs sheng's launchd job policy after the Data mount: nothing
// to do here (this board's launch policy is DroidVMLaunchPolicy.c, through native-board-launch-policy).
extern "C" __attribute__((weak, visibility("hidden"))) int native_ufs_block_authenticates(IOService *, const char *, uint8_t *)
{
    return -1;
}
extern "C" __attribute__((weak, visibility("hidden"))) int sheng_boot_watchdog_policy(void *, uint64_t)
{
    return 0;
}

static const OSMetaClass *storageParent;
static bool attempted;

// Only the source IOBlockStorageDevice's metaclass constructor asks, while its class registers: the original class is its parent.
extern "C" const OSMetaClass *native_source_storage_parent(void)
{
#if DROIDVM_NATIVE_KEXT
    // As the kext loads, after its library IOStorageFamily: the original class is registered by now.
    if (!storageParent) {
        const OSSymbol *name = OSSymbol::withCString("IOBlockStorageDevice");
        storageParent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
        OSSafeReleaseNULL(name);
        if (storageParent && storageParent->getClassSize() != sizeof(IOBlockStorageDevice))
            panic("DroidVMSourceStorage: original IOBlockStorageDevice is %u bytes, the source one %lu",
                  storageParent->getClassSize(), sizeof(IOBlockStorageDevice));
    }
#endif
    if (!storageParent) panic("DroidVMSourceStorage: metaclass initialized before the original family");
    return storageParent;
}

// The built-in personalities of the iPadOS 17 kernel, in KernelConfigTables' syntax; as a kext, they name it, so matching
// finds the classes' kext loaded.
#if DROIDVM_NATIVE_KEXT
#define STORAGE_BUNDLE "    'CFBundleIdentifier' = \"com.droidvm.driver.DroidVMStorage\";"
#else
#define STORAGE_BUNDLE
#endif
static const char kPersonalities[] =
    "("
    "  {"
    "    'IOClass'         = NativeVirtioBlock;"
    "    'IOProviderClass' = IOPlatformDevice;"
    "    'IOProbeScore'    = 1000:32;"
    STORAGE_BUNDLE
    "  },"
    "  {"
    "    'IOClass'         = NativeRamBlock;"
    "    'IOMatchCategory' = NativeRamBlock;"
    "    'IOProviderClass' = IOResources;"
    "    'IOResourceMatch' = IOBSD;"
    "    'IOProbeScore'    = 1000:32;"
    STORAGE_BUNDLE
    "  }"
    ")";

extern "C" void native_source_storage_start(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("native-source-storage", &enabled, sizeof(enabled)) || enabled != 1 || attempted) return;
    attempted = true;
    OSReturn status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
    if (status != kOSReturnSuccess) panic("DroidVMSourceStorage: IOStorageFamily load failed %x", status);
    const OSSymbol *name = OSSymbol::withCString("IOBlockStorageDevice");
    storageParent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!storageParent || storageParent->getClassSize() != sizeof(IOBlockStorageDevice))
        panic("DroidVMSourceStorage: original IOBlockStorageDevice is %u bytes, the source one %lu",
              storageParent ? storageParent->getClassSize() : 0, sizeof(IOBlockStorageDevice));
    // The root policies (the firmware's RootPolicies handoff, signed by the owner key compiled in) before any disk is read.
    if (!native_source_root_policies_load()) panic("DroidVMSourceStorage: root policy handoff or authentication failed");
    status = OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs");
    if (status != kOSReturnSuccess) panic("DroidVMSourceStorage: APFS load failed %x", status);

#if !DROIDVM_NATIVE_KEXT
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("DroidVMSourceStorage: cannot begin class registration");
    source_storage_base_init();
    source_storage_virtio_init();
    source_storage_ram_init();
    status = OSMetaClass::postModLoad(handle);
    if (status != kOSReturnSuccess) panic("DroidVMSourceStorage: class registration failed %x", status);
#else
    // The classes registered as the kext loaded; the kernel asks the kext's disks from now on.
    if (!droidvm_storage_register(&kStorageOps)) panic("DroidVMSourceStorage: the kernel refused the storage functions");
#endif

    OSString *error = nullptr;
    OSArray *personalities = OSDynamicCast(OSArray, OSUnserialize(kPersonalities, &error));
    if (!personalities)
        panic("DroidVMSourceStorage: personalities: %s", error ? error->getCStringNoCopy() : "not an array");
    OSSafeReleaseNULL(error);
    // Matching starts at once on the platform devices already published (the virtio-mmio disks) and on IOResources.
    if (!gIOCatalogue->addDrivers(personalities, true)) panic("DroidVMSourceStorage: the disk drivers were not added");
    personalities->release();
    printf("DroidVMSourceStorage: source IOBlockStorageDevice registered, virtio disks and Data seed matching\n");
}

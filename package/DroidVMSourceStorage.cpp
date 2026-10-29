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
#include <IOKit/IOService.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSUnserialize.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
// The static initializers of the source-built classes: their objects lose __mod_init_func when the archive is built, so the
// classes register here, between preModLoad and postModLoad, and not at kernel start before the original family exists.
extern "C" void source_storage_base_init(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void source_storage_virtio_init(void) __asm__("__GLOBAL__sub_I_NativeVirtioBlock.cpp");
extern "C" void source_storage_ram_init(void) __asm__("__GLOBAL__sub_I_NativeRamBlock.cpp");
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
    if (!storageParent) panic("DroidVMSourceStorage: metaclass initialized before the original family");
    return storageParent;
}

// The built-in personalities of the iPadOS 17 kernel, in KernelConfigTables' syntax.
static const char kPersonalities[] =
    "("
    "  {"
    "    'IOClass'         = NativeVirtioBlock;"
    "    'IOProviderClass' = IOPlatformDevice;"
    "    'IOProbeScore'    = 1000:32;"
    "  },"
    "  {"
    "    'IOClass'         = NativeRamBlock;"
    "    'IOMatchCategory' = NativeRamBlock;"
    "    'IOProviderClass' = IOResources;"
    "    'IOResourceMatch' = IOBSD;"
    "    'IOProbeScore'    = 1000:32;"
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

    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("DroidVMSourceStorage: cannot begin class registration");
    source_storage_base_init();
    source_storage_virtio_init();
    source_storage_ram_init();
    status = OSMetaClass::postModLoad(handle);
    if (status != kOSReturnSuccess) panic("DroidVMSourceStorage: class registration failed %x", status);

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
